#pragma once

#include <FreeInkUI.h>
#include <I18n.h>

#include <cstdint>

namespace keyboard_layouts {

// A build ships only the translations listed in platformio.ini's
// custom_languages, so Language has no enumerator for the others and naming one
// would not compile. Those rows carry Language::_COUNT instead: the layout keeps
// its persisted bit position -- which the table comment below makes load-bearing
// -- while forLanguage() simply never matches it and falls back to QwertyEn.
#ifdef I18N_HAS_EN
#define KBD_LANG_EN Language::EN
#else
#define KBD_LANG_EN Language::_COUNT
#endif
#ifdef I18N_HAS_FR
#define KBD_LANG_FR Language::FR
#else
#define KBD_LANG_FR Language::_COUNT
#endif
#ifdef I18N_HAS_DE
#define KBD_LANG_DE Language::DE
#else
#define KBD_LANG_DE Language::_COUNT
#endif
#ifdef I18N_HAS_ES
#define KBD_LANG_ES Language::ES
#else
#define KBD_LANG_ES Language::_COUNT
#endif
#ifdef I18N_HAS_RU
#define KBD_LANG_RU Language::RU
#else
#define KBD_LANG_RU Language::_COUNT
#endif
#ifdef I18N_HAS_UK
#define KBD_LANG_UK Language::UK
#else
#define KBD_LANG_UK Language::_COUNT
#endif
#ifdef I18N_HAS_BE
#define KBD_LANG_BE Language::BE
#else
#define KBD_LANG_BE Language::_COUNT
#endif
#ifdef I18N_HAS_KK
#define KBD_LANG_KK Language::KK
#else
#define KBD_LANG_KK Language::_COUNT
#endif
#ifdef I18N_HAS_HE
#define KBD_LANG_HE Language::HE
#else
#define KBD_LANG_HE Language::_COUNT
#endif

struct LayoutInfo {
  freeink::ui::KeyboardLayoutId id;
  Language language;
};

// Table position is the persisted bit assignment. Append future layouts so
// SDK enum changes cannot reinterpret an existing settings file.
inline constexpr LayoutInfo ALL[] = {
    {freeink::ui::KeyboardLayoutId::QwertyEn, KBD_LANG_EN},
    {freeink::ui::KeyboardLayoutId::AzertyFr, KBD_LANG_FR},
    {freeink::ui::KeyboardLayoutId::QwertzDe, KBD_LANG_DE},
    {freeink::ui::KeyboardLayoutId::SpanishEs, KBD_LANG_ES},
    {freeink::ui::KeyboardLayoutId::CyrillicRu, KBD_LANG_RU},
    {freeink::ui::KeyboardLayoutId::CyrillicUk, KBD_LANG_UK},
    {freeink::ui::KeyboardLayoutId::CyrillicBe, KBD_LANG_BE},
    {freeink::ui::KeyboardLayoutId::CyrillicKk, KBD_LANG_KK},
    {freeink::ui::KeyboardLayoutId::HebrewIl, KBD_LANG_HE},
};
inline constexpr uint8_t COUNT = sizeof(ALL) / sizeof(ALL[0]);
static_assert(COUNT <= 16, "keyboard layout mask is uint16_t");

inline constexpr uint16_t bitAt(const uint8_t i) { return static_cast<uint16_t>(1u << i); }
inline constexpr uint16_t LATIN_BITS = bitAt(0) | bitAt(1) | bitAt(2) | bitAt(3);

uint16_t enabled();
freeink::ui::KeyboardLayoutId startingLayout();
freeink::ui::KeyboardLayoutId next(freeink::ui::KeyboardLayoutId current);

}  // namespace keyboard_layouts
