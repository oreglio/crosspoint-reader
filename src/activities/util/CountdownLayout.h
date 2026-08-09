#pragma once

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
