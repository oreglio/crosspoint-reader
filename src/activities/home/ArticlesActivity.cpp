#include "ArticlesActivity.h"

#include <FsHelpers.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

constexpr fui::ActionId ACTION_ROW = 1;
constexpr char ARTICLES_DIR[] = "/Articles";
constexpr char INDEX_PATH[] = "/Articles/.index";
constexpr char DONE_DIR[] = "/Articles/.done";
constexpr char DONE_QUEUE[] = "/.crosspoint/raindrop-done.txt";
constexpr unsigned long LONG_CONFIRM_MS = 700;
// Ligne d'index : fichier\ttitre\ttags\toctets\tdate — bornee par les 120
// caracteres de nom cote serveur plus titre/tags assainis.
constexpr size_t INDEX_LINE_MAX = 384;
constexpr size_t MAX_FILTER_TAGS = 18;

// Minuscule ASCII seule : suffisant pour une recherche de titre, les accents
// se comparent tels quels.
char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool containsNoCase(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  if (needle.size() > haystack.size()) return false;
  for (size_t i = 0; i + needle.size() <= haystack.size(); i++) {
    size_t j = 0;
    while (j < needle.size() && asciiLower(haystack[i + j]) == asciiLower(needle[j])) j++;
    if (j == needle.size()) return true;
  }
  return false;
}

// Le champ tags est un CSV : correspondance exacte entre virgules.
bool hasTag(const std::string& csv, const std::string& tag) {
  size_t start = 0;
  while (start <= csv.size()) {
    size_t end = csv.find(',', start);
    if (end == std::string::npos) end = csv.size();
    if (end - start == tag.size() && csv.compare(start, tag.size(), tag) == 0) return true;
    if (end == csv.size()) break;
    start = end + 1;
  }
  return false;
}

}  // namespace

ArticlesActivity::ArticlesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string highlightFile)
    : Activity("Articles", renderer, mappedInput),
      highlightFile(std::move(highlightFile)),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void ArticlesActivity::clearRowCache() {
  for (size_t i = 0; i < ROW_CACHE; i++) rowCacheKeys[i] = UINT32_MAX;
}

void ArticlesActivity::onEnter() {
  Activity::onEnter();
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  clearRowCache();
  loadHiddenNames();
  ensureIndexFile();
  rebuildVisible();

  // Retour de lecture : la selection retombe sur l'article qu'on vient de lire.
  if (!highlightFile.empty()) {
    const size_t slash = highlightFile.find_last_of('/');
    const std::string name = slash == std::string::npos ? highlightFile : highlightFile.substr(slash + 1);
    HalFile file;
    if (Storage.openFileForRead("ART", INDEX_PATH, file)) {
      Row row;
      for (size_t i = 0; i < visible.size(); i++) {
        if (readIndexLine(file, visible[i], row) && row.file == name) {
          selectorIndex = i;
          break;
        }
      }
    }
    highlightFile.clear();
  }

  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &ArticlesActivity::onRowEvent, this);
  app.setScreen(&ArticlesActivity::listScreen, this);
  requestUpdate();
}

void ArticlesActivity::onExit() {
  Activity::onExit();
  visible.clear();
  visible.shrink_to_fit();
  hiddenNames.clear();
  clearRowCache();
}

