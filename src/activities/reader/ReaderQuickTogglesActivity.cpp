#include "ReaderQuickTogglesActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

const ReaderQuickTogglesActivity::Toggle ReaderQuickTogglesActivity::TOGGLES[] = {
    {StrId::STR_FONT_SIZE, nullptr, true},
    {StrId::STR_READER_DARK_MODE, &CrossPointSettings::readerDarkMode, true},
    {StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, true},
    {StrId::STR_GUIDE_READING, &CrossPointSettings::guideReadingEnabled, false},
    // Status-bar contents. None of these touch the text, so the page under the
    // panel stays valid and only the panel repaints.
    {StrId::STR_PUBLISHER_PAGE_NUMBERS, &CrossPointSettings::publisherPageNumbers, true},
    {StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount, true},
    {StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage, true},
#if CROSSINK_APP_CAP_TOUCH
    // X3 and X4 have no touchscreen, so this row would be dead weight there.
    {StrId::STR_DISABLE_TOUCHSCREEN, &CrossPointSettings::disableReaderTouchscreen, false},
#endif
};

const int ReaderQuickTogglesActivity::TOGGLE_COUNT =
    static_cast<int>(sizeof(TOGGLES) / sizeof(TOGGLES[0]));

namespace {
constexpr int PANEL_PADDING = 12;
constexpr int ROW_PADDING = 8;
}  // namespace

ReaderQuickTogglesActivity::ReaderQuickTogglesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       void* backgroundContext, BackgroundRenderFn backgroundRender,
                                                       FontStepFn fontStep, SaveSettingsFn saveSettings)
    : Activity("ReaderQuickToggles", renderer, mappedInput),
      backgroundContext_(backgroundContext),
      backgroundRender_(backgroundRender),
      fontStep_(fontStep),
      saveSettings_(saveSettings) {}

void ReaderQuickTogglesActivity::onEnter() {
  Activity::onEnter();
  requestUpdate(true);
}

int ReaderQuickTogglesActivity::rowHeight() const { return renderer.getLineHeight(UI_12_FONT_ID) + ROW_PADDING * 2; }

int ReaderQuickTogglesActivity::panelTop() const {
  const int height = TOGGLE_COUNT * rowHeight() + PANEL_PADDING * 2;
  const int top = renderer.getScreenHeight() - height;
  return top > 0 ? top : 0;
}

void ReaderQuickTogglesActivity::drawPanel() const {
  const int top = panelTop();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight() - top;

  // Opaque: the page underneath is covered rather than blended, which is what
  // lets a toggle repaint the panel alone without restoring page pixels.
  renderer.fillRect(0, top, width, height, false);
  renderer.drawLine(0, top, width - 1, top, true);

  int y = top + PANEL_PADDING;
  for (int i = 0; i < TOGGLE_COUNT; i++) {
    const bool isSelected = (i == selected_);
    const int rowTop = y;
    const int rh = rowHeight();

    if (isSelected) {
      renderer.fillRect(PANEL_PADDING / 2, rowTop, width - PANEL_PADDING, rh, true);
    }

    const char* label = I18N.get(TOGGLES[i].label);
    char sizeText[12];
    const char* state;
    if (TOGGLES[i].field == nullptr) {
      // Font row: show the point size, with arrows hinting Left/Right.
      snprintf(sizeText, sizeof(sizeText), "< %u pt >", static_cast<unsigned>(SETTINGS.readerFontPointSize));
      state = sizeText;
    } else {
      state = I18N.get(SETTINGS.*(TOGGLES[i].field) != 0 ? StrId::STR_ON : StrId::STR_OFF);
    }

    const int textY = rowTop + ROW_PADDING;
    // Selected row is inverted, so its text draws in the opposite colour.
    renderer.drawText(UI_12_FONT_ID, PANEL_PADDING, textY, label, !isSelected);
    const int stateWidth = renderer.getTextWidth(UI_12_FONT_ID, state);
    renderer.drawText(UI_12_FONT_ID, width - PANEL_PADDING - stateWidth, textY, state, !isSelected);

    y += rh;
  }
}

void ReaderQuickTogglesActivity::render(RenderLock&&) {
  if (repaintPagePending_) {
    if (backgroundRender_) {
      backgroundRender_(backgroundContext_);
    } else {
      renderer.clearScreen();
    }
    repaintPagePending_ = false;
  }
  drawPanel();
  // FAST_REFRESH: the panel is a small region and the page under it is
  // unchanged, so a full waveform would sweep the whole panel for nothing.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void ReaderQuickTogglesActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // One write for the whole visit rather than one per toggle: flipping four
    // rows should not cost four SD writes (see the debounce rule in AGENTS.md).
    if (dirty_) {
      if (saveSettings_) {
        saveSettings_(backgroundContext_);
      } else {
        LOG_ERR("RQT", "no save hook: quick toggles not persisted");
      }
      dirty_ = false;
    }
    // The reader repaints its own page on resume; nothing to restore here.
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    selected_ = (selected_ + 1) % TOGGLE_COUNT;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    selected_ = (selected_ + TOGGLE_COUNT - 1) % TOGGLE_COUNT;
    requestUpdate();
    return;
  }

  // Font row steps with Left/Right. The reader's own callback persists the new
  // size and reindexes the current section, so the page under the panel has to
  // be redrawn once it returns.
  if (TOGGLES[selected_].field == nullptr && fontStep_) {
    const bool smaller = mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool larger = mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (smaller || larger) {
      fontStep_(backgroundContext_, larger);
      // Deliberately no page repaint: the size change has already invalidated
      // the reader's layout, so reloading the page here finds nothing and
      // clears to white. The stale text above the panel stays put until the
      // reader reflows it once, on close.
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const Toggle& toggle = TOGGLES[selected_];
    if (toggle.field == nullptr) return;  // font row is stepped, not toggled
    SETTINGS.*(toggle.field) = SETTINGS.*(toggle.field) ? 0 : 1;
    dirty_ = true;
    // Only dark mode and anti-aliasing change the page itself; the rest leave
    // the pixels under the panel valid, so we repaint the panel alone.
    repaintPagePending_ = toggle.repaintsPage;
    requestUpdate();
  }
}
