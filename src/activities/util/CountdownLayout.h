#pragma once

#include "fontIds.h"

// The largest font the firmware actually registers. main.cpp inserts Lexend Deca
// and Bitter at 10/12/14/16 plus the UI faces at 10/12, and nothing above that —
// the 18 and 20 identifiers exist but their data is not built in. UI_12 left the
// figure lost inside the ring.
#define COUNTDOWN_VALUE_FONT_ID LEXENDDECA_16_FONT_ID

class GfxRenderer;
class MappedInputManager;

// Where the ring, its centred text and the context line go. Derived from the
// renderer and the theme rather than hardcoded, so orientation and future device
// profiles carry over. Shared by the countdown and pomodoro screens so the two
// cannot drift apart.
struct CountdownLayout {
  int cx;
  int cy;
  int outerRadius;
  int stroke;
  int centerMaxWidth;   // truncation budget inside the ring
  int contextY;         // top of the single context line under the ring
  int contextMaxWidth;  // truncation budget for that line
};

CountdownLayout computeCountdownLayout(const GfxRenderer& renderer, const MappedInputManager& mappedInput);
