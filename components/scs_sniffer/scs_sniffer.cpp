#include "scs_sniffer.h"
#include "esphome/core/gpio_pin.h"
#include "esphome/core/log.h"
#include "esphome/core/progmem.h"

#include <cinttypes>

namespace esphome::scs_sniffer {

// Short, and what the tools that read these logs expect
ESPHOME_LOG_TAG(TAG, "scs");

// Indexed by Error
PROGMEM_STRING_TABLE(ErrorStrings, "long low", "bit phase", "stop bit", "no end byte", "checksum", "truncated frame");
static_assert(ErrorStrings::COUNT == static_cast<size_t>(Error::ERROR_TRUNCATED) + 1);

static constexpr size_t RAW_PER_LINE = 24;

void SCSSniffer::Sink::reset() {
  this->bytes.clear();
  this->errors.clear();
  this->frames = 0;
  this->stray = 0;
  this->errors_dropped = 0;
  this->bytes_dropped = false;
}

void SCSSniffer::Sink::on_byte(uint8_t b) {
  if (this->bytes.size() == this->bytes.capacity()) {
    this->bytes_dropped = true;
  } else {
    this->bytes.push_back(b);
  }
}

void SCSSniffer::Sink::on_stray(uint8_t b) {
  if (b == ACK_BYTE) {
    ESP_LOGD(TAG, "ack");
  } else {
    ++this->stray;
  }
}

void SCSSniffer::Sink::on_error(Error e) {
  if (this->errors.size() == this->errors.capacity()) {
    ++this->errors_dropped;
  } else {
    this->errors.push_back(e);
  }
}

void SCSSniffer::Sink::on_frame(const Frame &f) {
  ++this->frames;
  const Message message = parse(f);
  char summary[SUMMARY_SIZE];
  const size_t length = describe(message, summary);
  char hex[format_hex_pretty_size(MAX_FRAME_SIZE)];
  ESP_LOGD(TAG, "frame %s: %s", format_hex_pretty_to(hex, f.begin(), f.size, ' '), summary);
  this->parent->frame_callback_.call(f, message, StringRef(summary, length));
}

void SCSSniffer::setup() { setup_output_pin(this->enable_pin_, true); }

void SCSSniffer::dump_config() {
  ESP_LOGCONFIG(TAG, "SCS sniffer:");
  LOG_PIN("  Enable pin: ", this->enable_pin_);
}

bool SCSSniffer::on_receive(remote_base::RemoteReceiveData data) {
  const remote_base::RawTimingsView raw = data.get_raw_data();
  Sink &sink = this->decoder_.sink();
  sink.reset();
  const uint32_t glitches_before = this->decoder_.glitches();
  this->decoder_.feed_marks_spaces(raw.data(), raw.size());
  const uint32_t glitches = this->decoder_.glitches() - glitches_before;
  if (!sink.errors.empty() || sink.stray != 0 || glitches != 0)
    this->log_capture_(raw, glitches);
  return false;
}

void SCSSniffer::log_capture_(remote_base::RawTimingsView raw, uint32_t glitches) {
  const Sink &sink = this->decoder_.sink();

  // Every error name is shorter than 16 characters
  char errors[(16 + 2) * MAX_LOGGED_ERRORS + 8];
  size_t pos = 0;
  errors[0] = '\0';
  for (Error e : sink.errors) {
    if (pos != 0)
      pos = buf_append_str(errors, sizeof(errors), pos, ", ");
    pos = buf_append_str_p(errors, sizeof(errors), pos,
                           reinterpret_cast<ESPHOME_PGM_P>(ErrorStrings::get_progmem_str(e)));
  }
  if (sink.errors_dropped != 0)
    buf_append_str(errors, sizeof(errors), pos, ", ...");
  ESP_LOGW(TAG, "capture: %u frames, %u errors [%s], %u stray bytes, %" PRIu32 " glitches, %u durations", sink.frames,
           static_cast<unsigned>(sink.errors.size() + sink.errors_dropped), errors, sink.stray, glitches,
           static_cast<unsigned>(raw.size()));

  char hex[format_hex_pretty_size(MAX_LOGGED_BYTES)];
  ESP_LOGW(TAG, "bytes: %s%s", format_hex_pretty_to(hex, sink.bytes.data(), sink.bytes.size(), ' '),
           sink.bytes_dropped ? LOG_STR_LITERAL(" ...") : "");

  // "raw[NNNNN]:" and up to RAW_PER_LINE values of at most 7 characters each, " -32767"
  char line[12 + 7 * RAW_PER_LINE + 1];
  for (size_t i = 0; i < raw.size(); i += RAW_PER_LINE) {
    size_t len = buf_append_printf(line, sizeof(line), 0, "raw[%u]:", static_cast<unsigned>(i));
    for (size_t j = i; j < raw.size() && j < i + RAW_PER_LINE; ++j)
      len = buf_append_printf(line, sizeof(line), len, " %" PRId32, raw[j]);
    ESP_LOGW(TAG, "%s", line);
  }
}

}  // namespace esphome::scs_sniffer