// L'index vient de la sync ; s'il n'existe pas encore, on le fabrique depuis
// le dossier (titre = nom sans prefixe date ni extension, tags vides). La
// prochaine sync l'ecrasera avec la verite serveur.
bool ArticlesActivity::ensureIndexFile() {
  if (Storage.exists(INDEX_PATH)) return true;

  auto dir = Storage.open(ARTICLES_DIR);
  if (!dir || !dir.isDirectory()) return false;

  // Noms collectes puis tries en ordre inverse : les noms portent la date en
  // prefixe, l'index synthetise garde donc les recents d'abord.
  std::vector<std::string> names;
  std::vector<uint32_t> sizes;
  char nameBuf[160];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(nameBuf, sizeof(nameBuf));
    if (!file.isDirectory() && nameBuf[0] != '.' && FsHelpers::hasMarkdownExtension(nameBuf)) {
      names.emplace_back(nameBuf);
      sizes.push_back(static_cast<uint32_t>(file.size()));
    }
    file.close();
  }
  dir.close();

  std::vector<size_t> order(names.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return names[a] > names[b]; });

  HalFile out;
  if (!Storage.openFileForWrite("ART", INDEX_PATH, out)) {
    LOG_ERR("ART", "Cannot write %s", INDEX_PATH);
    return false;
  }
  for (const size_t i : order) {
    const std::string& n = names[i];
    const bool datePrefixed = n.size() > 11 && n[4] == '-' && n[7] == '-' && n[10] == ' ';
    std::string title = datePrefixed ? n.substr(11) : n;
    if (title.size() > 3) title.resize(title.size() - 3);  // retire ".md"
    char line[INDEX_LINE_MAX];
    const int len = snprintf(line, sizeof(line), "%s\t%s\t\t%lu\t%s\n", n.c_str(), title.c_str(),
                             static_cast<unsigned long>(sizes[i]), datePrefixed ? n.substr(0, 10).c_str() : "");
    if (len > 0) out.write(reinterpret_cast<const uint8_t*>(line), std::min(static_cast<size_t>(len), sizeof(line) - 1));
  }
  return true;
}

// Les articles marques lus vivent dans .done mais restent dans l'index
// serveur jusqu'a leur archivage Raindrop : on les masque par nom.
void ArticlesActivity::loadHiddenNames() {
  hiddenNames.clear();
  auto dir = Storage.open(DONE_DIR);
  if (!dir || !dir.isDirectory()) return;
  char nameBuf[160];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(nameBuf, sizeof(nameBuf));
    if (!file.isDirectory()) hiddenNames.emplace_back(nameBuf);
    file.close();
  }
  dir.close();
  std::sort(hiddenNames.begin(), hiddenNames.end());
}

bool ArticlesActivity::isHidden(const std::string& file) const {
  return std::binary_search(hiddenNames.begin(), hiddenNames.end(), file);
}

// Une passe en flux sur l'index : collecte les offsets des lignes qui passent
// le filtre courant. 45 Ko lus sequentiellement, pas de .md ouverts.
void ArticlesActivity::rebuildVisible() {
  visible.clear();
  clearRowCache();
  selectorIndex = 0;
  topIndex = 0;

  HalFile file;
  if (!Storage.openFileForRead("ART", INDEX_PATH, file)) return;
  const size_t fileSize = file.size();
  visible.reserve(64);

  std::string line;
  line.reserve(INDEX_LINE_MAX);
  uint32_t lineStart = 0;
  uint8_t chunk[512];
  size_t offset = 0;
  while (offset < fileSize) {
    const size_t take = std::min(sizeof(chunk), fileSize - offset);
    if (file.read(chunk, take) != static_cast<int>(take)) break;
    for (size_t i = 0; i < take; i++) {
      const char c = static_cast<char>(chunk[i]);
      if (c != '\n') {
        if (line.size() < INDEX_LINE_MAX) line += c;
        continue;
      }
      // Champs : fichier \t titre \t tags — seuls les trois premiers filtrent.
      const size_t t1 = line.find('\t');
      const size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
      const size_t t3 = t2 == std::string::npos ? std::string::npos : line.find('\t', t2 + 1);
      if (t1 != std::string::npos && t3 != std::string::npos) {
        const std::string fileName = line.substr(0, t1);
        const std::string title = line.substr(t1 + 1, t2 - t1 - 1);
        const std::string tags = line.substr(t2 + 1, t3 - t2 - 1);
        const bool passes = !isHidden(fileName) && (filterTag.empty() || hasTag(tags, filterTag)) &&
                            (searchQuery.empty() || containsNoCase(title, searchQuery));
        if (passes) visible.push_back(lineStart);
      }
      line.clear();
      lineStart = static_cast<uint32_t>(offset + i + 1);
    }
    offset += take;
  }
}

