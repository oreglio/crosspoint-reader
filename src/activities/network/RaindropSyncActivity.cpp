#include "RaindropSyncActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <sys/time.h>

#include <cstring>
#include <ctime>
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
constexpr char DONE_DIR[] = "/Articles/.done";
constexpr char DONE_QUEUE[] = "/.crosspoint/raindrop-done.txt";
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

// wolfSSL valide les dates des certificats contre l'horloge SYSTEME (time()),
// que personne n'initialise dans le boot reseau minimal : elle demarre a
// l'epoch 1970 et le chargement de l'ancre echoue en ASN_BEFORE_DATE_E (-150,
// vu sur X3). Le RTC, lui, est a l'heure (UTC) : on le pousse vers time().
void syncSystemClockFromRtc() {
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
  if (!halClock.isAvailable() || !halClock.getDateTime(year, month, day, hour, minute)) {
    LOG_ERR("RDROP", "RTC unavailable; TLS certificate date checks may fail");
    return;
  }
  struct tm utc = {};
  utc.tm_year = year - 1900;
  utc.tm_mon = month - 1;
  utc.tm_mday = day;
  utc.tm_hour = hour;
  utc.tm_min = minute;
  const time_t epoch = mktime(&utc);  // TZ jamais definie sur l'appareil : mktime == UTC
  if (epoch <= 0) {
    return;
  }
  const timeval tv = {epoch, 0};
  settimeofday(&tv, nullptr);
  LOG_INF("RDROP", "System clock set from RTC: %04u-%02u-%02u %02u:%02u UTC", year, month, day, hour, minute);
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
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
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
  // Le repull Raindrop (refresh=1) est porte par la requete bundle/info qui
  // precede : ici on ne fait plus que recuperer le zip deja annonce.
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
  unsigned lastDrawnPercent = 0;
  const auto result = HttpDownloader::downloadToFile(
      url, BUNDLE_TMP,
      [this, &lastDrawnPercent](const size_t downloaded, const size_t total) {
        downloadedBytes_ = downloaded;
        totalBytes_ = total;
        mappedInput.update();
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          cancelRequested_ = true;
        }
        const unsigned percent = total > 0 ? static_cast<unsigned>(downloaded * 100ULL / total) : 0;
        if (percent >= lastDrawnPercent + 5 || (total > 0 && downloaded == total)) {
          lastDrawnPercent = percent;
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

// Lecteur de zip minimal, STORED uniquement, memoire constante : le lecteur
// EPUB (ZipFile) cache le repertoire central entier dans une unordered_map de
// std::string — ~150 octets par entree, soit ~60 Ko pour un bundle de 400
// articles, un abort() OOM assure sur C3 (vu sur X3). Ici : un seul buffer de
// 2 Ko reutilise, une passe sur le repertoire central, seek + copie par entree.
bool RaindropSyncActivity::unpackBundle() {
  HalFile zip;
  if (!Storage.openFileForRead("RDROP", BUNDLE_TMP, zip)) {
    errorMessage_ = tr(STR_RAINDROP_MANIFEST_FAILED);
    return false;
  }
  const size_t zipSize = zip.size();
  // EOCD fixe en fin de fichier (le serveur n'ecrit pas de commentaire).
  uint8_t eocd[22];
  if (zipSize < sizeof(eocd) || !zip.seekSet(zipSize - sizeof(eocd)) ||
      zip.read(eocd, sizeof(eocd)) != static_cast<int>(sizeof(eocd)) || eocd[0] != 0x50 || eocd[1] != 0x4b ||
      eocd[2] != 0x05 || eocd[3] != 0x06) {
    LOG_ERR("RDROP", "Bundle: EOCD introuvable");
    errorMessage_ = tr(STR_RAINDROP_MANIFEST_FAILED);
    return false;
  }
  auto u16 = [](const uint8_t* d) { return static_cast<uint16_t>(d[0] | (d[1] << 8)); };
  auto u32 = [](const uint8_t* d) {
    return static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8) | (static_cast<uint32_t>(d[2]) << 16) |
           (static_cast<uint32_t>(d[3]) << 24);
  };
  const uint16_t entryCount = u16(eocd + 10);
  uint32_t cdOffset = u32(eocd + 16);

  // 2 Ko partages entre lecture d'entetes et copie de donnees ; trop gros pour
  // la pile de la tache (8 Ko), liberes en sortie.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(EXTRACT_CHUNK);
  if (!buffer) {
    LOG_ERR("RDROP", "OOM: extract buffer (%u)", static_cast<unsigned>(EXTRACT_CHUNK));
    errorMessage_ = tr(STR_RAINDROP_SYNC_FAILED);
    return false;
  }

  std::string pendingCursor;
  unsigned long lastDrawMs = 0;
  for (uint16_t i = 0; i < entryCount; i++) {
    if (pollCancel()) {
      return false;
    }
    uint8_t cd[46];
    if (!zip.seekSet(cdOffset) || zip.read(cd, sizeof(cd)) != static_cast<int>(sizeof(cd)) || u32(cd) != 0x02014b50) {
      LOG_ERR("RDROP", "Bundle: entree centrale %u illisible", i);
      failedCount_++;
      break;
    }
    const uint16_t method = u16(cd + 10);
    const uint32_t dataSize = u32(cd + 20);
    const uint16_t nameLen = u16(cd + 28);
    const uint16_t extraLen = u16(cd + 30);
    const uint16_t commentLen = u16(cd + 32);
    const uint32_t localOffset = u32(cd + 42);
    char name[128] = {};
    const uint16_t readLen = nameLen < sizeof(name) - 1 ? nameLen : sizeof(name) - 1;
    if (zip.read(reinterpret_cast<uint8_t*>(name), readLen) != readLen) {
      failedCount_++;
      break;
    }
    cdOffset += 46 + nameLen + extraLen + commentLen;

    // En-tete local : ses champs name/extra peuvent differer du central.
    uint8_t local[30];
    if (!zip.seekSet(localOffset) || zip.read(local, sizeof(local)) != static_cast<int>(sizeof(local)) ||
        u32(local) != 0x04034b50 || method != 0) {
      LOG_ERR("RDROP", "Bundle: en-tete local invalide pour %s", name);
      failedCount_++;
      continue;
    }
    const uint32_t dataStart = localOffset + 30 + u16(local + 26) + u16(local + 28);

    const bool isManifest = strcmp(name, MANIFEST_ENTRY) == 0;
    // L'index TSV (snapshot titres/tags/dates du serveur) alimente le viewer
    // d'articles : extrait vers /Articles/.index en l'ecrasant.
    const bool isIndex = strcmp(name, "index.tsv") == 0;
    if (!isManifest && !isIndex && !isSafeArticleFileName(name)) {
      LOG_ERR("RDROP", "Rejected unsafe bundle entry name");
      failedCount_++;
      continue;
    }
    if (!isManifest && !isIndex) {
      // Un article marque lu sur la liseuse ne ressuscite pas quand le serveur
      // le reecrit (backfill de resume) : il attend son archivage Raindrop.
      const std::string donePath = std::string(DONE_DIR) + "/" + name;
      if (Storage.exists(donePath.c_str())) {
        continue;
      }
    }

    if (isManifest) {
      // Le curseur vit dans les premiers octets du manifest.json.
      char head[256] = {};
      const size_t take = dataSize < sizeof(head) - 1 ? dataSize : sizeof(head) - 1;
      if (zip.seekSet(dataStart) && zip.read(reinterpret_cast<uint8_t*>(head), take) == static_cast<int>(take)) {
        const char* at = strstr(head, "\"cursor\":\"");
        if (at != nullptr) {
          at += 10;
          const char* end = strchr(at, '"');
          if (end != nullptr) {
            pendingCursor.assign(at, end - at);
          }
        }
      }
      continue;
    }

    if (!isIndex) {
      currentArticle_ = name;
      const unsigned long now = millis();
      if (now - lastDrawMs > 1500) {
        lastDrawMs = now;
        requestUpdate(true);
      }
    }

    const std::string destPath = isIndex ? "/Articles/.index" : std::string(ARTICLES_DIR) + "/" + name;
    HalFile out;
    if (!Storage.openFileForWrite("RDROP", destPath.c_str(), out)) {
      LOG_ERR("RDROP", "Could not open %s for write", destPath.c_str());
      failedCount_++;
      continue;
    }
    bool ok = zip.seekSet(dataStart);
    uint32_t remaining = dataSize;
    while (ok && remaining > 0) {
      const size_t take = remaining < EXTRACT_CHUNK ? remaining : EXTRACT_CHUNK;
      if (zip.read(buffer.get(), take) != static_cast<int>(take) || out.write(buffer.get(), take) != take) {
        ok = false;
        break;
      }
      remaining -= take;
    }
    out.close();
    if (!ok) {
      LOG_ERR("RDROP", "Extraction failed: %s", name);
      Storage.remove(destPath.c_str());
      if (!isIndex) failedCount_++;
      continue;
    }
    if (!isIndex) newCount_++;
  }

  // Curseur avance des que la passe s'est terminee : re-telecharger 5,5 Mo
  // pour un article rate serait absurde — les echecs sont logges et un article
  // manque revient de lui-meme a sa prochaine modification cote serveur.
  if (!cancelRequested_ && !pendingCursor.empty()) {
    storeCursor(pendingCursor);
  }
  return true;
}

// Envoie au serveur les articles marques lus depuis la derniere sync : il les
// archive cote Raindrop a sa prochaine passe. La file ne se vide que sur 200,
// donc un echec reseau se rejoue a la sync suivante.
void RaindropSyncActivity::relayDoneQueue() {
  std::string queue;
  {
    HalFile in;
    if (!Storage.openFileForRead("RDROP", DONE_QUEUE, in)) {
      return;
    }
    const size_t size = in.size();
    if (size == 0 || size > 16384) {
      return;
    }
    queue.resize(size);
    if (in.read(reinterpret_cast<uint8_t*>(queue.data()), size) != static_cast<int>(size)) {
      return;
    }
  }

  std::string body = "{\"files\":[";
  bool first = true;
  int names = 0;
  size_t lineStart = 0;
  while (lineStart < queue.size() && names < 200) {
    size_t lineEnd = queue.find('\n', lineStart);
    if (lineEnd == std::string::npos) lineEnd = queue.size();
    const std::string name = queue.substr(lineStart, lineEnd - lineStart);
    lineStart = lineEnd + 1;
    // Les noms sont deja sans guillemets ni antislash (sanitisation serveur) ;
    // un nom qui en porterait casserait le JSON : ignore.
    if (name.empty() || name.find('"') != std::string::npos || name.find('\\') != std::string::npos) {
      continue;
    }
    if (!first) body += ',';
    body += '"';
    body += name;
    body += '"';
    first = false;
    names++;
  }
  body += "]}";
  if (names == 0) {
    Storage.remove(DONE_QUEUE);
    return;
  }

  freeink::SecureHttpClient http;
  http.setCACert(ISRG_ROOT_X1_PEM);
  const std::string url = serverBaseUrl() + "/api/v1/archived-files";
  if (!http.begin(url)) {
    LOG_ERR("RDROP", "Bad archive URL");
    return;
  }
  http.addHeader("Authorization", std::string("Bearer ") + SETTINGS.raindropToken);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  http.end();
  if (httpCode >= 200 && httpCode < 300) {
    LOG_INF("RDROP", "Archived %d read article(s) server-side", names);
    Storage.remove(DONE_QUEUE);
  } else {
    LOG_ERR("RDROP", "Archive relay failed: %d", httpCode);
  }
}

void RaindropSyncActivity::runSync() {
  syncSystemClockFromRtc();
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

  // La file des lus part d'abord : la passe refresh du serveur (bundle/info)
  // archive alors ces articles dans la meme foulee.
  relayDoneQueue();

  // Demande legere avant d'engager le telechargement : le serveur repull
  // Raindrop puis annonce combien d'articles et d'octets attendent. Zero
  // nouveaute = pas de telechargement du tout. Si l'annonce echoue, on
  // retombe sur le comportement historique : telecharger directement.
  if (fetchBundleInfo()) {
    if (pendingCount_ == 0) {
      state_ = State::COMPLETE;
      requestUpdate();
      return;
    }
    state_ = State::CONFIRM;
    requestUpdate();
    return;
  }
  runDownloadPhase();
}

// Le corps de bundle/info tient en ~40 octets : {"count":N,"bytes":M}.
bool RaindropSyncActivity::fetchBundleInfo() {
  freeink::SecureHttpClient http;
  http.setCACert(ISRG_ROOT_X1_PEM);
  std::string url = serverBaseUrl() + "/api/v1/bundle/info?refresh=1";
  const std::string cursor = readStoredCursor();
  if (!cursor.empty()) {
    url += "&cursor=" + cursor;
  }
  if (!http.begin(url)) {
    LOG_ERR("RDROP", "Bad bundle info URL");
    return false;
  }
  http.addHeader("Authorization", std::string("Bearer ") + SETTINGS.raindropToken);
  const int code = http.sendRequest("GET", std::string());
  if (code < 200 || code >= 300) {
    LOG_ERR("RDROP", "Bundle info failed: %d", code);
    http.end();
    return false;
  }
  const std::string& body = http.getString();
  const char* countAt = strstr(body.c_str(), "\"count\":");
  const char* bytesAt = strstr(body.c_str(), "\"bytes\":");
  http.end();
  if (countAt == nullptr || bytesAt == nullptr) {
    LOG_ERR("RDROP", "Bundle info: unexpected body");
    return false;
  }
  pendingCount_ = atoi(countAt + 8);
  pendingBytes_ = static_cast<uint32_t>(strtoul(bytesAt + 8, nullptr, 10));
  LOG_INF("RDROP", "Server announces %d article(s), ~%u bytes", pendingCount_, pendingBytes_);
  return true;
}

void RaindropSyncActivity::runDownloadPhase() {
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

  if (!cancelRequested_) {
    relayDoneQueue();
  }

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
  // Pas de mappedInput.update() ici : la boucle principale fait deja
  // gpio.update() a chaque passe, et un second update sous les 5 ms de
  // debounce CONSOMME le front de relachement avant wasReleased() — les
  // clics semblaient ignores un coup sur deux (vu sur X3). L'auto-update
  // ne se justifie que pendant le travail bloquant (pollCancel, progression),
  // ou la boucle principale est a l'arret.
  if (state_ == State::CONFIRM) {
    if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Meme rituel que le callback wifi : notre ecran d'abord, puis le
      // travail bloquant — sinon la dalle resterait figee sur la question.
      state_ = State::SYNCING;
      if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
        requestUpdate(true);
      }
      runDownloadPhase();
    }
    return;
  }
  if (state_ == State::COMPLETE || state_ == State::ERROR) {
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
    case State::CONFIRM: {
      char announce[80];
      if (pendingBytes_ >= 1024 * 1024) {
        const uint32_t tenthsMb = pendingBytes_ / (1024 * 102);  // dixiemes de Mo
        snprintf(announce, sizeof(announce), "%d %s  (~%u,%u Mo)", pendingCount_, tr(STR_RAINDROP_COUNT_NEW),
                 static_cast<unsigned>(tenthsMb / 10), static_cast<unsigned>(tenthsMb % 10));
      } else {
        snprintf(announce, sizeof(announce), "%d %s  (~%u Ko)", pendingCount_, tr(STR_RAINDROP_COUNT_NEW),
                 static_cast<unsigned>(pendingBytes_ / 1024));
      }
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - 2 * lineHeight, announce);
      renderer.drawCenteredText(SMALL_FONT_ID, centerY, tr(STR_RAINDROP_CONFIRM_DL));
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DOWNLOAD), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
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
