#include "RaindropSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryState.h>
#include <WiFi.h>
#include <ZipFile.h>

#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/IsrgRootX1.h"

namespace {

constexpr char ARTICLES_DIR[] = "/Articles";
constexpr char BUNDLE_TMP[] = "/.crosspoint/rdbundle.tmp";
constexpr char CURSOR_FILE[] = "/.crosspoint/raindrop-cursor.txt";
constexpr char MANIFEST_ENTRY[] = "manifest.json";
constexpr size_t EXTRACT_CHUNK = 2048;
// Same contiguous-block gate the KOReader sync client applies before TLS.
constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;

// The bundle is remote input: even with TLS verified, a compromised server
// must not be able to steer writes outside /Articles. Flat names only.
bool isSafeArticleFileName(const char* name) {
  if (name[0] == '\0' || name[0] == '.') return false;
  const size_t len = strlen(name);
  if (len < 4 || strcmp(name + len - 3, ".md") != 0) return false;
  if (strchr(name, '/') != nullptr || strchr(name, '\\') != nullptr || strstr(name, "..") != nullptr) return false;
  return true;
}

// The cursor is base64url from the server, opaque to us: allow only its
// alphabet so a corrupted file cannot inject query-string syntax.
bool isSafeCursor(const std::string& cursor) {
  if (cursor.empty() || cursor.size() > 32) return false;
  for (const char c : cursor) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

std::string serverBaseUrl() {
  std::string base = SETTINGS.raindropServerUrl;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base;
}

// Captures the head of manifest.json — the server writes `cursor` in the
// first bytes, so this never scales with the article count. Short-writes once
// full, which lets readFileToStream stop early.
class ManifestHeadSink final : public Print {
 public:
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buffer, size_t size) override {
    const size_t room = sizeof(head_) - 1 - used_;
    const size_t take = size < room ? size : room;
    memcpy(head_ + used_, buffer, take);
    used_ += take;
    head_[used_] = '\0';
    return take;
  }
  std::string cursor() const {
    const char* at = strstr(head_, "\"cursor\":\"");
    if (at == nullptr) return {};
    at += 10;
    const char* end = strchr(at, '"');
    if (end == nullptr) return {};
    return std::string(at, end - at);
  }

 private:
  char head_[256] = {};
  size_t used_ = 0;
};

}  // namespace

RaindropSyncActivity::RaindropSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("RaindropSync", renderer, mappedInput) {}

void RaindropSyncActivity::onEnter() {
  Activity::onEnter();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void RaindropSyncActivity::onExit() {
  Activity::onExit();
  if (Storage.exists(BUNDLE_TMP)) {
    Storage.remove(BUNDLE_TMP);
  }
#ifndef SIMULATOR
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  // Launched from a minimal network boot: rebooting restores the full app and
  // clears the heap fragmentation the wifi session left behind.
  silentRestart();
#endif
}

void RaindropSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    state_ = State::ERROR;
    errorMessage_ = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
    return;
  }
  // The whole sync runs synchronously inside this callback, exactly like the
  // OPDS browser's feed fetch: the render ritual below puts our own screen up
  // before the blocking work. Deferring the work to loop() left the wifi
  // child's last frame on screen with dead buttons (seen on device).
  state_ = State::SYNCING;
  const auto renderResult = requestUpdateAndWait();
  LOG_INF("RDROP", "pre-sync render result=%d", static_cast<int>(renderResult));
  if (renderResult != RequestUpdateResult::Rendered) {
    requestUpdate(true);
  }
  runSync();
}

bool RaindropSyncActivity::pollCancel() {
  mappedInput.update();
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelRequested_ = true;
  }
  return cancelRequested_;
}

std::string RaindropSyncActivity::readStoredCursor() {
  HalFile file;
  if (!Storage.openFileForRead("RDROP", CURSOR_FILE, file)) {
    return {};
  }
  char buffer[40] = {};
  const int got = file.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer) - 1);
  if (got <= 0) {
    return {};
  }
  buffer[got] = '\0';
  std::string cursor(buffer);
  while (!cursor.empty() && (cursor.back() == '\n' || cursor.back() == '\r' || cursor.back() == ' ')) {
    cursor.pop_back();
  }
  return isSafeCursor(cursor) ? cursor : std::string();
}

void RaindropSyncActivity::storeCursor(const std::string& cursor) {
  if (!isSafeCursor(cursor)) {
    return;
  }
  HalFile file;
  if (!Storage.openFileForWrite("RDROP", CURSOR_FILE, file)) {
    LOG_ERR("RDROP", "Could not persist sync cursor");
    return;
  }
  file.write(reinterpret_cast<const uint8_t*>(cursor.c_str()), cursor.size());
}

