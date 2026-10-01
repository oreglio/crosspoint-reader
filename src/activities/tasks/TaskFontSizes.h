#pragma once

#include <I18n.h>

#include "fontIds.h"

// The task list's third text size. It is the built-in Lexend Deca 14 where the
// firmware carries it; X3/X4 builds drop the 14 pt reading fonts
// (OMIT_MEDIUM_FONT), and a font id nobody registered draws nothing at all, so
// there it is the 16 pt face the countdown already keeps. The label follows the
// face so the menu names the size the list actually uses.
#if CROSSINK_SCALABLE_FONTS || (!defined(OMIT_LEXENDDECA_FONT) && !defined(OMIT_MEDIUM_FONT))
constexpr int kLargeTaskFontId = LEXENDDECA_14_FONT_ID;
constexpr StrId kLargeTaskFontLabel = StrId::STR_TASK_FONT_14;
#elif !defined(OMIT_LEXENDDECA_FONT) && !defined(OMIT_LARGE_FONT)
constexpr int kLargeTaskFontId = LEXENDDECA_16_FONT_ID;
constexpr StrId kLargeTaskFontLabel = StrId::STR_TASK_FONT_16;
#else
constexpr int kLargeTaskFontId = UI_12_FONT_ID;
constexpr StrId kLargeTaskFontLabel = StrId::STR_TASK_FONT_12;
#endif
