#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/display/display_buffer.h"
#include "esphome/components/spi/spi.h"

namespace esphome {
namespace inkplate_spi {

/// Enum for Inkplate SPI models
enum InkplateSPIModel : uint8_t {
  INKPLATE_2 = 0,
  // Future contributors can add models here:
  // Example: INKPLATE_4 = 1,
  // Example: INKPLATE_6_COLOR = 2,
  // Example: INKPLATE_7 = 3,
};

/** Component for SPI-based Inkplate e-paper displays.
 *
 * Supports Inkplate 2 (and future SPI Inkplate models).
 * These displays use SPI communication unlike the parallel GPIO-based
 * Inkplate 5/6/10 models which use the separate 'inkplate' component.
 */
class InkplateSPI : public display::DisplayBuffer,
                    public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW, spi::CLOCK_PHASE_LEADING,
                                          spi::DATA_RATE_1MHZ> {
 public:
  /** Set the Data/Command pin. */
  void set_dc_pin(GPIOPin *dc_pin) { this->dc_pin_ = dc_pin; }

  /** Set the Reset pin (optional). */
  void set_reset_pin(GPIOPin *reset_pin) { this->reset_pin_ = reset_pin; }

  /** Set the Busy pin (optional). */
  void set_busy_pin(GPIOPin *busy_pin) { this->busy_pin_ = busy_pin; }

  /** Set the Inkplate model. */
  void set_model(InkplateSPIModel model) { this->model_ = model; }

  // Component lifecycle methods
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override;

  /** Get the display type (supports color). */
  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_COLOR; }

 protected:
  /** Draw a single pixel at the specified coordinates with the given color. */
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  /** Get the display width based on the model. */
  int get_width_internal() override;

  /** Get the display height based on the model. */
  int get_height_internal() override;

  /** Calculate buffer size based on model specifications. */
  size_t get_buffer_length_();

  // Hardware control methods

  /** Initialize the display hardware. */
  void init_display_();

  /** Write the buffer contents to the display. */
  void write_display_();

  /** Hardware reset the display. */
  void reset_();

  /** Enter deep sleep mode. */
  void deep_sleep_();

  /** Write a command byte to the display. */
  void command_(uint8_t cmd);

  /** Write a data byte to the display. */
  void data_(uint8_t value);

  /** Write multiple data bytes to the display. */
  void data_(const uint8_t *data, size_t len);

  /** Wait until the busy pin goes low (display ready). */
  bool wait_until_idle_(uint32_t timeout_ms);

  GPIOPin *dc_pin_{nullptr};
  GPIOPin *reset_pin_{nullptr};
  GPIOPin *busy_pin_{nullptr};
  InkplateSPIModel model_{INKPLATE_2};

  // Frame buffer
  uint8_t *buffer_{nullptr};
  size_t buffer_size_{0};
};

}  // namespace inkplate_spi
}  // namespace esphome
