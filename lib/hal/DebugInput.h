#pragma once

#include <cstdint>
#include <cstdlib>

// Main-task-only synthetic contacts. Fixed storage; no queues or heap allocations.
// Gesture thresholds match InputManager's single-contact classifier.
class DebugInput {
 public:
  static constexpr uint32_t CONTACT_TIMEOUT_MS = 10000;
  static constexpr uint8_t HOME = 7;
  struct Point {
    int16_t x = 0, y = 0;
  };
  uint8_t buttons = 0, pressed = 0, released = 0;
  bool touch = false, down = false, up = false, longPress = false;
  bool suppressed = false, moved = false, tapMoved = false, longFired = false;
  bool homeLong = false, homeTap = false;
  Point start, position;
  uint32_t touchAt = 0, touchHeld = 0, lastButtonHeld = 0, lastPowerHeld = 0;

  bool button(uint8_t index, bool held, uint32_t duration = 0) {
    if (index > HOME) return false;
    const uint8_t mask = 1u << index;
    if (held && (requested & mask)) return false;
    if (!held && !(requested & mask)) return false;
    if (held)
      requested |= mask;
    else
      requested &= ~mask;
    durations[index] = duration;
    return true;
  }

  bool touchDown(Point point) { return touchDown(point, 0, point); }

  bool touchDown(Point point, uint32_t duration, Point end) {
    if (touch || pendingDown) return false;
    pendingDown = true;
    start = position = point;
    target = duration ? end : point;
    touchDuration = duration;
    return true;
  }

  bool touchMove(Point point) {
    if (!touch || touchDuration) return false;
    position = point;
    classifyMotion();
    return true;
  }

  bool touchUp() {
    if (!touch || touchDuration) return false;
    pendingUp = true;
    return true;
  }

  void reset() {
    const bool cancelTouch = touch || pendingDown;
    *this = DebugInput();
    pendingCancel = cancelTouch;
  }

  void update(uint32_t now) {
    pressed = released = 0;
    down = up = longPress = homeLong = homeTap = false;
    if (pendingCancel) {
      pendingCancel = false;
      up = suppressed = true;
    }
    for (uint8_t i = 0; i <= HOME; ++i) {
      const uint8_t bit = 1u << i;
      if ((requested & bit) && !(buttons & bit)) {
        buttonAt[i] = now;
        pressed |= bit;
        if (i == HOME) homeLongFired = false;
      } else if (buttons & bit) {
        const uint32_t held = now - buttonAt[i];
        if ((requested & bit) && durations[i] && held >= durations[i]) requested &= ~bit;
        if (!(requested & bit)) {
          released |= bit;
          lastButtonHeld = held;
          if (i == 6) lastPowerHeld = held;
          if (i == HOME) homeTap = !homeLongFired;
        } else if (held >= CONTACT_TIMEOUT_MS) {
          // A lost host must not leave a button down or synthesize a click.
          requested &= ~bit;
        } else if (i == HOME && !homeLongFired && held >= 700) {
          homeLong = homeLongFired = true;
        }
      }
    }
    buttons = requested;
    if (pendingDown) {
      pendingDown = false;
      touch = down = true;
      suppressed = moved = tapMoved = longFired = false;
      touchAt = now;
    } else if (touch) {
      touchHeld = now - touchAt;
      if (touchDuration) {
        const uint32_t elapsed = touchHeld < touchDuration ? touchHeld : touchDuration;
        position.x =
            start.x + (target.x - start.x) * static_cast<int32_t>(elapsed) / static_cast<int32_t>(touchDuration);
        position.y =
            start.y + (target.y - start.y) * static_cast<int32_t>(elapsed) / static_cast<int32_t>(touchDuration);
        classifyMotion();
        if (touchHeld >= touchDuration) pendingUp = true;
      }
      if (pendingUp) {
        pendingUp = false;
        touch = false;
        up = !suppressed;
      } else if (touchHeld >= CONTACT_TIMEOUT_MS) {
        touch = false;
        up = suppressed = true;
      } else if (!suppressed && !moved && !longFired && touchHeld >= 500) {
        longPress = longFired = true;
      }
    }
  }

  bool tap() const { return up && !suppressed && !tapMoved; }
  bool swipe() const {
    return up && !suppressed && touchHeld <= 700 &&
           (std::abs(position.x - start.x) >= 60 || std::abs(position.y - start.y) >= 60);
  }
  bool active() const { return buttons || requested || touch || pendingDown; }
  uint32_t held(uint32_t now, uint8_t index) const {
    return index <= HOME && (buttons & (1u << index)) ? now - buttonAt[index] : 0;
  }
  uint32_t held(uint32_t now) const {
    uint32_t longest = 0;
    for (uint8_t i = 0; i < HOME; ++i) {
      const auto value = held(now, i);
      if (value > longest) longest = value;
    }
    return buttons ? longest : lastButtonHeld;
  }

 private:
  uint8_t requested = 0;
  uint32_t buttonAt[8] = {}, durations[8] = {};
  uint32_t touchDuration = 0;
  Point target;
  bool pendingDown = false, pendingUp = false, pendingCancel = false, homeLongFired = false;
  void classifyMotion() {
    const int dx = std::abs(position.x - start.x), dy = std::abs(position.y - start.y);
    moved |= dx > 28 || dy > 28;
    tapMoved |= dx >= 60 || dy >= 60;
  }
};
