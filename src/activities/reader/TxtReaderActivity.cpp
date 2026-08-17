#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "MappedInputManager.h"
#include "QuickActions.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/boot_sleep/SleepCoverAssets.h"
#include "activities/home/FileBrowserActionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading

// Which side-button long-press actions THIS reader dispatches. Exhaustive on
// purpose: -Werror=switch turns the next action added to SIDE_LONG_PRESS into a
// build failure here, where the dispatch lives, instead of a setting that is
// offered in Settings and quietly does nothing. Answering false is not free --
// see detectPageTurn: an unhandled action used to cost the side page-turners
// their press-time response for a gesture that did nothing.
bool handlesSideLongPress(const CrossPointSettings::SIDE_LONG_PRESS action) {
  switch (action) {
    case CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_ORIENTATION_CHANGE:
    case CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_LIBRARY:
      return true;
    case CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_CHAPTER_SKIP:
    case CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_FONT_SIZE:
    case CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_OFF:
    case CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_QUICK_TOGGLES:
    case CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_PRESS_COUNT:
      return false;
  }
  return false;
}

// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 4;          // Increment when cache format changes
constexpr uint32_t MAX_CACHE_PAGES = 65535;   // Sanity cap to prevent unbounded reserve()

// --- Markdown-lite ----------------------------------------------------------
// Les .md affiches ici sortent du turndown du serveur CrossDrop : titres ATX,
// puces '-', citations '> ', fences ```, liens [libelle](url), gras **,
// italique _, ponctuation significative echappee par '\'. Cette grammaire est
// strictement par LIGNE source, donc la classification et le nettoyage se font
// pendant le wrap, sans parseur global ni etat entre pages. Seules les fences
// demanderaient un etat inter-page : elles sont rendues comme des filets et
// leur contenu comme du texte, compromis assume.
// Le style d'une ligne visuelle voyage en bande : premier octet dans
// [0x01,0x07] (jamais present dans du texte — la tabulation est 0x09).
enum MdStyle : uint8_t {
  MD_BODY = 0,
  MD_HEADING = 0x01,    // gras, pleine largeur
  MD_QUOTE = 0x02,      // italique, indente
  MD_LIST_CONT = 0x03,  // continuation d'un item de liste : indente
  MD_RULE = 0x04,       // filet horizontal, aucun texte
};

struct MdBlock {
  MdStyle first = MD_BODY;  // premiere ligne visuelle du bloc
  MdStyle cont = MD_BODY;   // lignes de continuation apres cesure
  size_t prefixLen = 0;     // octets du marqueur bloc a retirer
  bool isRule = false;
};

// hr : '---' (nos en-tetes serveur) ou '* * *' (defaut turndown), tolerant.
bool mdIsRuleLine(const std::string& s) {
  char marker = 0;
  int count = 0;
  for (const char c : s) {
    if (c == ' ' || c == '\t') continue;
    if (c != '-' && c != '*' && c != '_') return false;
    if (marker == 0) marker = c;
    if (c != marker) return false;
    count++;
  }
  return count >= 3;
}

MdBlock mdClassify(const std::string& line) {
  MdBlock b;
  if (line.empty()) return b;
  if (line[0] == '#') {
    size_t n = 0;
    while (n < line.size() && line[n] == '#') n++;
    if (n <= 6 && n < line.size() && line[n] == ' ') {
      b.first = b.cont = MD_HEADING;
      b.prefixLen = n + 1;
      return b;
    }
  }
  if (line[0] == '>') {
    b.first = b.cont = MD_QUOTE;
    b.prefixLen = (line.size() > 1 && line[1] == ' ') ? 2 : 1;
    return b;
  }
  if (mdIsRuleLine(line) || line.rfind("```", 0) == 0 || line.rfind("~~~", 0) == 0) {
    b.isRule = true;
    return b;
  }
  // Puces et listes numerotees : le marqueur reste affiche tel quel, seules
  // les lignes de continuation s'indentent sous le texte.
  if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
    b.cont = MD_LIST_CONT;
    return b;
  }
  size_t d = 0;
  while (d < line.size() && line[d] >= '0' && line[d] <= '9') d++;
  if (d > 0 && d + 1 < line.size() && line[d] == '.' && line[d + 1] == ' ') {
    b.cont = MD_LIST_CONT;
    return b;
  }
  return b;
}