bool ArticlesActivity::readIndexLine(HalFile& file, const uint32_t offset, Row& out) const {
  char buf[INDEX_LINE_MAX];
  if (!file.seekSet(offset)) return false;
  const int got = file.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
  if (got <= 0) return false;
  buf[got] = '\0';
  char* nl = strchr(buf, '\n');
  if (nl != nullptr) *nl = '\0';

  std::string line(buf);
  const size_t t1 = line.find('\t');
  const size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
  const size_t t3 = t2 == std::string::npos ? std::string::npos : line.find('\t', t2 + 1);
  const size_t t4 = t3 == std::string::npos ? std::string::npos : line.find('\t', t3 + 1);
  if (t1 == std::string::npos || t4 == std::string::npos) return false;

  out.file = line.substr(0, t1);
  out.title = line.substr(t1 + 1, t2 - t1 - 1);
  out.tags = line.substr(t2 + 1, t3 - t2 - 1);
  const uint32_t bytes = static_cast<uint32_t>(strtoul(line.c_str() + t3 + 1, nullptr, 10));
  const std::string date = line.substr(t4 + 1);

  // "date · ≈N p. · tags" — memes ~1800 octets par page que le navigateur.
  char sub[96];
  const uint32_t pages = bytes > 0 ? (bytes + 1799) / 1800 : 1;
  snprintf(sub, sizeof(sub), "%s \xc2\xb7 ~%lu p.%s%s", date.c_str(), static_cast<unsigned long>(pages),
           out.tags.empty() ? "" : " \xc2\xb7 ", out.tags.c_str());
  out.subtitle = sub;
  return true;
}

const ArticlesActivity::Row& ArticlesActivity::rowAt(const size_t displayRow) {
  const size_t slot = displayRow % ROW_CACHE;
  const uint32_t key = visible[displayRow];
  if (rowCacheKeys[slot] != key) {
    Row& row = rowCache[slot];
    HalFile file;
    if (!Storage.openFileForRead("ART", INDEX_PATH, file) || !readIndexLine(file, key, row)) {
      row.file.clear();
      row.title = "?";
      row.subtitle.clear();
      row.tags.clear();
    }
    rowCacheKeys[slot] = key;
  }
  return rowCache[slot];
}

void ArticlesActivity::activateSelected() {
  if (lockNextConfirmRelease) {
    lockNextConfirmRelease = false;
    return;
  }
  if (selectorIndex >= visible.size()) return;
  const Row& row = rowAt(selectorIndex);
  if (row.file.empty()) return;
  onSelectBook(std::string(ARTICLES_DIR) + "/" + row.file);
}

void ArticlesActivity::markSelectedDone() {
  if (selectorIndex >= visible.size()) return;
  const Row row = rowAt(selectorIndex);  // copie : la selection va bouger
  if (row.file.empty()) return;

  if (!Storage.exists(DONE_DIR) && !Storage.mkdir(DONE_DIR)) {
    LOG_ERR("ART", "Cannot create %s", DONE_DIR);
    return;
  }
  const std::string src = std::string(ARTICLES_DIR) + "/" + row.file;
  const std::string dst = std::string(DONE_DIR) + "/" + row.file;
  if (!Storage.rename(src.c_str(), dst.c_str())) {
    LOG_ERR("ART", "Cannot move %s to done", row.file.c_str());
    return;
  }

  // File de relais : la prochaine sync archive l'article dans Raindrop.
  // openFileForWrite tronque, donc lecture puis reecriture (meme mecanique
  // que le navigateur de fichiers).
  std::string queue;
  {
    HalFile in;
    if (Storage.openFileForRead("ART", DONE_QUEUE, in)) {
      const size_t size = in.size();
      if (size > 0 && size <= 16384) {
        queue.resize(size);
        if (in.read(reinterpret_cast<uint8_t*>(queue.data()), size) != static_cast<int>(size)) queue.clear();
      }
    }
  }
  queue += row.file;
  queue += '\n';
  HalFile out;
  if (Storage.openFileForWrite("ART", DONE_QUEUE, out)) {
    out.write(reinterpret_cast<const uint8_t*>(queue.c_str()), queue.size());
  } else {
    LOG_ERR("ART", "Cannot persist done queue");
  }

  hiddenNames.insert(std::lower_bound(hiddenNames.begin(), hiddenNames.end(), row.file), row.file);
  const size_t keep = selectorIndex;
  rebuildVisible();
  selectorIndex = std::min(keep, visible.empty() ? 0 : visible.size() - 1);
  requestUpdate();
}

