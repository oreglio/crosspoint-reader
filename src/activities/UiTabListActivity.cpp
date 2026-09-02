#include "UiTabListActivity.h"

#include <GfxRenderer.h>

#include <cassert>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"

namespace fui = freeink::ui;

UiTabListActivity::UiTabListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const bool wantsTouchLongPress)
    : UiListActivity(name, renderer, mappedInput, wantsTouchLongPress) {}

void UiTabListActivity::onEnter() {
  // Size the per-tab state before the base resets activeNav() (which indexes
  // into it).
  tabNavs.assign(static_cast<size_t>(tabCount()), fui::ListNav{});
  UiListActivity::onEnter();
  app.on(ACTION_TAB, &UiTabListActivity::tabActionTrampoline, this);
}

fui::ListNav& UiTabListActivity::activeNav() {
  if (tabNavs.empty()) return nav;  // pre-onEnter fallback
  // Invariant: subclasses keep activeTab() inside [0, tabCount()), and
  // tabCount() does not change after onEnter() sized tabNavs.
  assert(activeTab() >= 0 && static_cast<size_t>(activeTab()) < tabNavs.size());
  return tabNavs[static_cast<size_t>(activeTab())];
}

int UiTabListActivity::ringPos() const {
  if (tabNavs.empty()) return 0;
  assert(activeTab() >= 0 && static_cast<size_t>(activeTab()) < tabNavs.size());
  return tabNavs[static_cast<size_t>(activeTab())].selected;
}

void UiTabListActivity::tabActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiTabListActivity*>(user);
  if (event.value < 0 || event.value >= self->tabCount()) return;
  if (event.longPress) {
    self->onTabLongPress(event.value);
    return;
  }
  self->onTabAction(event.value);
}

void UiTabListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value + 1;  // ring position, not row index
  if (event.longPress) {
    onRowLongPress(event.value);
    return;
  }
  activateIndex(event.value);
}

void UiTabListActivity::moveRingTo(const int ringIndex) {
  auto& n = activeNav();
  n.selected = ringIndex;
  if (ringIndex == 0) {
    n.top = 0;
  } else {
    // Pull the viewport to the row (ring - 1); ListNav::follow reads
    // n.selected as a row index, so compute directly here.
    const uint16_t rows = n.visibleRows > 0 ? static_cast<uint16_t>(n.visibleRows) : 1;
    n.top = fui::listTopIndexFor(static_cast<int16_t>(ringIndex - 1), static_cast<uint16_t>(n.top < 0 ? 0 : n.top),
                                 rows, static_cast<uint16_t>(listCount()));
  }
  requestUpdate();
}

void UiTabListActivity::navigateButtons() {
  // Buttons walk the tab band (index 0) plus the rows (1..listCount).
  const int ringSize = listCount() + 1;
  buttonNavigator.onNextRelease([this, ringSize] { moveRingTo(ButtonNavigator::nextIndex(ringPos(), ringSize)); });
  buttonNavigator.onPreviousRelease(
      [this, ringSize] { moveRingTo(ButtonNavigator::previousIndex(ringPos(), ringSize)); });
  buttonNavigator.onNextContinuous([this] { stepTab(1); });
  buttonNavigator.onPreviousContinuous([this] { stepTab(-1); });
}

void UiTabListActivity::syncTabListViewport(UiScreen& screen, fui::ListProps& props, const bool hasSubtitle) {
  const int count = listCount();
  auto& n = activeNav();
  // Same non-touch density rule as UiListActivity::syncListViewport (the
  // non-tab counterpart of this): uiListRowHeight is the theme token on touch
  // hardware and the denser, UI-scale-aware metric on button devices.
  const auto rowType = hasSubtitle ? UiListRowType::WithSubtitle : UiListRowType::SingleLine;
  const int16_t rowHeight = uiListRowHeight(screen.theme(), rowType);
  props.rowHeight = rowHeight;
  const uint16_t rows = fui::listVisibleRows(screen.body(), rowHeight, screen.theme().listRowGap);
  n.visibleRows = rows > 0 ? rows : 1;
  if (n.followOnBuild) {
    // Screen entry / tab switch: show the tab's remembered selection, or the
    // top when the tab bar holds the focus.
    n.followOnBuild = false;
    n.top = n.selected > 0 ? static_cast<int>(fui::listTopIndexFor(
                                 static_cast<int16_t>(n.selected - 1), static_cast<uint16_t>(n.top < 0 ? 0 : n.top),
                                 static_cast<uint16_t>(n.visibleRows), static_cast<uint16_t>(count)))
                           : 0;
  }
  n.scrollBy(0, count);  // clamp to range
  if (n.selected > count) n.selected = count;
  props.topIndex = static_cast<uint16_t>(n.top);
  props.selectedIndex = static_cast<int16_t>(n.selected - 1);  // -1 = tab band focused
  props.nav = &n;
}

void UiTabListActivity::buildTabBar(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Tabs. The selected pill dims to a dither when the selection is down in
  // the list (the legacy focused/unfocused tab distinction).
  // Stack array, not a heap vector: this runs on every render and the tab
  // count is small and fixed.
  constexpr int MAX_TABS = 8;
  const int count = tabCount() < MAX_TABS ? tabCount() : MAX_TABS;
  fui::TabItem tabs[MAX_TABS];
  for (int i = 0; i < count; i++) {
    tabs[i].label = tabLabel(i);
    tabs[i].icon = tabIcon(i);
    tabs[i].value = static_cast<int16_t>(i);
    tabs[i].selected = activeTab() == i;
  }
  fui::TabBarProps tabProps;
  tabProps.tabs = tabs;
  tabProps.count = static_cast<uint16_t>(count);
  tabProps.action = ACTION_TAB;
  tabProps.inputMask = tabInputMask();
  // Pill shape and label size are theme-driven; the label-hugging treatment
  // keeps wide labels inside their slot at large UI scales.
  const bool tabsFocused = ringPos() == 0;
  tabProps.text = screen.theme().smallText;
  tabProps.tabInset = tabsFocused ? fui::Insets{2, 0, 4, 0} : fui::Insets{2, 0, 0, 0};
  tabProps.contentInset = fui::Insets{2, 8, 2, 8};
  const int16_t tabLineHeight = screen.target().lineHeight(tabProps.text.font);
  const int16_t tabBand = static_cast<int16_t>(tabLineHeight + 10);
  // Two-state treatment: with the selection on the tab band, the active tab is
  // a solid pill; with the selection down in the list, it keeps a light box
  // with an underline.
  tabProps.divider = true;
  fui::StyleSet tabStyles;
  tabStyles.explicitlySet = true;
  tabStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  if (tabsFocused) {
    tabStyles.selected.background = fui::Paint::solid(fui::Color::Black);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
    tabStyles.selected.radius = screen.theme().listRowRadius;
  } else {
    tabStyles.selected.background = fui::Paint::dither(fui::Color::LightGray);
    tabStyles.selected.foreground = fui::Paint::solid(fui::Color::Black);
    tabProps.selectedUnderline = 2;
  }
  // Focus/flash states keep the pill instead of falling back to an unset
  // (blank) style.
  tabStyles.focused = tabStyles.selected;
  tabStyles.active = tabStyles.selected;
  tabProps.tabStyles = tabStyles;
  const fui::Rect tabRect = screen.takeTop(tabBand);
  fui::tabBar(screen.frame(), tabRect, tabProps);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
}