// Retire les marqueurs inline (liens -> libelle, **, _, `, ~~, echappements)
// en gardant la correspondance transformee -> source : une coupure de page en
// plein paragraphe doit retomber sur un octet SOURCE. anchors = debuts de runs
// (position transformee, position source relative au debut de ligne source).
std::string mdStripInline(const std::string& src, size_t prefixLen,
                          std::vector<std::pair<uint32_t, uint32_t>>& anchors) {
  std::string out;
  out.reserve(src.size());
  anchors.clear();
  anchors.emplace_back(0, static_cast<uint32_t>(prefixLen));
  const auto anchorAt = [&](size_t srcPos) {
    anchors.emplace_back(static_cast<uint32_t>(out.size()), static_cast<uint32_t>(prefixLen + srcPos));
  };
  size_t i = 0;
  while (i < src.size()) {
    const char c = src[i];
    const char next = (i + 1 < src.size()) ? src[i + 1] : '\0';
    if (c == '\\' && next != '\0' && next > 0x20 && next < 0x7F && !isalnum(static_cast<unsigned char>(next))) {
      out += next;  // turndown echappe la ponctuation significative du texte
      i += 2;
      anchorAt(i);
      continue;
    }
    if (c == '*' || c == '_' || c == '`') {
      i++;  // marqueur d'emphase/code : les litteraux arrivent echappes
      anchorAt(i);
      continue;
    }
    if (c == '~' && next == '~') {
      i += 2;  // ~~barre~~ (gfm) ; un '~' isole ("~30 min") reste litteral
      anchorAt(i);
      continue;
    }
    if (c == '[') {
      // Ouverture de lien seulement si "](" puis ')' suivent sur la ligne ;
      // sinon litteral (ex. "[alt]" des images aplaties par le serveur).
      const size_t close = src.find("](", i + 1);
      if (close != std::string::npos && src.find(')', close + 2) != std::string::npos) {
        i++;
        anchorAt(i);
        continue;
      }
    }
    if (c == ']' && next == '(') {
      const size_t paren = src.find(')', i + 2);
      if (paren != std::string::npos) {
        i = paren + 1;  // saute "](url)"
        anchorAt(i);
        continue;
      }
    }
    out += c;
    i++;
  }
  return out;
}

size_t mdMapToSource(const size_t tpos, const std::vector<std::pair<uint32_t, uint32_t>>& anchors) {
  for (auto it = anchors.rbegin(); it != anchors.rend(); ++it) {
    if (it->first <= tpos) return it->second + (tpos - it->first);
  }
  return tpos;
}

// Prefixe le style en bande ; MD_BODY reste une chaine nue.
std::string mdStyled(const MdStyle style, std::string text) {
  if (style == MD_BODY) return text;
  text.insert(text.begin(), static_cast<char>(style));
  return text;
}

