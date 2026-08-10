#pragma once

#include "fontIds.h"

// The countdown figure is pinned to one reading face, so platformio.ini must
// keep 16 pt built. OMIT_LARGE_FONT unregisters it silently: the id is a macro
// that still resolves, only the insertFont call stops running, so getLineHeight()
// returns 0 and drawCenteredText draws nothing -- an empty ring, no build error.
// Which faces exist is a build flag now, not a property of this file.
//
// So the file states its dependency to the compiler rather than to the reader.
// A comment is what shipped v1.5.42 with an empty ring for two releases; this
// stops the build instead, and whoever wants the 219 KB back has to give these
// two screens a face they can draw with first.
#if defined(OMIT_LARGE_FONT)
#error \
    "OMIT_LARGE_FONT drops the 16 pt face that COUNTDOWN_VALUE_FONT_ID points at: the countdown and pomodoro rings would draw empty. Give CountdownLayout.h a face that survives the build before omitting this one."
#endif
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

// The figure and its label, centred in the ring and truncated to fit inside it.
// Shared so the countdown and the pomodoro cannot drift apart, and so the start
// cue has one place to invert rather than two copies to keep in step.
void drawCountdownCentre(const GfxRenderer& renderer, const CountdownLayout& layout, const char* value,
                         const char* label, bool inverted);
