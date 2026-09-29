#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include "I18nKeys.h"
#include "../ReaderFontSizeStep.h"

struct FontFamilyPointSizeRange {
  uint8_t first = 0;
  uint8_t last = 0;

  bool isValid() const { return first != 0; }
};

// The built-in reading families this firmware carries, in FONT_FAMILY order.
//
// Every screen that offers a font choice reads this one table. Three separate
// hand-written copies existed before, and dropping Bitter from the build left
// one of them still advertising it -- with setting index 1, which by then meant
// "first SD family", so picking Bitter silently installed Alegreya. Same shape
// as the v1.5.21 menu-counter bug. Adding a family here is the only edit
// needed; CrossPointSettings.cpp static_asserts this count against
// BUILTIN_FONT_COUNT.
struct BuiltinFontFamilyEntry {
  StrId label;
  uint8_t settingIndex;
};

inline constexpr BuiltinFontFamilyEntry BUILTIN_FONT_FAMILIES[] = {
    {StrId::STR_LEXEND_DECA, 0},
#ifndef OMIT_BITTER_FONT
    {StrId::STR_BITTER, 1},
#endif
};

inline constexpr uint8_t BUILTIN_FONT_FAMILY_COUNT =
    static_cast<uint8_t>(sizeof(BUILTIN_FONT_FAMILIES) / sizeof(BUILTIN_FONT_FAMILIES[0]));

// The point sizes the built-in families actually offer, which is what their
// labels must advertise. BUILTIN_READER_FONT_SIZES already leaves out every
// size this build dropped, and holds the whole scalable range when the
// reader renders vector fonts.
inline constexpr FontFamilyPointSizeRange BUILTIN_FONT_POINT_SIZE_RANGE{
    BUILTIN_READER_FONT_SIZES[0], BUILTIN_READER_FONT_SIZES[std::size(BUILTIN_READER_FONT_SIZES) - 1]};

inline constexpr FontFamilyPointSizeRange builtinFontPointSizeRange() { return BUILTIN_FONT_POINT_SIZE_RANGE; }

inline FontFamilyPointSizeRange fontFamilyPointSizeRange(const SdCardFontFamilyInfo& family) {
  if (family.firstSize) return {family.firstSize, family.lastSize};
  if (family.isScalable()) return {8, 22};
  FontFamilyPointSizeRange range;
  for (const auto& file : family.files) {
    if (file.style != 0) continue;
    if (!range.isValid() || file.pointSize < range.first) range.first = file.pointSize;
    if (file.pointSize > range.last) range.last = file.pointSize;
  }
  return range;
}

inline std::string fontFamilyLabel(const std::string_view familyName, const FontFamilyPointSizeRange range) {
  std::string label;
  label.reserve(familyName.size() + 24);
  label.append(familyName.data(), familyName.size());
  if (!range.isValid()) return label;

  label += " (";
  label += std::to_string(range.first);
  if (range.last != range.first) {
    label += "-";
    label += std::to_string(range.last);
  }
  label += "pt";
  label += ")";
  return label;
}