// Parses and word-wraps lines from a file chunk into outLines.
// Returns the number of bytes consumed from the start of buffer.
// En mode markdown, chaque ligne visuelle peut porter un octet de style en
// bande (voir MdStyle) et mdIndent retrecit les lignes indentees.
size_t parseAndWrapLines(const uint8_t* buffer, size_t chunkSize, size_t fileOffset, size_t fileSize, int linesPerPage,
                         GfxRenderer& renderer, int fontId, int vw, std::vector<std::string>& outLines,
                         bool markdown = false, int mdIndent = 0) {
  size_t pos = 0;
  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') lineEnd++;
    bool lineComplete = (lineEnd < chunkSize) || (fileOffset + lineEnd >= fileSize);
    if (!lineComplete && !outLines.empty()) break;

    size_t lineContentLen = lineEnd - pos;
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;
    std::string line(reinterpret_cast<const char*>(buffer + pos), displayLen);
    size_t lineBytePos = 0;

    MdBlock block;
    std::vector<std::pair<uint32_t, uint32_t>> anchors;
    bool firstVisualLine = true;
    if (markdown) {
      block = mdClassify(line);
      if (block.isRule) {
        outLines.push_back(mdStyled(MD_RULE, std::string()));
        pos = lineEnd + 1;
        continue;
      }
      line = mdStripInline(line.substr(block.prefixLen), block.prefixLen, anchors);
    }
    const auto styleOf = [&] { return firstVisualLine ? block.first : block.cont; };
    const auto widthOf = [&] {
      const MdStyle s = styleOf();
      return (s == MD_QUOTE || s == MD_LIST_CONT) ? vw - mdIndent : vw;
    };
    const auto measure = [&](const std::string& s) {
      return markdown ? renderer.getTextAdvanceX(fontId, s.c_str(),
                                                 styleOf() == MD_HEADING ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR)
                      : renderer.getTextWidth(fontId, s.c_str());
    };

    do {
      if (line.empty()) {
        outLines.emplace_back();
        break;
      }

      if (measure(line) <= widthOf()) {
        outLines.push_back(markdown ? mdStyled(styleOf(), line) : line);
        lineBytePos = displayLen;
        line.clear();
        break;
      }
      // Forward word accumulation: measure growing prefixes up to the first
      // word that overflows. Cost is bounded by the screen width, not the
      // paragraph length — the old back-from-the-end scan re-measured near
      // full-line prefixes per step, which never finished on the single-line
      // multi-thousand-character paragraphs Markdown articles are made of.
      size_t breakPos = 0;
      size_t candidate = 0;
      while (true) {
        const size_t nextSpace = line.find(' ', candidate);
        const size_t end = (nextSpace == std::string::npos) ? line.length() : nextSpace;
        if (measure(line.substr(0, end)) > widthOf()) break;
        breakPos = end;
        if (nextSpace == std::string::npos) break;
        candidate = nextSpace + 1;
      }
      if (breakPos == 0) {
        // First word alone is wider than the screen: cut at the last
        // codepoint boundary that still fits.
        size_t end = 1;
        while (end < line.length() && (line[end] & 0xC0) == 0x80) end++;
        size_t lastFit = end;
        while (end <= line.length()) {
          if (measure(line.substr(0, end)) > widthOf()) break;
          lastFit = end;
          end++;
          while (end < line.length() && (line[end] & 0xC0) == 0x80) end++;
        }
        breakPos = lastFit;
      }
      outLines.push_back(markdown ? mdStyled(styleOf(), line.substr(0, breakPos)) : line.substr(0, breakPos));
      firstVisualLine = false;
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') skipChars++;
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    if (line.empty()) {
      pos = lineEnd + 1;
    } else {
      // En markdown le wrap a couru sur le texte transforme : la coupure de
      // page doit retomber sur l'octet SOURCE correspondant.
      pos = pos + (markdown ? mdMapToSource(lineBytePos, anchors) : lineBytePos);
      break;
    }
  }
  if (pos == 0 && !outLines.empty()) {
    pos = 1;
  }
  return pos;
}

int getReaderLineHeight(const GfxRenderer& renderer, const int fontId) {
  return std::max(1, static_cast<int>(renderer.getLineHeight(fontId) * SETTINGS.getReaderLineCompression() + 0.5f));
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const bool toastBackgroundBlack = ReaderUtils::readerForegroundBlack();
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, toastBackgroundBlack);
  renderer.drawRect(toastX, toastY, toastW, toastH, !toastBackgroundBlack);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, !toastBackgroundBlack);
  renderer.displayBuffer();
}
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  sdFontSystem.ensureLoaded(renderer);
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Activate reader-specific front button mapping (if configured).
  mappedInput.setReaderMode(true);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  SleepCoverAssets::prepareTxt(*txt);
  const std::string coverBmpPath = Storage.exists(txt->getCoverBmpPath().c_str()) ? txt->getCoverBmpPath() : "";
  RECENT_BOOKS.addOrUpdateBook(filePath, fileName, "", coverBmpPath);

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  // Deactivate reader-specific front button mapping.
  mappedInput.setReaderMode(false);

  if (!flushQueuedProgress()) {
    LOG_ERR("TRS", "Failed to flush debounced reader progress on exit");
  }

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::openReaderMenu() {
  if (!txt) return;
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.push_back({FileBrowserAction::SendNearby, StrId::STR_SEND_NEARBY_BOOK});
  auto menu = makeUniqueNoThrow<FileBrowserActionActivity>(renderer, mappedInput, txt->getTitle(), std::move(items));
  if (!menu) {
    LOG_ERR("NBOOK", "OOM: TXT nearby transfer menu");
    return;
  }
  startActivityForResult(std::move(menu), [this](const ActivityResult& result) {
    const auto* action = std::get_if<FileBrowserActionResult>(&result.data);
    if (!result.isCancelled && action &&
        static_cast<FileBrowserAction>(action->action) == FileBrowserAction::SendNearby) {
      saveProgress(currentPage);
      activityManager.goToNearbyBookSend(txt ? txt->getPath() : std::string{}, true);
    } else {
      requestUpdate();
    }
  });
}

