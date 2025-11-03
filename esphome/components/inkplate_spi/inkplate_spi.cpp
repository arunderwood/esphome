#include "inkplate_spi.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <cstring>

namespace esphome {
namespace inkplate_spi {

static const char *const TAG = "inkplate_spi";

// Display controller commands
static constexpr uint8_t CMD_PANEL_SETTING = 0x00;
static constexpr uint8_t CMD_POWER_OFF = 0x02;
static constexpr uint8_t CMD_POWER_ON = 0x04;
static constexpr uint8_t CMD_DEEP_SLEEP = 0x07;
static constexpr uint8_t CMD_DATA_START_TRANSMISSION = 0x10;
static constexpr uint8_t CMD_DATA_STOP = 0x11;
static constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;
static constexpr uint8_t CMD_DATA_START_TRANSMISSION_2 = 0x13;
static constexpr uint8_t CMD_VCOM_AND_DATA_INTERVAL = 0x50;
static constexpr uint8_t CMD_RESOLUTION_SETTING = 0x61;

void InkplateSPI::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Inkplate SPI...");

  // Setup pins
  this->dc_pin_->setup();
  this->dc_pin_->digital_write(false);

  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();
  }

  if (this->busy_pin_ != nullptr) {
    this->busy_pin_->setup();
    // Arduino library uses INPUT_PULLUP for busy pin
    this->busy_pin_->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
  }

  // Initialize SPI
  this->spi_setup();

  // Allocate display buffer
  this->buffer_size_ = this->get_buffer_length_();

  // Try PSRAM first, fall back to regular malloc
  this->buffer_ = (uint8_t *) ps_malloc(this->buffer_size_);
  if (this->buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate buffer with ps_malloc, trying malloc");
    this->buffer_ = (uint8_t *) malloc(this->buffer_size_);  // NOLINT
  }

  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate %d bytes for display buffer", this->buffer_size_);
    this->mark_failed();
    return;
  }

  // Initialize buffers
  // Hardware encoding: B&W: 0=white, 1=black; Red: 0=red, 1=no red
  size_t bw_buffer_size = (this->get_width_internal() * this->get_height_internal()) / 8;
  // B&W buffer: all 0s (white background)
  memset(this->buffer_, 0x00, bw_buffer_size);
  // Red buffer: all 1s (no red)
  memset(this->buffer_ + bw_buffer_size, 0xFF, bw_buffer_size);

  // Initialize display hardware
  this->reset_();
  this->init_display_();
}

float InkplateSPI::get_setup_priority() const { return setup_priority::PROCESSOR; }

void InkplateSPI::update() {
  // Trigger display update through the normal display flow
  this->do_update_();
  // Send buffer to hardware
  this->write_display_();
}

void InkplateSPI::dump_config() {
  LOG_DISPLAY("", "Inkplate SPI", this);

  switch (this->model_) {
    case INKPLATE_2:
      ESP_LOGCONFIG(TAG, "  Model: Inkplate 2");
      ESP_LOGCONFIG(TAG, "  Resolution: 104x212");
      ESP_LOGCONFIG(TAG, "  Colors: 3 (Black/White/Red)");
      break;
    // Future models: add dump_config cases here
    default:
      ESP_LOGCONFIG(TAG, "  Model: Unknown");
      break;
  }

  LOG_PIN("  DC Pin: ", this->dc_pin_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_PIN("  Busy Pin: ", this->busy_pin_);
  LOG_UPDATE_INTERVAL(this);
}

int InkplateSPI::get_width_internal() {
  switch (this->model_) {
    case INKPLATE_2:
      return 104;
    // Future models: add width cases here
    // case INKPLATE_4:
    //   return 400;
    default:
      return 0;
  }
}

int InkplateSPI::get_height_internal() {
  switch (this->model_) {
    case INKPLATE_2:
      return 212;
    // Future models: add height cases here
    // case INKPLATE_4:
    //   return 300;
    default:
      return 0;
  }
}

size_t InkplateSPI::get_buffer_length_() {
  // Inkplate 2: Split buffer format (1 bit per pixel for each half)
  // First half: B&W pixels, Second half: Red pixels
  // Future models may use different formats (e.g., 3 bits for 7-color)
  int width = this->get_width_internal();
  int height = this->get_height_internal();

  switch (this->model_) {
    case INKPLATE_2:
      // Split buffer: (width * height / 8) * 2
      // First half for B&W, second half for red
      return (width * height / 8) * 2;
    // Future models with different color modes:
    // case INKPLATE_6_COLOR:
    //   // 3 bits per pixel for 7-color
    //   return (width * height * 3) / 8;
    default:
      return 0;
  }
}

void InkplateSPI::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= this->get_width_internal() || y < 0 || y >= this->get_height_internal()) {
    return;
  }

  int width = this->get_width_internal();
  int bw_buffer_size = (width * this->get_height_internal()) / 8;

  // Calculate bit position
  int pixel_index = y * width + x;
  int byte_index = pixel_index / 8;
  int bit_position = 7 - (pixel_index % 8);  // MSB first
  uint8_t bit_mask = 1 << bit_position;

  // Determine color
  bool is_red = (color.r > 200 && color.g < 100 && color.b < 100);
  bool is_black = !color.is_on();

  // Update B&W buffer (first half)
  // Hardware encoding: 0=white, 1=black
  if (is_black) {
    // Black: set bit to 1
    this->buffer_[byte_index] |= bit_mask;
  } else {
    // White or red: clear bit to 0
    this->buffer_[byte_index] &= ~bit_mask;
  }

  // Update Red buffer (second half)
  // Hardware encoding: 0=red, 1=no red (Arduino: buffer &= ~bit for red)
  if (is_red) {
    // Red: clear bit to 0
    this->buffer_[bw_buffer_size + byte_index] &= ~bit_mask;
  } else {
    // No red: set bit to 1
    this->buffer_[bw_buffer_size + byte_index] |= bit_mask;
  }
}

