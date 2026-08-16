#include "RaindropSyncActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryState.h>
#include <WiFi.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

constexpr char ARTICLES_DIR[] = "/Articles";
constexpr char MANIFEST_TMP[] = "/.crosspoint/rdmanifest.tmp";
// Items per manifest page. Small keeps the transient JsonDocument a few KB —
// the walk never holds more than one page.
constexpr int MANIFEST_PAGE_LIMIT = 25;
// Guards against a cursor bug looping forever: 40 pages x 25 = 1000 articles,
// matching the server's own first-sync cap.
constexpr int MANIFEST_MAX_PAGES = 40;
// Same contiguous-block gate the KOReader sync client applies before TLS.
constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;

// Article filenames carry spaces and accents; the manifest sends them raw and
// the URL path needs them percent-encoded. Unreserved characters pass through.
std::string percentEncodePathSegment(const std::string& raw) {
  static constexpr char kHexDigits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(raw.size() * 3);
  for (const char c : raw) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '.' || c == '_' || c == '~';
    if (unreserved) {
      out.push_back(c);
    } else {
      const auto byte = static_cast<uint8_t>(c);
      out.push_back('%');
      out.push_back(kHexDigits[byte >> 4]);
      out.push_back(kHexDigits[byte & 0x0F]);
    }
  }
  return out;
}

// The manifest is remote input: even with TLS verified, a compromised server
// must not be able to steer writes outside /Articles. Flat names only.
bool isSafeArticleFileName(const char* name) {
  if (name[0] == '\0' || name[0] == '.') return false;
  const size_t len = strlen(name);
  if (len < 4 || strcmp(name + len - 3, ".md") != 0) return false;
  if (strchr(name, '/') != nullptr || strchr(name, '\\') != nullptr || strstr(name, "..") != nullptr) return false;
  return true;
}

std::string serverBaseUrl() {
  std::string base = SETTINGS.raindropServerUrl;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base;
}

}  // namespace

RaindropSyncActivity::RaindropSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("RaindropSync", renderer, mappedInput) {}