void TxtReaderActivity::loop() {
  if (quickActionsPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (consumeLongPowerButtonRelease()) {
    return;
  }
  if (executePowerButtonAction()) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openReaderMenu();
    return;
  }

  if (longPressBackHandled) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        !mappedInput.isPressed(MappedInputManager::Button::Back)) {
      longPressBackHandled = false;
    }
    return;
  }

  if (!longPressBackHandled && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    longPressBackHandled = true;
    mappedInput.suppressNextBackRelease();
    executeLongPressBackAction();
    return;
  }

  // Short press BACK goes directly to home. Articles instead return to their
  // list: the browser extracts the folder from the full path and selects the
  // article that was just read. (goHome() clears returnToArticlesOnReaderExit,
  // so routing must happen here, not in the ActivityManager pop path.)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (txt && txt->getPath().rfind("/Articles/", 0) == 0) {
      activityManager.goToFileBrowser(txt->getPath());
    } else {
      onGoHome();
    }
    return;
  }

  // The Library is format-agnostic, so the side gesture that opens it in the
  // EPUB reader opens it here too. Without this the setting was offered in
  // Settings, took effect in one reader out of three, and still cost the side
  // page-turners their press-time response (see detectPageTurn) everywhere.
  const bool sideLongPressChangesOrientation =
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_ORIENTATION_CHANGE;
  const bool sideLongPressOpensLibrary =
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_LIBRARY;
  if (sideLongPressChangesOrientation || sideLongPressOpensLibrary) {
    const bool topReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool bottomReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (sideButtonLongPressHandled && (topReleased || bottomReleased)) {
      sideButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool topLongPressed =
        longPressReady && (mappedInput.isPressed(MappedInputManager::Button::Up) || topReleased);
    const bool bottomLongPressed =
        longPressReady && (mappedInput.isPressed(MappedInputManager::Button::Down) || bottomReleased);

    // Direction carries no meaning for the Library, so either side button opens it.
    if (!sideButtonLongPressHandled && sideLongPressOpensLibrary && (topLongPressed || bottomLongPressed)) {
      sideButtonLongPressHandled = !(topReleased || bottomReleased);
      activityManager.goToLibrary();
      return;
    }

    if (!sideButtonLongPressHandled && sideLongPressChangesOrientation && (topLongPressed || bottomLongPressed)) {
      sideButtonLongPressHandled = !(topReleased || bottomReleased);
      SETTINGS.orientation = ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/bottomLongPressed);
      SETTINGS.saveGlobalDefaults();
      {
        RenderLock lock(*this);
        ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
        pageOffsets.clear();
        currentPageLines.clear();
        initialized = false;
      }
      requestUpdate();
      return;
    }
  }

  const bool frontLongPressChangesFont = SETTINGS.longPressButtonBehavior == CrossPointSettings::FONT_SIZE_CHANGE;
  const bool frontLongPressOpensLibrary = SETTINGS.longPressButtonBehavior == CrossPointSettings::LIBRARY;
  if (SETTINGS.longPressButtonBehavior == CrossPointSettings::ORIENTATION_CHANGE || frontLongPressChangesFont ||
      frontLongPressOpensLibrary) {
    const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (frontButtonLongPressHandled && (leftReleased || rightReleased)) {
      frontButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool prevLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool nextLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!frontButtonLongPressHandled && (prevLongPressed || nextLongPressed)) {
      frontButtonLongPressHandled = true;
      if (frontLongPressOpensLibrary) {
        activityManager.goToLibrary();
        return;
      }
      if (frontLongPressChangesFont) {
        if (sdFontSystem.changeReaderFontSize(/*larger=*/nextLongPressed)) {
          SETTINGS.saveGlobalDefaults();
          sdFontSystem.ensureLoaded(renderer);
          {
            RenderLock lock(*this);
            pageOffsets.clear();
            currentPageLines.clear();
            initialized = false;
          }
          requestUpdate();
        }
        return;
      }

      SETTINGS.orientation = ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/prevLongPressed);
      SETTINGS.saveGlobalDefaults();
      {
        RenderLock lock(*this);
        ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
        pageOffsets.clear();
        currentPageLines.clear();
        initialized = false;
      }
      requestUpdate();
      return;
    }
  }

  auto [prevTriggered, nextTriggered, fromSideBtn, fromTilt] = ReaderUtils::detectPageTurn(
      mappedInput,
      handlesSideLongPress(static_cast<CrossPointSettings::SIDE_LONG_PRESS>(SETTINGS.sideButtonLongPress)));
  (void)fromSideBtn;
  (void)fromTilt;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestUpdate();
    }
  }
}