// Menu unique sur appui long : action sur l'article + entrees de filtre.
void ArticlesActivity::openActionsMenu() {
  std::vector<std::string> options;
  options.reserve(4);
  options.emplace_back(tr(STR_RAINDROP_MARK_DONE));
  options.emplace_back(tr(STR_ARTICLES_FILTER_TAG));
  options.emplace_back(tr(STR_ARTICLES_SEARCH));
  options.emplace_back(tr(STR_ARTICLES_ALL));
  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "ArticlesMenu", StrId::STR_ARTICLES_TITLE,
                                                std::move(options), 0),
      [this](const ActivityResult& result) {
        const auto* choice = std::get_if<OptionSelectionResult>(&result.data);
        if (result.isCancelled || choice == nullptr) {
          requestUpdate();
          return;
        }
        switch (choice->index) {
          case 0:
            markSelectedDone();
            break;
          case 1:
            openTagFilterMenu();
            return;
          case 2:
            openSearch();
            return;
          case 3:
            filterTag.clear();
            searchQuery.clear();
            rebuildVisible();
            break;
          default:
            break;
        }
        requestUpdate();
      });
}

// Tags distincts + compteurs en une passe sur l'index (articles visibles ou
// non : le filtre montre tout ce qui existe).
void ArticlesActivity::openTagFilterMenu() {
  std::vector<std::string> tags;
  std::vector<uint16_t> counts;

  HalFile file;
  if (Storage.openFileForRead("ART", INDEX_PATH, file)) {
    const size_t fileSize = file.size();
    std::string line;
    line.reserve(INDEX_LINE_MAX);
    uint8_t chunk[512];
    size_t offset = 0;
    while (offset < fileSize) {
      const size_t take = std::min(sizeof(chunk), fileSize - offset);
      if (file.read(chunk, take) != static_cast<int>(take)) break;
      for (size_t i = 0; i < take; i++) {
        const char c = static_cast<char>(chunk[i]);
        if (c != '\n') {
          if (line.size() < INDEX_LINE_MAX) line += c;
          continue;
        }
        const size_t t1 = line.find('\t');
        const size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
        const size_t t3 = t2 == std::string::npos ? std::string::npos : line.find('\t', t2 + 1);
        if (t1 != std::string::npos && t3 != std::string::npos && !isHidden(line.substr(0, t1))) {
          size_t start = t2 + 1;
          while (start < t3) {
            size_t end = line.find(',', start);
            if (end == std::string::npos || end > t3) end = t3;
            if (end > start) {
              const std::string tag = line.substr(start, end - start);
              const auto it = std::find(tags.begin(), tags.end(), tag);
              if (it != tags.end()) {
                counts[static_cast<size_t>(it - tags.begin())]++;
              } else if (tags.size() < MAX_FILTER_TAGS) {
                tags.push_back(tag);
                counts.push_back(1);
              }
            }
            start = end + 1;
          }
        }
        line.clear();
      }
      offset += take;
    }
  }

  std::vector<std::string> options;
  options.reserve(tags.size() + 1);
  options.emplace_back(tr(STR_ARTICLES_ALL));
  for (size_t i = 0; i < tags.size(); i++) {
    char label[96];
    snprintf(label, sizeof(label), "%s (%u)", tags[i].c_str(), counts[i]);
    options.emplace_back(label);
  }
  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "ArticlesTags",
                                                StrId::STR_ARTICLES_FILTER_TAG, std::move(options), 0),
      [this, tags](const ActivityResult& result) {
        const auto* choice = std::get_if<OptionSelectionResult>(&result.data);
        if (!result.isCancelled && choice != nullptr) {
          filterTag = choice->index == 0 || choice->index > tags.size() ? std::string() : tags[choice->index - 1];
          rebuildVisible();
        }
        requestUpdate();
      });
}

void ArticlesActivity::openSearch() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ARTICLES_SEARCH),
                                                                 searchQuery, 48, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             searchQuery = std::get<KeyboardResult>(result.data).text;
                             rebuildVisible();
                           }
                           requestUpdate();
                         });
}

void ArticlesActivity::onRowEvent(const freeink::ui::ActionEvent& event, void* user) {
  auto* self = static_cast<ArticlesActivity*>(user);
  if (event.value < 0 || static_cast<size_t>(event.value) >= self->visible.size()) return;
  self->selectorIndex = static_cast<size_t>(event.value);
  if (event.longPress) {
    self->openActionsMenu();
    return;
  }
  self->activateSelected();
}

void ArticlesActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<ArticlesActivity*>(user)->buildListScreen(screen);
}

void ArticlesActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Bande du bas : filtre/recherche actifs, sinon le compte d'articles.
  {
    const int bandLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(bandLineHeight + metrics.verticalSpacing));
    screen.target().fill(fui::Rect{band.x, band.y, band.width, 3}, fui::Paint::solid(fui::Color::Black));
    char status[112];
    if (!filterTag.empty() || !searchQuery.empty()) {
      snprintf(status, sizeof(status), "%s%s%s%s \xc2\xb7 %u", filterTag.c_str(),
               (!filterTag.empty() && !searchQuery.empty()) ? " \xc2\xb7 " : "", searchQuery.c_str(),
               searchQuery.empty() ? "" : "?", static_cast<unsigned>(visible.size()));
    } else {
      snprintf(status, sizeof(status), "%u %s", static_cast<unsigned>(visible.size()), tr(STR_ARTICLES_TITLE));
    }
    const int y =
        band.y + metrics.verticalSpacing / 2 + (band.height - metrics.verticalSpacing / 2 - bandLineHeight) / 2;
    renderer.drawText(SMALL_FONT_ID, band.x + metrics.contentSidePadding, y, status);
  }

  if (visible.empty()) {
    screen.centeredText(tr(STR_ARTICLES_EMPTY), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  const fui::Rect listRect = screen.body();
  const auto rows = configureUiList(props, screen.theme(), listRect, UiListRowType::WithSubtitle);
  visibleRowCount = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRowCount, static_cast<int>(visible.size()));

  const size_t drawCount = std::min<size_t>(visibleRowCount, visible.size() - static_cast<size_t>(topIndex));
  std::vector<fui::ListItem> items;
  items.reserve(drawCount);
  for (size_t i = 0; i < drawCount; i++) {
    const size_t displayRow = static_cast<size_t>(topIndex) + i;
    const Row& row = rowAt(displayRow);
    fui::ListItem item;
    item.label = row.title.c_str();
    if (!row.subtitle.empty()) item.subtitle = row.subtitle.c_str();
    item.actionValue = static_cast<int16_t>(displayRow);
    items.push_back(item);
  }

  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex =
      selectorIndex >= static_cast<size_t>(topIndex) && selectorIndex < static_cast<size_t>(topIndex) + drawCount
          ? static_cast<int16_t>(selectorIndex - static_cast<size_t>(topIndex))
          : -1;
  props.action = ACTION_ROW;
  props.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);
  props.topIndex = 0;
  screen.list(props);
  fui::drawListScrollIndicator(screen.target(), listRect, visible.size(), visibleRowCount, topIndex,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}

void ArticlesActivity::loop() {
  // Retour : Home avec la selection sur l'entree Raindrop Sync du menu.
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    onGoHome(HomeMenuItem::RAINDROP);
    return;
  }
  // Le tactile passe par la FreeInkApp : render() a enregistre les rectangles
  // de lignes, on route l'instantane et onRowEvent dispatche.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  if (longPressConfirmHandled) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressConfirmHandled = false;
    }
    return;
  }
  if (!visible.empty() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_CONFIRM_MS) {
    longPressConfirmHandled = true;
    openActionsMenu();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::RAINDROP);
    return;
  }

  const int listSize = static_cast<int>(visible.size());
  if (listSize <= 0) return;

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRowCount : -visibleRowCount;
    const int next = scrollListBy(topIndex, delta, visibleRowCount, listSize);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto moveSelection = [this, listSize](const int index) {
    selectorIndex = static_cast<size_t>(index);
    topIndex = followListSelection(static_cast<int>(selectorIndex), topIndex, visibleRowCount, listSize);
    requestUpdate();
  };
  buttonNavigator.onNextRelease(
      [this, listSize, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize)); });
  buttonNavigator.onPreviousRelease(
      [this, listSize, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize)); });
  buttonNavigator.onNextContinuous([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, visibleRowCount));
  });
  buttonNavigator.onPreviousContinuous([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, visibleRowCount));
  });
}

void ArticlesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_ARTICLES_TITLE), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_ARTICLES_TITLE));
  }
  uiReady = false;
  app.render();
  uiReady = true;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), visible.empty() ? "" : tr(STR_OPEN), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
