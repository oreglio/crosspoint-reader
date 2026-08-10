#pragma once

// A quick-toggle drawer drawn over the reader page.
//
// It carries only settings that take effect immediately. Font size, spacing and
// margins all reflow the book -- the long "indexing" pass -- which has no place
// behind a control you flip mid-paragraph. Those stay in ReaderOptionsActivity.
//
// No framebuffer snapshot. inx and CrumBLE, where this idea comes from, copy the
// whole 48000-byte framebuffer so they can repaint the drawer without losing the
// page under it. On a C3 the largest allocatable block mid-read is about 49 KB
// (see .claude/CONTEXT.md), so that allocation sits at ~98% of what is available
// and fails on a fragmented heap -- CrumBLE ships a fallback path for exactly
// that. This drawer instead asks the caller to repaint the page, through the
// same BackgroundRenderFn the dictionary modal already uses, and allocates
// nothing at all. The panel is opaque and fixed height, so toggling a row that
// does not change the page repaints only the panel.

#include <cstdint>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "activities/Activity.h"
#include "util/HoldOpenReleaseLock.h"

class ReaderQuickTogglesActivity final : public Activity {
 public:
  // Matches DictionaryDefinitionActivity's shape: a raw function pointer plus a
  // context, per the no-std::function rule in AGENTS.md.
  using BackgroundRenderFn = void (*)(void* context);
  // Steps the reader font one size and reindexes the current section only --
  // the reader already does this for its side-button gesture, so the drawer
  // borrows the same path rather than reflowing the whole book.
  using FontStepFn = void (*)(void* context, bool larger);
  // Writes the toggles to disk. Persisting is the reader's job, not the
  // drawer's: a bare SETTINGS.saveToFile() here saves whatever the CURRENT
  // BOOK's overrides left in SETTINGS as the global defaults, so opening a
  // book with a per-book font or dark-mode override and flipping one toggle
  // rewrote the defaults for every other book. The reader hands over
  // saveGlobalSettingsPreservingBookOverrides(), which puts the pre-book
  // globals back around the write. Same shape as the font-step hook.
  using SaveSettingsFn = void (*)(void* context);

  // No defaulted arguments: every hook is required. The drawer cannot do any
  // of these three jobs itself, and a caller that silently got nullptr for the
  // save hook would be back to losing the user's toggles.
  ReaderQuickTogglesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, void* backgroundContext,
                             BackgroundRenderFn backgroundRender, FontStepFn fontStep, SaveSettingsFn saveSettings);

  void onEnter() override;
  void render(RenderLock&&) override;
  void loop() override;

  // The drawer belongs to the reader: it must keep reader touch handling and
  // the power-as-confirm mapping rather than behave like a settings screen.
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  struct Toggle {
    StrId label;
    // nullptr marks the font-size row: it steps a value with Left/Right rather
    // than flipping a boolean with Confirm.
    uint8_t CrossPointSettings::*field;
    // Dark mode and anti-aliasing change how the page itself is drawn, so the
    // page has to be repainted under the panel. The others only change future
    // behaviour and leave the visible page alone.
    bool repaintsPage;
  };

  // A fixed table rather than the SettingInfo machinery: building that list
  // allocates a vector of settings each holding std::strings, which is what
  // ran the drawer out of memory in CrumBLE. Pointer-to-member costs nothing.
  static const Toggle TOGGLES[];
  static const int TOGGLE_COUNT;

  void drawPanel() const;
  int panelTop() const;
  int rowHeight() const;

  void* backgroundContext_ = nullptr;
  BackgroundRenderFn backgroundRender_ = nullptr;
  FontStepFn fontStep_ = nullptr;
  SaveSettingsFn saveSettings_ = nullptr;
  int selected_ = 0;
  bool repaintPagePending_ = true;
  bool dirty_ = false;
  // The drawer opens on a HOLD of one of the two button pairs, and every one of
  // those four buttons does something here on release.
  HoldOpenReleaseLock openingGestureLock_;
};