void TxtReaderActivity::toggleDarkMode() {
  SETTINGS.readerDarkMode = !SETTINGS.readerDarkMode;
  SETTINGS.saveGlobalDefaults();
  requestUpdate();
}

bool TxtReaderActivity::consumeLongPowerButtonRelease() {
  if (!longPowerButtonHandled) {
    return false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) ||
      !mappedInput.isPressed(MappedInputManager::Button::Power)) {
    longPowerButtonHandled = false;
    return true;
  }

  return false;
}

bool TxtReaderActivity::consumeLongPowerButtonHold() {
  if (longPowerButtonHandled || !mappedInput.isPressed(MappedInputManager::Button::Power) ||
      mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return false;
  }

  longPowerButtonHandled = true;
  return true;
}

bool TxtReaderActivity::supportsQuickAction(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::SLEEP:
    case CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH:
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
      return true;
    default:
      return false;
  }
}

bool TxtReaderActivity::executeReaderShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      activityManager.goToFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      activityManager.goToCalibreWireless(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
      activityManager.goToJoinNetworkFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      activityManager.goToHotspotFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
      toggleDarkMode();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
      activityManager.goToFileBrowser(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER:
      toggleHomeButtonInReader();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
      return handleGlobalPowerButtonAction(action);
    default:
      return false;
  }
}

bool TxtReaderActivity::executePowerButtonAction() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return executeReaderShortcutAction(static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn));
  }

  const auto longPowerAction = static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  if (longPowerAction == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN || !consumeLongPowerButtonHold()) {
    return false;
  }

  if (executeReaderShortcutAction(longPowerAction)) {
    return true;
  }

  return false;
}

void TxtReaderActivity::toggleHomeButtonInReader() {
  if (!mappedInput.hasHomeKey()) return;
  SETTINGS.homeButtonInReaderEnabled = SETTINGS.homeButtonInReaderEnabled ? 0 : 1;
  if (!SETTINGS.saveGlobalDefaults()) {
    LOG_ERR("TXT", "Failed to save Home button reader setting");
  }
  mappedInput.clearDeferredHomeGesture();
  drawToast(renderer, SETTINGS.homeButtonInReaderEnabled ? tr(STR_HOME_BUTTON_ENABLED) : tr(STR_HOME_BUTTON_DISABLED));
  delay(1000);
  requestUpdate();
}

