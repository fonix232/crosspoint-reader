#pragma once

#include <cstdint>
#include <cstring>

namespace wired_debug {
enum class Operation { Info, Power, Reset, Screenshot, Button, Touch };
enum class Action { Down, Up, Click, Move, Tap, Swipe };
struct Request {
  uint32_t id = 0;
  Operation operation = Operation::Info;
  Action action = Action::Down;
  uint8_t button = 0;
  uint32_t x = 0, y = 0, endX = 0, endY = 0, duration = 0;
};

inline bool word(const char*& text, const char* value) {
  const size_t length = strlen(value);
  if (strncmp(text, value, length) || (text[length] && text[length] != ' ')) return false;
  text += length;
  if (*text == ' ') ++text;
  return true;
}

inline bool number(const char*& text, uint32_t& value, uint32_t limit) {
  if (*text < '0' || *text > '9') return false;
  value = 0;
  do {
    const auto digit = static_cast<uint32_t>(*text++ - '0');
    if (value > limit / 10 || (value == limit / 10 && digit > limit % 10)) return false;
    value = value * 10 + digit;
  } while (*text >= '0' && *text <= '9');
  if (*text && *text != ' ') return false;
  if (*text == ' ') ++text;
  return true;
}

inline bool parse(const char* text, Request& out) {
  if (!word(text, "CMD:DEBUG") || !number(text, out.id, 2147483647) || !out.id) return false;
  if (word(text, "INFO"))
    out.operation = Operation::Info;
  else if (word(text, "POWER"))
    out.operation = Operation::Power;
  else if (word(text, "RESET"))
    out.operation = Operation::Reset;
  else if (word(text, "SCREENSHOT"))
    out.operation = Operation::Screenshot;
  else if (word(text, "BUTTON")) {
    out.operation = Operation::Button;
    static constexpr const char* names[] = {"BACK", "CONFIRM", "LEFT", "RIGHT", "UP", "DOWN", "POWER", "HOME"};
    uint8_t i = 0;
    for (; i < 8; ++i)
      if (word(text, names[i])) break;
    if (i == 8) return false;
    out.button = i;
    if (word(text, "DOWN"))
      out.action = Action::Down;
    else if (word(text, "UP"))
      out.action = Action::Up;
    else if (word(text, "CLICK")) {
      out.action = Action::Click;
      if (!number(text, out.duration, 5000) || !out.duration) return false;
    } else
      return false;
  } else if (word(text, "TOUCH")) {
    out.operation = Operation::Touch;
    if (word(text, "UP")) {
      out.action = Action::Up;
      return !*text;
    }
    if (word(text, "DOWN"))
      out.action = Action::Down;
    else if (word(text, "MOVE"))
      out.action = Action::Move;
    else if (word(text, "TAP"))
      out.action = Action::Tap;
    else if (word(text, "SWIPE"))
      out.action = Action::Swipe;
    else
      return false;
    if (!number(text, out.x, 32767) || !number(text, out.y, 32767)) return false;
    if (out.action == Action::Swipe && (!number(text, out.endX, 32767) || !number(text, out.endY, 32767))) return false;
    if ((out.action == Action::Tap || out.action == Action::Swipe) &&
        (!number(text, out.duration, out.action == Action::Swipe ? 700 : 5000) || !out.duration))
      return false;
  } else
    return false;
  return !*text;
}

inline uint32_t crc32(uint32_t crc, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
  }
  return crc;
}
}  // namespace wired_debug
