#include "jsdesk.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome {
namespace jsdesk {

static const char *const TAG = "jsdesk";

static const uint8_t PANEL_START = 0xA5;
static const uint8_t BOX_START = 0x5A;
static const uint8_t BUTTON_UP = 0x20;
static const uint8_t BUTTON_DOWN = 0x40;
static const uint8_t PRESET_BUTTONS[] = {0x02, 0x04, 0x08, 0x10};
// Holding M unlocks the panel ("---" on the display) and shows the height.
static const uint8_t BUTTON_M = 0x01;

static const uint32_t FRAME_INTERVAL_MS = 16;
// Send our own frames if the panel goes quiet while we are moving.
static const uint32_t PANEL_SILENT_MS = 50;
// Stop if the box stops reporting the height.
static const uint32_t BOX_SILENT_MS = 1000;
// Stop if the height has not changed for this long (end stop, obstacle, preset reached).
static const uint32_t STALL_MS = 2000;
static const uint32_t MAX_RUN_MS = 60000;
static const uint32_t STATS_INTERVAL_MS = 10000;
static const uint32_t UNLOCK_TIMEOUT_MS = 6000;
static const size_t MAX_BUFFER = 64;

static int segment_to_digit(uint8_t seg) {
  switch (seg & 0x7F) {
    case 0x3F:
      return 0;
    case 0x06:
      return 1;
    case 0x5B:
      return 2;
    case 0x4F:
      return 3;
    case 0x66:  // standard (kortschak's capture)
    case 0x67:  // used by the ssieb/desktronic components
      return 4;
    case 0x6D:
      return 5;
    case 0x7D:
      return 6;
    case 0x07:
      return 7;
    case 0x7F:
      return 8;
    case 0x6F:
      return 9;
    default:
      return -1;
  }
}

static char segment_to_char(uint8_t seg) {
  int digit = segment_to_digit(seg);
  if (digit >= 0)
    return '0' + digit;
  switch (seg & 0x7F) {
    case 0x00:
      return ' ';
    case 0x40:
      return '-';
    case 0x08:
      return '_';
    case 0x77:
      return 'A';
    case 0x7C:
      return 'b';
    case 0x39:
      return 'C';
    case 0x5E:
      return 'd';
    case 0x79:
      return 'E';
    case 0x71:
      return 'F';
    case 0x76:
      return 'H';
    case 0x38:
      return 'L';
    case 0x54:
      return 'n';
    case 0x5C:
      return 'o';
    case 0x73:
      return 'P';
    case 0x50:
      return 'r';
    case 0x3E:
      return 'U';
    case 0x1C:
      return 'u';
    default:
      return '?';
  }
}

static std::string to_hex(const uint8_t *data, size_t len) {
  std::string out;
  char buf[4];
  for (size_t i = 0; i < len; i++) {
    snprintf(buf, sizeof(buf), i ? " %02X" : "%02X", data[i]);
    out += buf;
  }
  return out;
}

static const char *command_to_str(JSDeskCommand command) {
  switch (command) {
    case JSDESK_IDLE:
      return "idle";
    case JSDESK_UP:
      return "up";
    case JSDESK_DOWN:
      return "down";
    case JSDESK_TARGET:
      return "target";
    case JSDESK_PRESET:
      return "preset";
    case JSDESK_PRESS:
      return "press";
    default:
      return "unknown";
  }
}

void JSDesk::setup() {
  // KEY1 must never float high: a high KEY1 on boot makes some boxes run the desk.
  if (this->key_out_pin_ != nullptr) {
    this->key_out_pin_->setup();
    this->key_out_pin_->digital_write(false);
  }
  if (this->key_in_pin_ != nullptr) {
    this->key_in_pin_->setup();
    this->key_in_state_ = this->key_in_pin_->digital_read();
    if (this->key1_sensor_ != nullptr)
      this->key1_sensor_->publish_initial_state(this->key_in_state_);
  }
  if (this->key2_in_pin_ != nullptr) {
    this->key2_in_pin_->setup();
    this->key2_in_state_ = this->key2_in_pin_->digital_read();
    if (this->key2_sensor_ != nullptr)
      this->key2_sensor_->publish_initial_state(this->key2_in_state_);
  }
  // In passthrough mode the panel's frames go through us; forward them with minimal delay.
  if (this->box_tx_uart_ != nullptr)
    this->high_freq_.start();
  this->stats_start_ms_ = millis();
}

void JSDesk::loop() {
  const uint32_t now = millis();
  this->read_box_(now);
  this->read_panel_(now);
  this->read_keys_(now);
  this->update_command_(now);
  this->update_key_out_(now);

  if (this->log_frames_ && now - this->stats_start_ms_ >= STATS_INTERVAL_MS) {
    ESP_LOGD(TAG, "Last %" PRIu32 " s: panel %" PRIu32 " frames (%" PRIu32 " bad bytes), box %" PRIu32
                  " frames (%" PRIu32 " bad bytes), box frame length %u, sent %" PRIu32 " frames",
             (now - this->stats_start_ms_) / 1000, this->panel_ok_, this->panel_bad_, this->box_ok_, this->box_bad_,
             this->box_len_, this->tx_frames_);
    this->panel_ok_ = this->panel_bad_ = this->box_ok_ = this->box_bad_ = this->tx_frames_ = 0;
    this->stats_start_ms_ = now;
  }
}

void JSDesk::read_box_(uint32_t now) {
  uint8_t c;
  while (this->box_uart_->available() && this->box_uart_->read_byte(&c)) {
    this->box_buf_.push_back(c);
    if (this->box_buf_.size() > MAX_BUFFER)
      this->box_buf_.clear();

    while (!this->box_buf_.empty()) {
      if (this->box_buf_[0] != BOX_START) {
        this->box_buf_.erase(this->box_buf_.begin());
        this->box_bad_++;
        continue;
      }
      const size_t n = this->box_buf_.size();
      if (n < 5 || (n < 6 && this->box_len_ != 5))
        break;  // wait for more bytes (a 6-byte frame is still possible)

      const uint8_t *b = this->box_buf_.data();
      const bool ok6 = n >= 6 && this->box_len_ != 5 && uint8_t(b[1] + b[2] + b[3] + b[4]) == b[5];
      const bool ok5 = this->box_len_ != 6 && uint8_t(b[1] + b[2] + b[3]) == b[4];
      size_t len = ok6 ? 6 : (ok5 ? 5 : 0);
      if (len == 0) {
        this->box_buf_.erase(this->box_buf_.begin());
        this->box_bad_++;
        if (++this->box_bad_streak_ > 50 && this->box_len_ != 0) {
          ESP_LOGW(TAG, "Too many invalid box frames, re-detecting frame length");
          this->box_len_ = 0;
          this->box_bad_streak_ = 0;
        }
        continue;
      }
      if (this->box_len_ != len)
        ESP_LOGI(TAG, "Box frames are %u bytes long", (unsigned) len);
      this->box_len_ = (uint8_t) len;
      this->box_bad_streak_ = 0;
      this->handle_box_frame_(b, len, now);
      this->box_buf_.erase(this->box_buf_.begin(), this->box_buf_.begin() + len);
    }
  }
}

void JSDesk::handle_box_frame_(const uint8_t *frame, size_t len, uint32_t now) {
  this->box_ok_++;
  this->last_box_ms_ = now;
  if (this->last_box_frame_.size() != len || !std::equal(frame, frame + len, this->last_box_frame_.begin())) {
    if (this->log_frames_)
      ESP_LOGD(TAG, "box   -> %s", to_hex(frame, len).c_str());
    this->last_box_frame_.assign(frame, frame + len);
  }

  // In 6-byte frames d[3] is a status byte (0x01 on veli's box, 0x10 on the Q200-23);
  // the first three bytes are the display either way.
  const uint8_t *d = frame + 1;

  if (this->display_sensor_ != nullptr) {
    std::string text;
    for (int i = 0; i < 3; i++) {
      text += segment_to_char(d[i]);
      if (d[i] & 0x80)
        text += '.';
    }
    if (!this->display_sensor_->has_state() || this->display_sensor_->state != text)
      this->display_sensor_->publish_state(text);
  }

  // "---" means the panel is locked; a height means it is unlocked. Blank and
  // E04 flash in between in both states and leave the lock state unchanged.
  if (d[0] == 0x40 && d[1] == 0x40 && d[2] == 0x40) {
    if (!this->locked_)
      ESP_LOGD(TAG, "Panel locked");
    this->locked_ = true;
    return;
  }
  if ((d[0] | d[1] | d[2]) == 0)
    return;  // display blank
  const int d0 = segment_to_digit(d[0]), d1 = segment_to_digit(d[1]), d2 = segment_to_digit(d[2]);
  if (d0 < 0 || d1 < 0 || d2 < 0)
    return;  // error code, menu, ...
  if (this->locked_)
    ESP_LOGD(TAG, "Panel unlocked");
  this->locked_ = false;
  this->last_height_frame_ms_ = now;
  float height = d0 * 100 + d1 * 10 + d2;
  if (d[1] & 0x80)
    height /= 10.0f;  // "72.5" has the decimal point on the middle digit
  if (height < this->min_height_ - 10.0f || height > this->max_height_ + 10.0f) {
    ESP_LOGV(TAG, "Ignoring implausible height %.1f", height);
    return;
  }
  if (height != this->height_) {
    if (!std::isnan(this->height_))
      this->last_delta_ = height > this->height_ ? 1 : -1;
    this->height_ = height;
    this->last_height_change_ms_ = now;
    if (this->height_sensor_ != nullptr)
      this->height_sensor_->publish_state(height);
  }
}

void JSDesk::read_panel_(uint32_t now) {
  if (this->panel_uart_ == nullptr)
    return;
  uint8_t c;
  while (this->panel_uart_->available() && this->panel_uart_->read_byte(&c)) {
    this->panel_buf_.push_back(c);
    if (this->panel_buf_.size() > MAX_BUFFER)
      this->panel_buf_.clear();

    while (!this->panel_buf_.empty()) {
      if (this->panel_buf_[0] != PANEL_START) {
        this->panel_buf_.erase(this->panel_buf_.begin());
        this->panel_bad_++;
        continue;
      }
      if (this->panel_buf_.size() < 5)
        break;
      const uint8_t *b = this->panel_buf_.data();
      if (uint8_t(b[1] + b[2] + b[3]) != b[4]) {
        this->panel_buf_.erase(this->panel_buf_.begin());
        this->panel_bad_++;
        continue;
      }
      this->handle_panel_frame_(b, now);
      this->panel_buf_.erase(this->panel_buf_.begin(), this->panel_buf_.begin() + 5);
    }
  }
}

void JSDesk::handle_panel_frame_(const uint8_t *frame, uint32_t now) {
  this->panel_ok_++;
  this->last_panel_ms_ = now;
  if (memcmp(frame, this->last_panel_frame_, 5) != 0) {
    if (this->log_frames_)
      ESP_LOGD(TAG, "panel -> %s", to_hex(frame, 5).c_str());
    memcpy(this->last_panel_frame_, frame, 5);
  }

  const uint8_t buttons = frame[2];
  for (auto &entry : this->button_sensors_)
    entry.second->publish_state(buttons & entry.first);

  if (this->box_tx_uart_ == nullptr)
    return;  // sniff mode

  if (this->command_ != JSDESK_IDLE) {
    if ((frame[1] | buttons) != 0) {
      ESP_LOGI(TAG, "Panel button pressed (%02X %02X), handing control back", frame[1], buttons);
      this->stop();
      this->box_tx_uart_->write_array(frame, 5);
    } else {
      // Replace the panel's idle poll with our command, keeping its timing.
      this->send_command_frame_(now);
    }
    return;
  }
  this->box_tx_uart_->write_array(frame, 5);
}

void JSDesk::read_keys_(uint32_t now) {
  if (this->key_in_pin_ != nullptr) {
    const bool state = this->key_in_pin_->digital_read();
    if (state != this->key_in_state_) {
      if (this->log_frames_)
        ESP_LOGD(TAG, "KEY1 -> %s (previous level lasted %" PRIu32 " ms)", ONOFF(state),
                 now - this->key_in_changed_ms_);
      this->key_in_state_ = state;
      this->key_in_changed_ms_ = now;
      if (this->key1_sensor_ != nullptr)
        this->key1_sensor_->publish_state(state);
    }
  }
  if (this->key2_in_pin_ != nullptr) {
    const bool state = this->key2_in_pin_->digital_read();
    if (state != this->key2_in_state_) {
      if (this->log_frames_)
        ESP_LOGD(TAG, "KEY2 -> %s (previous level lasted %" PRIu32 " ms)", ONOFF(state),
                 now - this->key2_in_changed_ms_);
      this->key2_in_state_ = state;
      this->key2_in_changed_ms_ = now;
      if (this->key2_sensor_ != nullptr)
        this->key2_sensor_->publish_state(state);
    }
  }
}

void JSDesk::update_command_(uint32_t now) {
  if (this->command_ == JSDESK_IDLE) {
    // No panel (or panel quiet): poll the box ourselves so it keeps reporting the height.
    if (this->box_tx_uart_ != nullptr && now - this->last_panel_ms_ > PANEL_SILENT_MS &&
        now - this->last_cmd_tx_ms_ >= FRAME_INTERVAL_MS)
      this->send_command_frame_(now);
    return;
  }

  if (now - this->command_start_ms_ > MAX_RUN_MS) {
    ESP_LOGW(TAG, "Move took longer than %" PRIu32 " s, stopping", MAX_RUN_MS / 1000);
    this->stop();
    return;
  }
  if (now - this->last_box_ms_ > BOX_SILENT_MS) {
    ESP_LOGW(TAG, "No height reports from the box, stopping");
    this->stop();
    return;
  }
  if (this->unlocking_) {
    if (int32_t(this->last_height_frame_ms_ - this->unlock_start_ms_) > 0) {
      ESP_LOGI(TAG, "Unlocked after %" PRIu32 " ms, starting %s", now - this->unlock_start_ms_,
               command_to_str(this->command_));
      this->unlocking_ = false;
      this->command_start_ms_ = now;
      this->last_height_change_ms_ = now;
      if (this->command_ == JSDESK_TARGET && !this->aim_target_())
        return;
    } else if (now - this->unlock_start_ms_ > UNLOCK_TIMEOUT_MS) {
      ESP_LOGW(TAG, "Could not unlock the box within %" PRIu32 " ms", UNLOCK_TIMEOUT_MS);
      this->stop();
      return;
    }
  } else if (this->command_ == JSDESK_PRESS) {
    if (int32_t(this->press_until_ms_ - now) <= 0) {
      ESP_LOGI(TAG, "Released button %02X", this->command_button_);
      this->stop();
      return;
    }
  } else if (now - this->last_height_change_ms_ > STALL_MS) {
    ESP_LOGI(TAG, "Desk stopped moving at %.1f cm", this->height_);
    this->stop();
    return;
  }
  if (this->command_ == JSDESK_TARGET && !this->unlocking_) {
    if ((this->target_dir_ > 0 && this->height_ >= this->target_ - this->stop_tolerance_) ||
        (this->target_dir_ < 0 && this->height_ <= this->target_ + this->stop_tolerance_)) {
      ESP_LOGI(TAG, "Reached %.1f cm (target %.1f cm)", this->height_, this->target_);
      this->stop();
      return;
    }
  }
  // Normally our frames replace the panel's polls; keep the box fed if the panel is quiet.
  if (now - this->last_panel_ms_ > PANEL_SILENT_MS && now - this->last_cmd_tx_ms_ >= FRAME_INTERVAL_MS)
    this->send_command_frame_(now);
}

void JSDesk::update_key_out_(uint32_t now) {
  if (this->key_out_pin_ == nullptr)
    return;
  bool level;
  if (this->key_on_move_ && this->command_ != JSDESK_IDLE && this->command_ != JSDESK_PRESS && !this->unlocking_) {
    level = this->key_pulse_ms_ == 0 || now - this->command_start_ms_ < this->key_pulse_ms_;
  } else {
    level = this->key_in_state_;  // mirror the panel (low without one)
  }
  if (level != this->key_out_state_) {
    this->key_out_pin_->digital_write(level);
    this->key_out_state_ = level;
  }
}

void JSDesk::send_command_frame_(uint32_t now) {
  const uint8_t button = this->unlocking_ ? BUTTON_M : this->command_button_;
  const uint8_t frame[5] = {PANEL_START, 0x00, button, uint8_t(0xFF - button), 0xFF};
  this->box_tx_uart_->write_array(frame, 5);
  this->last_cmd_tx_ms_ = now;
  this->tx_frames_++;
}

void JSDesk::begin_(JSDeskCommand command, uint8_t button) {
  if (this->box_tx_uart_ == nullptr) {
    ESP_LOGW(TAG, "Cannot move the desk: box_tx_uart is not configured (sniff mode)");
    return;
  }
  const uint32_t now = millis();
  if (now - this->last_box_ms_ > BOX_SILENT_MS) {
    ESP_LOGW(TAG, "Cannot move the desk: no height reports from the box");
    return;
  }
  ESP_LOGI(TAG, "Starting %s (button %02X) at %.1f cm", command_to_str(command), button, this->height_);
  this->command_ = command;
  this->command_button_ = button;
  this->command_start_ms_ = now;
  this->last_height_change_ms_ = now;
  this->unlocking_ = command != JSDESK_PRESS && this->locked_;
  if (this->unlocking_) {
    ESP_LOGI(TAG, "Box is locked, holding M to unlock first");
    this->unlock_start_ms_ = now;
  }
}

void JSDesk::move_up() { this->begin_(JSDESK_UP, BUTTON_UP); }

void JSDesk::move_down() { this->begin_(JSDESK_DOWN, BUTTON_DOWN); }

void JSDesk::move_to(float height) {
  this->target_ = clamp(height, this->min_height_, this->max_height_);
  this->target_dir_ = 0;
  this->begin_(JSDESK_TARGET, 0);
  // When the box is locked the height is not known yet; aim once it is unlocked.
  if (this->command_ == JSDESK_TARGET && !this->unlocking_)
    this->aim_target_();
}

bool JSDesk::aim_target_() {
  if (std::isnan(this->height_) || std::fabs(this->target_ - this->height_) <= this->stop_tolerance_) {
    ESP_LOGI(TAG, "Already at %.1f cm", this->height_);
    this->stop();
    return false;
  }
  this->target_dir_ = this->target_ > this->height_ ? 1 : -1;
  this->command_button_ = this->target_dir_ > 0 ? BUTTON_UP : BUTTON_DOWN;
  ESP_LOGI(TAG, "Moving %s from %.1f to %.1f cm", this->target_dir_ > 0 ? "up" : "down", this->height_,
           this->target_);
  return true;
}

void JSDesk::goto_preset(int preset) {
  if (preset < 1 || preset > 4) {
    ESP_LOGW(TAG, "Invalid preset %d (1-4)", preset);
    return;
  }
  this->begin_(JSDESK_PRESET, PRESET_BUTTONS[preset - 1]);
}

void JSDesk::press(uint8_t button, uint32_t ms) {
  ms = std::min<uint32_t>(ms, 10000);
  this->begin_(JSDESK_PRESS, button);
  if (this->command_ == JSDESK_PRESS)
    this->press_until_ms_ = millis() + ms;
}

void JSDesk::stop() {
  if (this->command_ != JSDESK_IDLE)
    ESP_LOGI(TAG, "Stopping %s at %.1f cm", command_to_str(this->command_), this->height_);
  this->command_ = JSDESK_IDLE;
  this->command_button_ = 0;
  this->unlocking_ = false;
}

int JSDesk::direction() const {
  switch (this->command_) {
    case JSDESK_UP:
      return 1;
    case JSDESK_DOWN:
      return -1;
    case JSDESK_TARGET:
      return this->target_dir_;
    case JSDESK_PRESET:
      return this->last_delta_;
    default:
      return 0;
  }
}

void JSDesk::dump_config() {
  ESP_LOGCONFIG(TAG, "JSDRIVE desk:");
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->box_tx_uart_ != nullptr ? "passthrough (control enabled)" : "sniff (listen only)");
  ESP_LOGCONFIG(TAG, "  Height range: %.1f - %.1f cm, stop tolerance %.1f cm", this->min_height_, this->max_height_,
                this->stop_tolerance_);
  ESP_LOGCONFIG(TAG, "  KEY1 on move: %s", YESNO(this->key_on_move_));
  if (this->key_on_move_ && this->key_pulse_ms_ > 0)
    ESP_LOGCONFIG(TAG, "  KEY1 pulse: %" PRIu32 " ms", this->key_pulse_ms_);
  LOG_PIN("  KEY1 in: ", this->key_in_pin_);
  LOG_PIN("  KEY1 out: ", this->key_out_pin_);
  LOG_PIN("  KEY2 in: ", this->key2_in_pin_);
  LOG_SENSOR("  ", "Height", this->height_sensor_);
  LOG_TEXT_SENSOR("  ", "Display", this->display_sensor_);
}

}  // namespace jsdesk
}  // namespace esphome