bool TxtReaderActivity::executeLongPressBackAction() {
  switch (static_cast<CrossPointSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressBackAction)) {
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_SLEEP:
      enterDeepSleep();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_REFRESH_SCREEN:
      prepareManualRefresh();
      requestUpdate();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FILE_TRANSFER:
      activityManager.goToFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CALIBRE_WIRELESS:
      activityManager.goToCalibreWireless(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_JOIN_NETWORK:
      activityManager.goToJoinNetworkFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CREATE_HOTSPOT:
      activityManager.goToHotspotFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_TOGGLE_DARK_MODE:
      toggleDarkMode();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FILE_BROWSER:
      activityManager.goToFileBrowser(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_LIBRARY:
      activityManager.goToLibrary();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CREATE_CLIPPING:
      return false;
    default:
      return false;
  }
}

bool TxtReaderActivity::handleShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  if (action == CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS) {
    QuickActions::showConfiguredPopup(
        quickActionsPopup, [this] { requestUpdate(); }, {},
        [](const auto quickAction) { return supportsQuickAction(quickAction); });
    return true;
  }
  return executeReaderShortcutAction(action);
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;
  markdownMode = txt && FsHelpers::hasMarkdownExtension(txt->getPath());
  markdownIndent = renderer.getTextAdvanceX(cachedFontId, "- ", EpdFontFamily::REGULAR);

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  const int topStatusBarReservedHeight = ReaderUtils::getTopClockStatusBarReservedHeight(renderer);
  if (topStatusBarReservedHeight > 0) {
    cachedOrientedMarginTop += std::max(static_cast<int>(cachedScreenMargin),
                                        topStatusBarReservedHeight + ReaderUtils::TOP_CLOCK_TEXT_PADDING);
  } else {
    cachedOrientedMarginTop += cachedScreenMargin;
  }
  cachedOrientedMarginBottom += std::max(
      cachedScreenMargin,
      static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight() + ReaderUtils::STATUS_BAR_TEXT_PADDING));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = getReaderLineHeight(renderer, cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex();
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  // Prime the SD card font's advance table before the wrap helper starts
  // measuring strings. This avoids on-demand SD glyph lookups for every width
  // check while preserving the shared parseAndWrapLines() implementation.
  if (renderer.isSdCardFont(cachedFontId)) {
    // Markdown : les titres se mesurent en gras et les citations en italique,
    // il faut donc primer tous les styles, pas seulement le regulier.
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer),
                                   markdownMode ? 0x0F : 0x01);
  }

  size_t pos = parseAndWrapLines(buffer, chunkSize, offset, fileSize, linesPerPage, renderer, cachedFontId,
                                 viewportWidth, outLines, markdownMode, markdownIndent);
  nextOffset = offset + pos;
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  free(buffer);

  return !outLines.empty();
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }
  if (quickActionsPopup.processRender(renderer, mappedInput)) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), ReaderUtils::readerForegroundBlack(),
                              EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);

  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  renderPage();

  if (!queueProgressSave()) {
    LOG_ERR("TRS", "Failed to save debounced reader progress");
  }
}

void TxtReaderActivity::renderPage() {
  const int lineHeight = getReaderLineHeight(renderer, cachedFontId);
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        // Style markdown en bande : premier octet dans [0x01,0x07].
        const char* text = line.c_str();
        auto style = EpdFontFamily::REGULAR;
        int indent = 0;
        if (markdownMode && static_cast<uint8_t>(line[0]) >= MD_HEADING &&
            static_cast<uint8_t>(line[0]) <= MD_RULE) {
          switch (static_cast<MdStyle>(line[0])) {
            case MD_HEADING:
              style = EpdFontFamily::BOLD;
              break;
            case MD_QUOTE:
              style = EpdFontFamily::ITALIC;
              indent = markdownIndent;
              break;
            case MD_LIST_CONT:
              indent = markdownIndent;
              break;
            case MD_RULE: {
              const int ruleWidth = contentWidth / 3;
              renderer.fillRect(cachedOrientedMarginLeft + (contentWidth - ruleWidth) / 2, y + lineHeight / 2,
                                ruleWidth, 2, ReaderUtils::readerForegroundBlack());
              y += lineHeight;
              continue;
            }
            case MD_BODY:
              break;
          }
          text++;
        }
        int x = cachedOrientedMarginLeft + indent;
        const bool lineIsRtl = BidiUtils::startsWithRtl(text, BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(cachedFontId, text, style);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, text, ReaderUtils::readerForegroundBlack(), style);
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // scan pass — text accumulated, no drawing
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();
  GUI.drawTopStatusBarClock(renderer, UITheme::getInstance().getMetrics().topPadding, nullptr, true, 0,
                            ReaderUtils::readerDarkModeEnabled());

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing && ReaderUtils::readerForegroundBlack()) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title, 0, 0, false, nullptr,
                    ReaderUtils::readerDarkModeEnabled());
}

