#pragma once

// A screen opened by a button HOLD is handed that button's release a frame or
// two later, and acts on it: the Library moved its selection, the quick-toggle
// drawer stepped a row. MappedInputManager has suppressNextBackRelease() and
// suppressNextConfirmRelease() for the same problem on Back and Confirm, but
// nothing for the two button PAIRS, which is where the hold gestures live.
//
// So the screen ignores the first release of each pair button instead. Local
// state, like ConfirmationActivity::ignoreConfirmRelease -- opening on the
// release rather than the hold would remove the whole class of bug, but it
// also removes the feedback at the moment the threshold is crossed, and the
// hold is the gesture that was settled on the device.
//
// Construct one as a member, call consumeRelease() first thing in loop(), and
// return when it says true.

#include <cstdint>

#include "MappedInputManager.h"

class HoldOpenReleaseLock {
 public:
  // True when this frame's input was the opening gesture's release and has
  // been swallowed. The caller should return without handling anything else.
  bool consumeRelease(const MappedInputManager& input) {
    if (locked_ == 0) return false;

    bool swallowed = false;
    for (uint8_t i = 0; i < kButtonCount; i++) {
      const uint8_t bit = static_cast<uint8_t>(1u << i);
      if ((locked_ & bit) == 0) continue;

      // A button that is not down any more can never produce the release we
      // are waiting for, so its lock goes whether or not the edge landed in
      // this frame. The three the user never pressed clear on the first loop
      // and cost nothing.
      if (input.wasReleased(kButtons[i])) {
        locked_ &= static_cast<uint8_t>(~bit);
        swallowed = true;
      } else if (!input.isPressed(kButtons[i])) {
        locked_ &= static_cast<uint8_t>(~bit);
      }
    }
    return swallowed;
  }

 private:
  // The side pair (Up/Down) and the front pair (Left/Right): between them they
  // are every button a hold gesture can be bound to.
  static constexpr MappedInputManager::Button kButtons[] = {
      MappedInputManager::Button::Up, MappedInputManager::Button::Down, MappedInputManager::Button::Left,
      MappedInputManager::Button::Right};
  static constexpr uint8_t kButtonCount = 4;

  uint8_t locked_ = (1u << kButtonCount) - 1;
};