void RaindropSyncActivity::onEnter() {
  Activity::onEnter();
  // No WiFi.mode() here: forcing STA before WifiSelectionActivity races its
  // saved-network auto-connect (the OPDS flow, which works, lets the child
  // own the radio state entirely).
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void RaindropSyncActivity::onExit() {
  Activity::onExit();
  if (Storage.exists(MANIFEST_TMP)) {
    Storage.remove(MANIFEST_TMP);
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
  if (connected) {
    state_ = State::CONNECTED;
  } else {
    state_ = State::ERROR;
    errorMessage_ = tr(STR_WIFI_CONN_FAILED);
  }
  requestUpdate();
}

bool RaindropSyncActivity::pollCancel() {
  mappedInput.update();
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelRequested_ = true;
  }
  return cancelRequested_;
}

bool RaindropSyncActivity::downloadArticle(const std::string& fileName, const size_t expectedBytes) {
  const std::string localPath = std::string(ARTICLES_DIR) + "/" + fileName;

  // The manifest's byte size doubles as the change detector: the server
  // rewrites an article (summary backfill) by growing it, so a matching size
  // means the local copy is current. No sidecar state file needed.
  if (Storage.exists(localPath.c_str())) {
    HalFile existing;
    if (Storage.openFileForRead("RDROP", localPath.c_str(), existing)) {
      const size_t localSize = existing.size();
      if (localSize == expectedBytes) {
        keptCount_++;
        return true;
      }
    }
  }

  const std::string url = serverBaseUrl() + "/api/v1/articles/" + percentEncodePathSegment(fileName);
  HttpDownloader::DownloadOptions options;
  options.bearerToken = SETTINGS.raindropToken;
  options.shouldCancel = [this] { return pollCancel(); };
  const auto result = HttpDownloader::downloadToFile(url, localPath, nullptr, &cancelRequested_, "", "", options);
  if (result != HttpDownloader::OK) {
    LOG_ERR("RDROP", "Article download failed (%d): %s", static_cast<int>(result), fileName.c_str());
    failedCount_++;
    return false;
  }
  newCount_++;
  library::markShelfStaleIfBook(localPath.c_str());
  return true;
}

bool RaindropSyncActivity::syncOnePage(std::string& cursor, bool& morePages) {
  morePages = false;

  std::string manifestUrl = serverBaseUrl() + "/api/v1/manifest?limit=" + std::to_string(MANIFEST_PAGE_LIMIT);
  if (!cursor.empty()) {
    manifestUrl += "&cursor=" + percentEncodePathSegment(cursor);
  }

  HttpDownloader::DownloadOptions options;
  options.bearerToken = SETTINGS.raindropToken;
  options.shouldCancel = [this] { return pollCancel(); };
  const auto result =
      HttpDownloader::downloadToFile(manifestUrl, MANIFEST_TMP, nullptr, &cancelRequested_, "", "", options);
  if (result != HttpDownloader::OK) {
    errorMessage_ = tr(STR_RAINDROP_MANIFEST_FAILED);
    return false;
  }

  HalFile manifestFile;
  if (!Storage.openFileForRead("RDROP", MANIFEST_TMP, manifestFile)) {
    errorMessage_ = tr(STR_RAINDROP_MANIFEST_FAILED);
    return false;
  }

  // Own scope so the parsed page never coexists with an article download
  // buffer. One page is at most a few KB (MANIFEST_PAGE_LIMIT small items).
  {
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, manifestFile);
    manifestFile.close();
    Storage.remove(MANIFEST_TMP);
    if (err) {
      LOG_ERR("RDROP", "Manifest parse error: %s", err.c_str());
      errorMessage_ = tr(STR_RAINDROP_MANIFEST_FAILED);
      return false;
    }
    if ((doc["schema"] | 0) != 1) {
      LOG_ERR("RDROP", "Unsupported manifest schema %d", doc["schema"] | 0);
      errorMessage_ = tr(STR_RAINDROP_MANIFEST_FAILED);
      return false;
    }

    for (JsonVariantConst item : doc["items"].as<JsonArrayConst>()) {
      if (pollCancel()) {
        return false;
      }
      const char* status = item["status"] | "";
      const char* fileName = item["file"] | "";
      // Tombstones are ignored in this first version: the sync only ever adds.
      if (strcmp(status, "active") != 0 || fileName[0] == '\0') {
        continue;
      }
      if (!isSafeArticleFileName(fileName)) {
        LOG_ERR("RDROP", "Rejected unsafe article name from manifest");
        failedCount_++;
        continue;
      }
      downloadArticle(fileName, item["bytes"] | 0);

      const unsigned long now = millis();
      if (now - lastProgressDrawMs_ > 1500) {
        lastProgressDrawMs_ = now;
        requestUpdate(true);
      }
    }

    cursor = (doc["cursor"] | "");
    morePages = (doc["more"] | false);
  }
  return true;
}

void RaindropSyncActivity::runSync() {
  state_ = State::SYNCING;
  requestUpdate(true);

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

  std::string cursor;
  bool morePages = true;
  while (morePages && pageCount_ < MANIFEST_MAX_PAGES && !cancelRequested_) {
    if (!syncOnePage(cursor, morePages)) {
      if (!cancelRequested_ && errorMessage_.empty()) {
        errorMessage_ = tr(STR_RAINDROP_SYNC_FAILED);
      }
      // Keep what already landed: a partial sync is still articles on the
      // shelf. Only a manifest failure with nothing fetched reads as an error.
      if (newCount_ == 0 && keptCount_ == 0 && !cancelRequested_) {
        state_ = State::ERROR;
        requestUpdate();
        return;
      }
      break;
    }
    pageCount_++;
  }

  state_ = State::COMPLETE;
  requestUpdate();
}

void RaindropSyncActivity::loop() {
  if (state_ == State::CONNECTED) {
    runSync();
    return;
  }
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
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  GUI.drawHeader(renderer, header, tr(STR_RAINDROP_SYNC));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;

  char counts[96];
  snprintf(counts, sizeof(counts), "%d %s · %d %s · %d %s", newCount_, tr(STR_RAINDROP_COUNT_NEW), keptCount_,
           tr(STR_RAINDROP_COUNT_KEPT), failedCount_, tr(STR_RAINDROP_COUNT_FAILED));

  switch (state_) {
    case State::WIFI_SELECTION:
      break;
    case State::CONNECTED:
    case State::SYNCING: {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_RAINDROP_SYNCING));
      renderer.drawCenteredText(SMALL_FONT_ID, centerY + lineHeight / 2, counts);
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::COMPLETE: {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_RAINDROP_SYNC_DONE));
      renderer.drawCenteredText(SMALL_FONT_ID, centerY + lineHeight / 2, counts);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case State::ERROR: {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, errorMessage_.c_str());
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }
}