bool TxtReaderActivity::saveProgress(const int page) {
  if (!txt) {
    return false;
  }
  HalFile f;
  if (!Storage.openFileForWrite("TRS", txt->getCachePath() + "/progress.bin", f)) {
    return false;
  }
  // 6-byte format: page(2 bytes LE) + file offset(4 bytes LE)
  // The offset lets drawCurrentPageToBuffer render without requiring index.bin.
  const size_t offset = (page >= 0 && page < static_cast<int>(pageOffsets.size())) ? pageOffsets[page] : 0;
  uint8_t data[6];
  data[0] = page & 0xFF;
  data[1] = (page >> 8) & 0xFF;
  data[2] = offset & 0xFF;
  data[3] = (offset >> 8) & 0xFF;
  data[4] = (offset >> 16) & 0xFF;
  data[5] = (offset >> 24) & 0xFF;
  const bool written = f.write(data, sizeof(data)) == sizeof(data);
  f.close();
  if (!written) {
    LOG_ERR("TRS", "Short write saving reader progress");
    return false;
  }
  progressSaveDebouncer.markPersisted(static_cast<uint32_t>(page));
  return true;
}

bool TxtReaderActivity::queueProgressSave() {
  if (!progressSaveDebouncer.observe(static_cast<uint32_t>(currentPage))) {
    return true;
  }
  return saveProgress(currentPage);
}

bool TxtReaderActivity::flushQueuedProgress() {
  return !progressSaveDebouncer.hasPending() ||
         saveProgress(static_cast<int>(progressSaveDebouncer.lastObservedPosition()));
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);
  if (numPages > MAX_CACHE_PAGES) {
    LOG_ERR("TRS", "Cache numPages %u exceeds cap %u, cache invalid", numPages, MAX_CACHE_PAGES);
    f.close();
    return false;
  }

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }
}

