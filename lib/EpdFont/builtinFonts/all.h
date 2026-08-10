#pragma once

// Built-in reading fonts are fixed at 10, 12, 14, and 16 pt. The default
// variant includes emoji/symbol and PHM CJK fallbacks; noemoji includes only
// the primary fonts.
//
// Reading fonts dominate the firmware image: they account for roughly a third
// of the app0 partition, so which ones are baked in is a build-time choice.
// Families and point sizes drop independently:
//
//   OMIT_BITTER_FONT      - drops the Bitter family
//   OMIT_LEXENDDECA_FONT  - drops the Lexend Deca family
//   OMIT_TINY_FONT        - drops 10 pt reading fonts
//   OMIT_SMALL_FONT       - drops 12 pt reading fonts
//   OMIT_MEDIUM_FONT      - drops 14 pt reading fonts
//   OMIT_LARGE_FONT       - drops 16 pt reading fonts
//
// Nothing else needs updating when a size is dropped: isReaderFontSizeAvailable
// in CrossPointSettings.cpp reports it as unavailable, and the reader snaps a
// persisted preference to the nearest surviving size. Dropping a family does
// need CrossPointSettings::FONT_FAMILY to follow; the static_asserts there
// fail the build if it does not.
//
// Macro names match CrumBLE's so the two forks can merge this area cleanly.
#ifdef OMIT_EMOJI_FONTS
#define BUILTIN_READING_FONT_HEADER(name) <builtinFonts/noemoji/name.h>
#else
#define BUILTIN_READING_FONT_HEADER(name) <builtinFonts/name.h>
#endif

#if defined(OMIT_BITTER_FONT) && defined(OMIT_LEXENDDECA_FONT)
#error "At least one built-in reading font family must be kept"
#endif
#if defined(OMIT_TINY_FONT) && defined(OMIT_SMALL_FONT) && defined(OMIT_MEDIUM_FONT) && defined(OMIT_LARGE_FONT)
#error "At least one built-in reading font size must be kept"
#endif

#ifndef OMIT_BITTER_FONT
#ifndef OMIT_TINY_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_10_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_10_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_10_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_10_regular)
#endif
#ifndef OMIT_SMALL_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_12_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_12_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_12_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_12_regular)
#endif
#ifndef OMIT_MEDIUM_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_14_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_14_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_14_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_14_regular)
#endif
#ifndef OMIT_LARGE_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_16_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_16_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_16_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_16_regular)
#endif
#endif  // OMIT_BITTER_FONT

#ifndef OMIT_LEXENDDECA_FONT
#ifndef OMIT_TINY_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_10_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_10_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_10_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_10_regular)
#endif
#ifndef OMIT_SMALL_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_12_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_12_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_12_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_12_regular)
#endif
#ifndef OMIT_MEDIUM_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_14_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_14_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_14_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_14_regular)
#endif
#ifndef OMIT_LARGE_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_16_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_16_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_16_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_16_regular)
#endif
#endif  // OMIT_LEXENDDECA_FONT

#undef BUILTIN_READING_FONT_HEADER

// UI fonts - no emoji or PHM variants. These are not build-time optional: the
// shell needs all three sizes regardless of which reading fonts survive.
#include <builtinFonts/inter_10_bold.h>
#include <builtinFonts/inter_10_regular.h>
#include <builtinFonts/inter_12_bold.h>
#include <builtinFonts/inter_12_regular.h>
#include <builtinFonts/inter_8_regular.h>
