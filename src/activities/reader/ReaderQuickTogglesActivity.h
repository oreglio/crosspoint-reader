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

class ReaderQuickTogglesActivity final : public Activity {
 public:
  // Matches DictionaryDefinitionActivity's shape: a raw function pointer plus a
  // context, per the no-std::function rule in AGENTS.md.
  using BackgroundRenderFn = void (*)(void* context);

  ReaderQuickTogglesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             void* backgroundContext = nullptr, BackgroundRenderFn backgroundRender = nullptr);

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
  int selected_ = 0;
  bool repaintPagePending_ = true;
  bool dirty_ = false;
};