bool TxtReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  Txt txt(filePath, "/.crosspoint");
  if (!txt.load()) {
    LOG_DBG("SLP", "TXT: failed to load %s", filePath.c_str());
    return false;
  }

  // Apply the reader orientation so margins match what the reader would produce
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }

  // Compute layout values that match what initializeReader() produces
  const int fontId = SETTINGS.getReaderFontId();
  const uint8_t screenMargin = SETTINGS.screenMargin;
  const uint8_t paragraphAlignment = SETTINGS.paragraphAlignment;

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginLeft += screenMargin;
  marginRight += screenMargin;
  const int topStatusBarReservedHeight = ReaderUtils::getTopClockStatusBarReservedHeight(renderer);
  if (topStatusBarReservedHeight > 0) {
    marginTop +=
        std::max(static_cast<int>(screenMargin), topStatusBarReservedHeight + ReaderUtils::TOP_CLOCK_TEXT_PADDING);
  } else {
    marginTop += screenMargin;
  }
  marginBottom += std::max(screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight() +
                                                              ReaderUtils::STATUS_BAR_TEXT_PADDING));

  const int vw = renderer.getScreenWidth() - marginLeft - marginRight;
  const int vh = renderer.getScreenHeight() - marginTop - marginBottom;
  const int lineHeight = getReaderLineHeight(renderer, fontId);
  const int linesPerPage = std::max(1, vh / lineHeight);

  // Step 1: Try to read the saved page and its file offset from progress.bin.
  // The 6-byte format (written by saveProgress) stores: page(2) + offset(4).
  // This lets us skip index.bin entirely, so the overlay works even when the
  // page index cache is missing or stale (e.g. after a firmware update).
  int savedPage = 0;
  size_t savedOffset = 0;
  bool offsetKnown = false;
  {
    FsFile progFile;
    if (Storage.openFileForRead("SLP", txt.getCachePath() + "/progress.bin", progFile)) {
      uint8_t data[6] = {0};
      const int n = progFile.read(data, 6);
      progFile.close();
      if (n >= 2) {
        savedPage = (int)((uint32_t)data[0] | ((uint32_t)data[1] << 8));
      }
      if (n >= 6) {
        const uint32_t off =
            (uint32_t)data[2] | ((uint32_t)data[3] << 8) | ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24);
        if (off < txt.getFileSize()) {
          savedOffset = off;
          offsetKnown = true;
        }
      }
    }
  }

  // Step 2: If progress.bin didn't provide the offset, fall back to index.bin.
  if (!offsetKnown) {
    std::string cachePath = txt.getCachePath() + "/index.bin";
    FsFile cacheFile;
    if (Storage.openFileForRead("SLP", cachePath, cacheFile)) {
      uint32_t magic;
      serialization::readPod(cacheFile, magic);
      uint8_t version;
      serialization::readPod(cacheFile, version);
      uint32_t cachedFileSize;
      serialization::readPod(cacheFile, cachedFileSize);
      int32_t cachedVw, cachedLpp, cachedFontId, cachedMargin;
      serialization::readPod(cacheFile, cachedVw);
      serialization::readPod(cacheFile, cachedLpp);
      serialization::readPod(cacheFile, cachedFontId);
      serialization::readPod(cacheFile, cachedMargin);
      uint8_t cachedAlignment;
      serialization::readPod(cacheFile, cachedAlignment);
      uint32_t numPages;
      serialization::readPod(cacheFile, numPages);

      if (magic == CACHE_MAGIC && version == CACHE_VERSION && cachedFileSize == txt.getFileSize() && cachedVw == vw &&
          cachedLpp == linesPerPage && cachedFontId == fontId && cachedMargin == screenMargin &&
          cachedAlignment == paragraphAlignment && numPages > 0 && numPages <= MAX_CACHE_PAGES) {
        if (savedPage < 0 || savedPage >= static_cast<int>(numPages)) savedPage = 0;
        for (uint32_t i = 0; i < numPages; i++) {
          uint32_t off;
          serialization::readPod(cacheFile, off);
          if (static_cast<int>(i) == savedPage) {
            if (off < txt.getFileSize()) {
              savedOffset = off;
              offsetKnown = true;
            } else {
              LOG_DBG("SLP", "TXT: index.bin offset %u out of range (fileSize=%u), ignoring", off, txt.getFileSize());
            }
          }
        }
      } else {
        LOG_DBG("SLP", "TXT: index cache invalid or stale");
      }
      cacheFile.close();
    }

    // Step 3: No valid cache at all; render from the start of the file as a last resort.
    // This shows page 1 rather than a blank screen, which is always preferable.
    if (!offsetKnown) {
      LOG_DBG("SLP", "TXT: no valid cache, falling back to start of file");
      savedOffset = 0;
    }
  }

  // Load the page lines from file
  std::vector<std::string> pageLines;
  const size_t fileSize = txt.getFileSize();
  size_t offset = savedOffset;
  if (offset >= fileSize) {
    LOG_DBG("SLP", "TXT: page offset out of bounds");
    return false;
  }

  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) return false;

  if (!txt.readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  const bool markdown = FsHelpers::hasMarkdownExtension(filePath);
  const int mdIndent = markdown ? renderer.getTextAdvanceX(fontId, "- ", EpdFontFamily::REGULAR) : 0;
  parseAndWrapLines(buffer, chunkSize, offset, fileSize, linesPerPage, renderer, fontId, vw, pageLines, markdown,
                    mdIndent);
  free(buffer);

  if (pageLines.empty()) return false;

  // Render lines to frame buffer (no displayBuffer call)
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  int y = marginTop;
  for (const auto& line : pageLines) {
    if (!line.empty()) {
      const char* text = line.c_str();
      auto style = EpdFontFamily::REGULAR;
      int indent = 0;
      if (markdown && static_cast<uint8_t>(line[0]) >= MD_HEADING && static_cast<uint8_t>(line[0]) <= MD_RULE) {
        switch (static_cast<MdStyle>(line[0])) {
          case MD_HEADING:
            style = EpdFontFamily::BOLD;
            break;
          case MD_QUOTE:
            style = EpdFontFamily::ITALIC;
            indent = mdIndent;
            break;
          case MD_LIST_CONT:
            indent = mdIndent;
            break;
          case MD_RULE: {
            const int ruleWidth = vw / 3;
            renderer.fillRect(marginLeft + (vw - ruleWidth) / 2, y + lineHeight / 2, ruleWidth, 2,
                              ReaderUtils::readerForegroundBlack());
            y += lineHeight;
            continue;
          }
          case MD_BODY:
            break;
        }
        text++;
      }
      int x = marginLeft + indent;
      switch (paragraphAlignment) {
        case CrossPointSettings::CENTER_ALIGN:
          x = marginLeft + indent + (vw - renderer.getTextAdvanceX(fontId, text, style)) / 2;
          break;
        case CrossPointSettings::RIGHT_ALIGN:
          x = marginLeft + vw - renderer.getTextAdvanceX(fontId, text, style);
          break;
        default:
          break;
      }
      renderer.drawText(fontId, x, y, text, ReaderUtils::readerForegroundBlack(), style);
    }
    y += lineHeight;
  }
  return true;
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