void InkplateSPI::reset_() {
  if (this->reset_pin_ == nullptr) {
    return;
  }

  this->reset_pin_->digital_write(false);
  delay(100);  // NOLINT
  this->reset_pin_->digital_write(true);
  delay(100);  // NOLINT
}

void InkplateSPI::init_display_() {
  // Power on display FIRST (Arduino library order)
  // This must happen before panel configuration
  this->command_(CMD_POWER_ON);
  this->wait_until_idle_(60000);

  // Model-specific initialization sequences
  switch (this->model_) {
    case INKPLATE_2:
      // Inkplate 2 initialization sequence
      // Based on SolderedElectronics Arduino library

      // Panel setting: LUT from OTP, scan up, shift right, booster on
      this->command_(CMD_PANEL_SETTING);
      this->data_(0x0F);
      this->data_(0x89);

      // Set resolution: 104x212
      this->command_(CMD_RESOLUTION_SETTING);
      this->data_(0x68);  // Width (104)
      this->data_(0x00);  // Height high byte (212 >> 8)
      this->data_(0xD4);  // Height low byte (212 & 0xFF)

      // VCOM and data interval setting
      this->command_(CMD_VCOM_AND_DATA_INTERVAL);
      this->data_(0x77);
      break;

    default:
      ESP_LOGW(TAG, "Unknown model, using default initialization");
      break;
  }
}

void InkplateSPI::write_display_() {
  // Initialize display (includes power on)
  this->init_display_();

  // Wait for panel to be ready (Arduino library uses 20ms delay)
  delay(20);

  // Calculate buffer sizes
  size_t bw_buffer_size = (this->get_width_internal() * this->get_height_internal()) / 8;

  // Stage 1: Send B&W pixel data (command 0x10)
  // Send buffer as-is: 1=black, 0=white
  this->command_(CMD_DATA_START_TRANSMISSION);

  this->enable();
  this->dc_pin_->digital_write(true);
  for (size_t i = 0; i < bw_buffer_size; i++) {
    this->write_byte(this->buffer_[i]);  // Send buffer as-is
  }
  this->disable();

  // Stage 2: Send red pixel data (command 0x13)
  // Send buffer as-is: 1=no red, 0=red
  this->command_(CMD_DATA_START_TRANSMISSION_2);

  this->enable();
  this->dc_pin_->digital_write(true);
  for (size_t i = 0; i < bw_buffer_size; i++) {
    this->write_byte(this->buffer_[bw_buffer_size + i]);  // Send buffer as-is
  }
  this->disable();

  // End data transfer (command 0x11)
  this->command_(CMD_DATA_STOP);
  this->data_(0x00);

  // Trigger display refresh (command 0x12)
  this->command_(CMD_DISPLAY_REFRESH);

  // Arduino library adds 500μs delay before waiting for busy pin
  delayMicroseconds(500);

  this->wait_until_idle_(60000);

  // Power off (enter deep sleep)
  this->deep_sleep_();
}

void InkplateSPI::command_(uint8_t cmd) {
  // Set DC low for command
  this->dc_pin_->digital_write(false);
  this->enable();
  this->write_byte(cmd);
  this->disable();
}

void InkplateSPI::data_(uint8_t value) {
  // Set DC high for data
  this->dc_pin_->digital_write(true);
  this->enable();
  this->write_byte(value);
  this->disable();
}

void InkplateSPI::data_(const uint8_t *data, size_t len) {
  // Set DC high for data
  this->dc_pin_->digital_write(true);
  this->enable();
  this->write_array(data, len);
  this->disable();
}

bool InkplateSPI::wait_until_idle_(uint32_t timeout_ms) {
  if (this->busy_pin_ == nullptr) {
    // No busy pin, use fixed delay
    delay(200);  // NOLINT
    return true;
  }

  const uint32_t start = millis();

  // Wait while busy pin is LOW (Arduino library waits while pin reads low)
  // Use tight polling loop like Arduino - no delays between reads
  while (!this->busy_pin_->digital_read() && (millis() - start < timeout_ms)) {
    App.feed_wdt();
  }

  if (millis() - start >= timeout_ms) {
    ESP_LOGE(TAG, "Timeout waiting for busy pin");
    return false;
  }

  // Arduino library adds 200ms delay after busy pin goes high
  delay(200);  // NOLINT
  return true;
}

void InkplateSPI::deep_sleep_() {
  // VCOM setting for power off
  this->command_(CMD_VCOM_AND_DATA_INTERVAL);
  this->data_(0xF7);

  // Power off
  this->command_(CMD_POWER_OFF);
  this->wait_until_idle_(60000);

  // Enter deep sleep mode
  this->command_(CMD_DEEP_SLEEP);
  this->data_(0xA5);  // Check code for deep sleep
}

}  // namespace inkplate_spi
}  // namespace esphome
