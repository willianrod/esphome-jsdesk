#pragma once

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace jsdesk {

// JSDRIVE / JSM desk controller (DCU_G-PRT5G style "P R T 5 G" bus).
//
// Panel -> box, every ~16 ms:           A5 00 <buttons> <0xFF - buttons> FF
// Box -> panel, reply:                  5A <seg1> <seg2> <seg3> [status] <checksum>
// The box reply is the panel's 7-segment display content; some boxes add a
// status byte, making the frame 6 bytes. The length is auto-detected.
// Which of R/T is the box's input differs between boxes: on the Q200-23,
// R is the box's input (panel -> box) and T its output (box -> panel).
//
// Sniff mode (no box_tx_uart): listen only, nothing is driven.
// Passthrough mode (box_tx_uart set): the R line and KEY1 are cut and routed
// through the ESP, which forwards the panel and substitutes its own frames
// while it is moving the desk. Pressing any panel button cancels ESP control.
// Standalone mode (box_tx_uart, no panel_uart): the ESP replaces the panel and
// polls the box itself. Whenever the panel is quiet the ESP polls in its place.

enum JSDeskCommand : uint8_t {
  JSDESK_IDLE = 0,
  JSDESK_UP,
  JSDESK_DOWN,
  JSDESK_TARGET,
  JSDESK_PRESET,
  JSDESK_PRESS,
};

class JSDesk : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_panel_uart(uart::UARTComponent *uart) { this->panel_uart_ = uart; }
  void set_box_uart(uart::UARTComponent *uart) { this->box_uart_ = uart; }
  void set_box_tx_uart(uart::UARTComponent *uart) { this->box_tx_uart_ = uart; }
  void set_key_in_pin(GPIOPin *pin) { this->key_in_pin_ = pin; }
  void set_key_out_pin(GPIOPin *pin) { this->key_out_pin_ = pin; }
  void set_key2_in_pin(GPIOPin *pin) { this->key2_in_pin_ = pin; }
  void set_min_height(float height) { this->min_height_ = height; }
  void set_max_height(float height) { this->max_height_ = height; }
  void set_stop_tolerance(float tolerance) { this->stop_tolerance_ = tolerance; }
  void set_key_pulse(uint32_t ms) { this->key_pulse_ms_ = ms; }
  void set_key_on_move(bool key_on_move) { this->key_on_move_ = key_on_move; }
  void set_log_frames(bool log_frames) { this->log_frames_ = log_frames; }
  void set_height_sensor(sensor::Sensor *sensor) { this->height_sensor_ = sensor; }
  void set_display_sensor(text_sensor::TextSensor *sensor) { this->display_sensor_ = sensor; }
  void set_key1_sensor(binary_sensor::BinarySensor *sensor) { this->key1_sensor_ = sensor; }
  void set_key2_sensor(binary_sensor::BinarySensor *sensor) { this->key2_sensor_ = sensor; }
  void add_button_sensor(uint8_t mask, binary_sensor::BinarySensor *sensor) {
    this->button_sensors_.emplace_back(mask, sensor);
  }

  void move_up();
  void move_down();
  void move_to(float height);
  void goto_preset(int preset);
  void stop();
  // Hold a raw button code for a fixed time (protocol exploration). KEY1 is not touched.
  void press(uint8_t button, uint32_t ms);

  bool can_control() const { return this->box_tx_uart_ != nullptr; }
  bool is_moving() const { return this->command_ != JSDESK_IDLE; }
  bool is_locked() const { return this->locked_; }
  // 1 = raising, -1 = lowering, 0 = idle/unknown
  int direction() const;
  float get_height() const { return this->height_; }
  float get_min_height() const { return this->min_height_; }
  float get_max_height() const { return this->max_height_; }

 protected:
  void read_box_(uint32_t now);
  void read_panel_(uint32_t now);
  void read_keys_(uint32_t now);
  void handle_box_frame_(const uint8_t *frame, size_t len, uint32_t now);
  void handle_panel_frame_(const uint8_t *frame, uint32_t now);
  void update_command_(uint32_t now);
  void update_key_out_(uint32_t now);
  void send_command_frame_(uint32_t now);
  void begin_(JSDeskCommand command, uint8_t button);
  bool aim_target_();

  uart::UARTComponent *panel_uart_{nullptr};
  uart::UARTComponent *box_uart_{nullptr};
  uart::UARTComponent *box_tx_uart_{nullptr};
  GPIOPin *key_in_pin_{nullptr};
  GPIOPin *key_out_pin_{nullptr};
  GPIOPin *key2_in_pin_{nullptr};
  float min_height_{60.0f};
  float max_height_{125.0f};
  float stop_tolerance_{0.5f};
  uint32_t key_pulse_ms_{0};
  bool key_on_move_{false};
  bool log_frames_{false};

  sensor::Sensor *height_sensor_{nullptr};
  text_sensor::TextSensor *display_sensor_{nullptr};
  binary_sensor::BinarySensor *key1_sensor_{nullptr};
  binary_sensor::BinarySensor *key2_sensor_{nullptr};
  std::vector<std::pair<uint8_t, binary_sensor::BinarySensor *>> button_sensors_;

  HighFrequencyLoopRequester high_freq_;

  std::vector<uint8_t> box_buf_;
  std::vector<uint8_t> panel_buf_;
  std::vector<uint8_t> last_box_frame_;
  uint8_t last_panel_frame_[5]{};
  uint8_t box_len_{0};  // 0 = not yet detected, 5 or 6
  uint32_t box_bad_streak_{0};

  float height_{NAN};
  int last_delta_{0};
  uint32_t last_height_change_ms_{0};
  uint32_t last_box_ms_{0};
  uint32_t last_panel_ms_{0};

  bool key_in_state_{false};
  bool key2_in_state_{false};
  bool key_out_state_{false};
  uint32_t key_in_changed_ms_{0};
  uint32_t key2_in_changed_ms_{0};

  JSDeskCommand command_{JSDESK_IDLE};
  uint8_t command_button_{0};
  float target_{NAN};
  int target_dir_{0};
  uint32_t command_start_ms_{0};
  uint32_t last_cmd_tx_ms_{0};
  bool locked_{false};  // display shows "---"
  bool unlocking_{false};
  uint32_t unlock_start_ms_{0};
  uint32_t last_height_frame_ms_{0};
  uint32_t press_until_ms_{0};

  uint32_t stats_start_ms_{0};
  uint32_t panel_ok_{0};
  uint32_t panel_bad_{0};
  uint32_t box_ok_{0};
  uint32_t box_bad_{0};
  uint32_t tx_frames_{0};
};

}  // namespace jsdesk
}  // namespace esphome