bool RaindropSyncActivity::downloadBundle() {
  LOG_INF("RDROP", "bundle download start free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  std::string url = serverBaseUrl() + "/api/v1/bundle";
  const std::string cursor = readStoredCursor();
  if (!cursor.empty()) {
    url += "?cursor=" + cursor;
  }

  HttpDownloader::DownloadOptions options;
  // wolfSSL comme les téléchargements OPDS : esp_http rampe à ~500 o/s sur les
  // gros corps HTTPS en C3 (bug documenté par le manifeste des polices). La
  // racine ISRG X1 garde la vérification du certificat que le bundle assurait.
  options.transport = HttpDownloader::Transport::WOLFSSL;
  options.caCertPem = ISRG_ROOT_X1_PEM;
  options.bearerToken = SETTINGS.raindropToken;
  options.shouldCancel = [this] { return pollCancel(); };
  unsigned long lastDrawMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      url, BUNDLE_TMP,
      [this, &lastDrawMs](const size_t downloaded, const size_t total) {
        downloadedBytes_ = downloaded;
        totalBytes_ = total;
        mappedInput.update();
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          cancelRequested_ = true;
        }
        const unsigned long now = millis();
        if (now - lastDrawMs > 1000) {
          lastDrawMs = now;
          LOG_INF("RDROP", "bundle progress %u/%u", static_cast<unsigned>(downloaded), static_cast<unsigned>(total));
          requestUpdate(true);
        }
      },
      &cancelRequested_, "", "", options);
  if (result != HttpDownloader::OK) {
    LOG_ERR("RDROP", "Bundle download failed (%d)", static_cast<int>(result));
    errorMessage_ = tr(STR_RAINDROP_MANIFEST_FAILED);
    return false;
  }
  return true;
}

bool RaindropSyncActivity::unpackBundle() {
  ZipFile zip{std::string(BUNDLE_TMP)};
  if (!zip.open() || !zip.loadAllFileStatSlims()) {
    LOG_ERR("RDROP", "Bundle is not a readable zip");
    errorMessage_ = tr(STR_RAINDROP_MANIFEST_FAILED);
    return false;
  }

  // Names are copied out: extraction reuses the zip handle per entry, and the
  // enumeration's string_views borrow cache internals. ~100 bytes per article,
  // freed when the sync ends.
  std::vector<std::string> names;
  names.reserve(64);
  zip.enumerateFilePaths([&names](const std::string_view path) {
    if (path != MANIFEST_ENTRY) {
      names.emplace_back(path);
    }
  });

  unsigned long lastDrawMs = 0;
  for (const auto& name : names) {
    if (pollCancel()) {
      zip.close();
      return false;
    }
    if (!isSafeArticleFileName(name.c_str())) {
      LOG_ERR("RDROP", "Rejected unsafe bundle entry name");
      failedCount_++;
      continue;
    }
    currentArticle_ = name;
    const unsigned long now = millis();
    if (now - lastDrawMs > 1000) {
      lastDrawMs = now;
      requestUpdate(true);
    }

    const std::string destPath = std::string(ARTICLES_DIR) + "/" + name;
    HalFile out;
    if (!Storage.openFileForWrite("RDROP", destPath.c_str(), out)) {
      LOG_ERR("RDROP", "Could not open %s for write", destPath.c_str());
      failedCount_++;
      continue;
    }
    const bool ok = zip.readFileToStream(name.c_str(), out, EXTRACT_CHUNK);
    out.close();
    if (!ok) {
      LOG_ERR("RDROP", "Extraction failed: %s", name.c_str());
      Storage.remove(destPath.c_str());
      failedCount_++;
      continue;
    }
    newCount_++;
    library::markShelfStaleIfBook(destPath.c_str());
  }

  // The new cursor lives in the first bytes of manifest.json; persist it only
  // when every entry landed, so a partial run retries from the old cursor.
  if (failedCount_ == 0 && !cancelRequested_) {
    ManifestHeadSink head;
    zip.readFileToStream(MANIFEST_ENTRY, head, 128, /*allowEarlyStop=*/true);
    const std::string cursor = head.cursor();
    if (!cursor.empty()) {
      storeCursor(cursor);
    }
  }
  zip.close();
  return true;
}

void RaindropSyncActivity::runSync() {
  if (serverBaseUrl().empty() || SETTINGS.raindropToken[0] == '\0') {
    state_ = State::ERROR;
    errorMessage_ = tr(STR_RAINDROP_NOT_CONFIGURED);
    requestUpdate();
    return;
  }

  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_TLS || maxAllocHeap < MIN_MAX_ALLOC_HEAP_FOR_TLS) {
    LOG_ERR("RDROP", "Insufficient heap for TLS: %u free, %u max alloc", freeHeap, maxAllocHeap);
    state_ = State::ERROR;
    errorMessage_ = tr(STR_RAINDROP_SYNC_FAILED);
    requestUpdate();
    return;
  }

  if (!Storage.exists(ARTICLES_DIR) && !Storage.mkdir(ARTICLES_DIR)) {
    LOG_ERR("RDROP", "Could not create %s", ARTICLES_DIR);
    state_ = State::ERROR;
    errorMessage_ = tr(STR_RAINDROP_SYNC_FAILED);
    requestUpdate();
    return;
  }

  const bool downloaded = downloadBundle();
  bool unpacked = false;
  if (downloaded && !cancelRequested_) {
    unpacking_ = true;
    requestUpdate(true);
    unpacked = unpackBundle();
  }
  if (Storage.exists(BUNDLE_TMP)) {
    Storage.remove(BUNDLE_TMP);
  }
  currentArticle_.clear();

  if (cancelRequested_ || (downloaded && unpacked) || newCount_ > 0) {
    state_ = State::COMPLETE;
  } else {
    state_ = State::ERROR;
    if (errorMessage_.empty()) {
      errorMessage_ = tr(STR_RAINDROP_SYNC_FAILED);
    }
  }
  requestUpdate();
}

void RaindropSyncActivity::loop() {
  if (state_ == State::COMPLETE || state_ == State::ERROR) {
    mappedInput.update();
    if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}

void RaindropSyncActivity::render(RenderLock&&) {
  LOG_INF("RDROP", "render state=%d unpack=%d dl=%u/%u new=%d fail=%d", static_cast<int>(state_), unpacking_ ? 1 : 0,
          static_cast<unsigned>(downloadedBytes_), static_cast<unsigned>(totalBytes_), newCount_, failedCount_);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  GUI.drawHeader(renderer, header, tr(STR_RAINDROP_SYNC));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;
  const int contentWidth = pageWidth - 2 * metrics.contentSidePadding;

  char counts[64];
  snprintf(counts, sizeof(counts), "%d %s   %d %s", newCount_, tr(STR_RAINDROP_COUNT_NEW), failedCount_,
           tr(STR_RAINDROP_COUNT_FAILED));

  switch (state_) {
    case State::WIFI_SELECTION:
      break;
    case State::SYNCING: {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - 2 * lineHeight, tr(STR_RAINDROP_SYNCING));
      if (!unpacking_) {
        char progress[48];
        if (totalBytes_ > 0) {
          snprintf(progress, sizeof(progress), "%u%%  (%u / %u Ko)",
                   static_cast<unsigned>(downloadedBytes_ * 100 / totalBytes_),
                   static_cast<unsigned>(downloadedBytes_ / 1024), static_cast<unsigned>(totalBytes_ / 1024));
        } else {
          snprintf(progress, sizeof(progress), "%u Ko", static_cast<unsigned>(downloadedBytes_ / 1024));
        }
        renderer.drawCenteredText(UI_10_FONT_ID, centerY, progress);
      } else {
        if (!currentArticle_.empty()) {
          const auto shown = renderer.truncatedText(SMALL_FONT_ID, currentArticle_.c_str(), contentWidth);
          renderer.drawCenteredText(SMALL_FONT_ID, centerY - smallLineHeight / 2, shown.c_str());
        }
        renderer.drawCenteredText(UI_10_FONT_ID, centerY + lineHeight, counts);
      }
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::COMPLETE: {
      const char* title = cancelRequested_   ? tr(STR_CANCEL)
                          : failedCount_ > 0 ? tr(STR_RAINDROP_SYNC_DONE_PARTIAL)
                                             : tr(STR_RAINDROP_SYNC_DONE);
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - 2 * lineHeight, title);
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, counts);
      if (newCount_ > 0) {
        renderer.drawCenteredText(SMALL_FONT_ID, centerY + 2 * lineHeight, tr(STR_RAINDROP_SEE_LIBRARY));
      }
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::ERROR: {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - 2 * lineHeight, tr(STR_RAINDROP_SYNC_FAILED));
      if (!errorMessage_.empty()) {
        const auto shown = renderer.truncatedText(SMALL_FONT_ID, errorMessage_.c_str(), contentWidth);
        renderer.drawCenteredText(SMALL_FONT_ID, centerY - smallLineHeight / 2, shown.c_str());
      }
      if (newCount_ + failedCount_ > 0) {
        renderer.drawCenteredText(SMALL_FONT_ID, centerY + lineHeight, counts);
      }
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }

  // Sans ce flush, tout ce qui précède reste dans le framebuffer : la dalle
  // continuait d'afficher la dernière image de l'écran WiFi (vu sur X3).
  renderer.displayBuffer(screenTransitionRefresh_.modeFor(static_cast<uint8_t>(state_)));
}
