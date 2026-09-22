#include "TaskSyncActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <sys/time.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "SilentRestart.h"
#include "TaskStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/IsrgRootX1.h"
#include "tasks/TaskOpQueue.h"
#include "tasks/TaskSecret.h"
#include "tasks/TaskSyncOutcome.h"

namespace {

constexpr char TAG[] = "TSYNC";
// Meme garde de bloc contigu que Raindrop et KOReader avant TLS : echouer ici
// avec un message lisible vaut mieux qu'un abort() au milieu de la poignee.
constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;
// Borne des tours d'une meme sync : un tour par page `more` du serveur ou par
// tranche de TASK_MAX_OPS_PER_SYNC ops. 20 tours = 1000 ops en attente, bien
// au-dela d'un usage hors ligne plausible, sans boucle infinie possible.
constexpr int MAX_ROUNDS = 20;
// Borne d'une ligne d'op serialisee (voir TASK_OP_LINE_BUF dans TaskStore.cpp),
// pour dimensionner le corps au plus juste plutot qu'a 12 Ko d'office.
constexpr size_t OP_LINE_BOUND = 512;
constexpr size_t BODY_ENVELOPE = 128;
#ifdef SIMULATOR
// Le client HTTP du simulateur n'a pas d'API de flux : le corps arrive entier,
// puis part au lecteur par tranches de 17 octets. Plus petit et plus biscornu
// que tout ce que le reseau livre : chaque frontiere de cadrage y passe (ligne
// coupee, note a cheval, \n final de note arrivant seul).
constexpr size_t SIMULATOR_FEED_CHUNK = 17;
#endif

// wolfSSL valide les dates des certificats contre l'horloge SYSTEME (time()),
// que personne n'initialise dans le boot reseau minimal : elle demarre a
// l'epoch 1970 et le chargement de l'ancre echoue en ASN_BEFORE_DATE_E (-150,
// vu sur X3). Le RTC, lui, est a l'heure (UTC) : on le pousse vers time().
// Copie de RaindropSyncActivity.cpp.
void syncSystemClockFromRtc() {
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
  if (!halClock.isAvailable() || !halClock.getDateTime(year, month, day, hour, minute)) {
    LOG_ERR(TAG, "RTC unavailable; TLS certificate date checks may fail");
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
  LOG_INF(TAG, "System clock set from RTC: %04u-%02u-%02u %02u:%02u UTC", year, month, day, hour, minute);
}

std::string serverBaseUrl() {
  std::string base = SETTINGS.taskServerUrl;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base;
}

const char* rejectionReasonText(const char* reason) {
  if (strcmp(reason, "full") == 0) return tr(STR_TASK_REJECT_FULL);
  if (strcmp(reason, "unknown") == 0) return tr(STR_TASK_REJECT_UNKNOWN);
  if (strcmp(reason, "badtitle") == 0) return tr(STR_TASK_REJECT_BAD_TITLE);
  return tr(STR_TASK_REJECT_OTHER);
}

}  // namespace

TaskSyncActivity::TaskSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("TaskSync", renderer, mappedInput) {}

void TaskSyncActivity::onEnter() {
  Activity::onEnter();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
  requestUpdate();
}

void TaskSyncActivity::onExit() {
  Activity::onExit();
  closeNoteFile();
#ifndef SIMULATOR
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  // Lance depuis un demarrage reseau minimal : redemarrer restaure l'appli
  // complete et efface la fragmentation du tas laissee par la session Wi-Fi.
  silentRestart();
#endif
}

void TaskSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    fail(Failure::WifiFailed);
    return;
  }
  // Meme rituel que Raindrop : notre ecran d'abord, puis le travail bloquant.
  // Differer le travail a loop() laissait la derniere image de l'ecran Wi-Fi
  // avec des boutons morts (vu sur appareil).
  state_ = State::SYNCING;
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    requestUpdate(true);
  }
  runSync();
}

void TaskSyncActivity::fail(const Failure failure) {
  failure_ = failure;
  state_ = State::FAILED;
  requestUpdate();
}

bool TaskSyncActivity::heapAllowsTls(const char* stage) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  LOG_INF(TAG, "Heap %s: %u free, %u max alloc", stage, freeHeap, maxAllocHeap);
  if (freeHeap < MIN_FREE_HEAP_FOR_TLS || maxAllocHeap < MIN_MAX_ALLOC_HEAP_FOR_TLS) {
    LOG_ERR(TAG, "Insufficient heap for TLS (%s): %u free, %u max alloc", stage, freeHeap, maxAllocHeap);
    return false;
  }
  return true;
}

void TaskSyncActivity::closeNoteFile() {
  if (noteFile_) noteFile_.close();
  noteId_[0] = '\0';
}

void TaskSyncActivity::runSync() {
  syncSystemClockFromRtc();

  const std::string base = serverBaseUrl();
  if (base.empty()) {
    fail(Failure::NotConfigured);
    return;
  }

  // Envoye EXACTEMENT tel que stocke : le serveur hache l'en-tete tel quel,
  // sans normalisation — un espace ou une minuscule de trop, et c'est 401 pour
  // toujours. Jamais journalise, a aucun niveau.
  char secret[TASK_SECRET_LEN + 8] = {};
  if (!TASK_STORE.readSecret(secret, sizeof(secret)) || secret[0] == '\0') {
    fail(Failure::NotPaired);
    return;
  }
  std::string auth = std::string("Bearer ") + secret;
  memset(secret, 0, sizeof(secret));

  if (!heapAllowsTls("before sync")) {
    fail(Failure::LowMemory);
    return;
  }

  Storage.mkdir(TaskStore::stagedNotesDir());
  const std::string url = base + "/api/v1/tasks/sync";
  // Un seul client pour tous les tours : il garde la connexion ouverte, donc
  // une seule poignee de main TLS par sync.
  freeink::SecureHttpClient http;
  http.setCACert(ISRG_ROOT_X1_PEM);

  size_t opsOffset = 0;
  bool allOpsSent = false;
  for (int round = 0; round < MAX_ROUNDS; round++) {
    size_t opsSent = 0;
    bool opsExhausted = false;
    const Failure failure = runRound(http, url, auth, opsOffset, opsSent, opsExhausted);
    if (failure != Failure::None) {
      // Les tours precedents, eux, ont ete valides un par un (index, notes,
      // curseur) ; la file d'ops est intacte, donc tout ce qu'ils ont envoye
      // repartira a la prochaine sync — sans effet, les ops sont idempotentes.
      http.end();
      fail(failure);
      return;
    }
    opsOffset += opsSent;
    if (!roundMore_ && opsExhausted) {
      allOpsSent = true;
      break;
    }
  }
  http.end();

  // DERNIERE ecriture de la sync, apres que chaque tour a ete accepte : c'est
  // l'ordre inverse d'un echec, ou la file n'est jamais touchee.
  if (allOpsSent) {
    TASK_STORE.clearOps();
  } else {
    LOG_ERR(TAG, "Stopped after %d rounds with ops still queued; they stay for the next sync", MAX_ROUNDS);
  }

  sent_ = static_cast<int>(opsOffset);
  total_ = static_cast<int>(TASK_STORE.all().size());
#ifndef SIMULATOR
  LOG_INF(TAG, "Stack high-water mark: %u bytes", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
  LOG_INF(TAG, "Sync complete: %d received, %d sent, %d total, %d rejected", received_, sent_, total_, rejectedCount_);
  state_ = State::DONE;
  requestUpdate();
}

TaskSyncActivity::Failure TaskSyncActivity::runRound(freeink::SecureHttpClient& http, const std::string& url,
                                                     const std::string& auth, const size_t opsOffset, size_t& opsSent,
                                                     bool& opsExhausted) {
  char cursor[TASK_CURSOR_BUF];
  if (!TASK_STORE.readCursor(cursor, sizeof(cursor))) cursor[0] = '\0';

  // Les deux tampons sont sur le tas : 50 ops font ~10,7 Ko et le corps
  // jusqu'a 12 Ko, bien trop pour la pile de 8 Ko de la boucle principale. Les
  // ops sont rendues des que le corps est assemble, avant la connexion TLS.
  std::unique_ptr<char[]> body;
  size_t bodyLen = 0;
  {
    auto ops = makeUniqueNoThrow<TaskOp[]>(TASK_MAX_OPS_PER_SYNC);
    if (!ops) {
      LOG_ERR(TAG, "OOM: ops batch (%u bytes)", static_cast<unsigned>(TASK_MAX_OPS_PER_SYNC * sizeof(TaskOp)));
      return Failure::LowMemory;
    }
    const size_t readCount = TASK_STORE.readOps(ops.get(), TASK_MAX_OPS_PER_SYNC, opsOffset);
    size_t sendCount = readCount;
    // Dimensionne au pire cas des ops lues, pas a 12 Ko d'office : une sync
    // sans op ne demande qu'une centaine d'octets de plus pendant TLS.
    size_t bodyCap = BODY_ENVELOPE + readCount * OP_LINE_BOUND;
    if (bodyCap > TASK_REQUEST_BUF_SIZE) bodyCap = TASK_REQUEST_BUF_SIZE;
    body = makeUniqueNoThrow<char[]>(bodyCap);
    if (!body) {
      LOG_ERR(TAG, "OOM: request body (%u bytes)", static_cast<unsigned>(bodyCap));
      return Failure::LowMemory;
    }
    // 50 ops aux titres de 200 octets echappes ne tiennent pas dans 12 Ko :
    // taskOpsToRequestBody refuse alors tout plutot que d'en ecarter en
    // silence. On reduit la tranche ; les suivantes partent au tour d'apres.
    bodyLen = taskOpsToRequestBody(ops.get(), sendCount, cursor, body.get(), bodyCap);
    while (bodyLen == 0 && sendCount > 1) {
      sendCount /= 2;
      bodyLen = taskOpsToRequestBody(ops.get(), sendCount, cursor, body.get(), bodyCap);
    }
    if (bodyLen == 0) {
      LOG_ERR(TAG, "Could not assemble the request body");
      return Failure::StorageFailed;
    }
    opsSent = sendCount;
    opsExhausted = readCount < TASK_MAX_OPS_PER_SYNC && sendCount == readCount;
  }

  // Le lecteur porte un tampon de ligne de 640 octets : sur le tas aussi.
  const TaskSyncCallbacks callbacks{this, onHeader, onTask, onDeleted, onNoteChunk, onRejected};
  auto reader = makeUniqueNoThrow<TaskSyncReader>(callbacks);
  if (!reader) {
    LOG_ERR(TAG, "OOM: sync reader");
    return Failure::LowMemory;
  }

  if (!heapAllowsTls("before request")) return Failure::LowMemory;
  if (heapFree_ == 0) {
    heapFree_ = ESP.getFreeHeap();
    heapMaxAlloc_ = ESP.getMaxAllocHeap();
  }

  // Etat du tour remis a zero ; les rejets d'un tour jete seront retires.
  roundCursor_[0] = '\0';
  roundMore_ = false;
  roundReset_ = false;
  roundReceived_ = 0;
  roundShownStart_ = shownRejections_;
  roundRejectedStart_ = rejectedCount_;
  noteFailed_ = false;
  TASK_STORE.clearStagedNotes();

  if (!http.begin(url)) {
    LOG_ERR(TAG, "Bad server URL");
    return Failure::NotConfigured;
  }
  http.addHeader("Authorization", auth);
  http.addHeader("Content-Type", "application/json");

  LOG_INF(TAG, "POST %u bytes, %u op(s) from offset %u", static_cast<unsigned>(bodyLen), static_cast<unsigned>(opsSent),
          static_cast<unsigned>(opsOffset));
#ifndef SIMULATOR
  // Chaque morceau va droit au lecteur : rien de plus gros qu'un morceau n'est
  // jamais tenu en RAM, pas meme une note de 4 Ko.
  TaskSyncReader* sink = reader.get();
  const int code = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(body.get()), bodyLen,
                                    [sink](const uint8_t* data, const size_t len) {
                                      sink->feed(reinterpret_cast<const char*>(data), len);
                                      return !sink->hasError();
                                    });
  const bool bodyComplete = http.responseComplete();
#else
  const int code = http.sendRequest("POST", std::string(body.get(), bodyLen));
  const auto response = http.getString();
  const char* bytes = response.c_str();
  const size_t total = response.length();
  for (size_t i = 0; i < total && !reader->hasError(); i += SIMULATOR_FEED_CHUNK) {
    const size_t take = total - i < SIMULATOR_FEED_CHUNK ? total - i : SIMULATOR_FEED_CHUNK;
    reader->feed(bytes + i, take);
  }
  // Le client du simulateur ne dit pas si le corps est arrive entier ; une
  // coupure franche entre deux lignes n'y est donc pas detectable (le lecteur
  // attrape toutes les autres). Sur l'appareil, responseComplete() la voit.
  const bool bodyComplete = code > 0;
#endif
  body.reset();
  closeNoteFile();
  httpCode_ = code;

  const TaskSyncOutcome outcome = classifyTaskSyncResponse(code, bodyComplete, reader->isComplete());
  LOG_INF(TAG, "Response %d, body %s, reader %s", code, bodyComplete ? "complete" : "incomplete",
          reader->isComplete() ? "complete" : (reader->hasError() ? "error" : "unfinished"));

  // ------------------------------------------------------------ echec
  // Rien de persiste ne bouge : pas de curseur, pas d'index, pas de note, pas
  // de file d'ops. La RAM est relue depuis la carte et les notes en attente
  // sont jetees, comme si ce tour n'avait jamais eu lieu.
  if (outcome != TaskSyncOutcome::Commit || noteFailed_) {
    TASK_STORE.discardStaged();
    shownRejections_ = roundShownStart_;
    rejectedCount_ = roundRejectedStart_;
    switch (outcome) {
      case TaskSyncOutcome::Commit:
        LOG_ERR(TAG, "A synced note could not be written; nothing applied");
        return Failure::StorageFailed;
      case TaskSyncOutcome::PairingRequired:
        return Failure::PairingRequired;
      case TaskSyncOutcome::TransportFailed:
        return Failure::Unreachable;
      case TaskSyncOutcome::ServerError:
        return Failure::ServerError;
      case TaskSyncOutcome::BadResponse:
        return Failure::BadResponse;
    }
    return Failure::BadResponse;
  }

  // ------------------------------------------------------------ succes
  // Ordre choisi pour qu'une coupure a n'importe quel point laisse un etat
  // que la sync suivante repare d'elle-meme :
  //  1. l'index d'abord — s'il ne s'ecrit pas, on s'arrete ici sans rien
  //     d'autre de change ;
  //  2. les notes ensuite (sur reset, les anciennes sont effacees avant que
  //     les nouvelles n'arrivent) — une coupure ici laisse au pire une note
  //     manquante, que l'ecran de detail signale ;
  //  3. le curseur en dernier — tant qu'il n'a pas avance, le serveur renverra
  //     les memes lignes.
  // La file d'ops, elle, n'est videe qu'apres le dernier tour (runSync).
  if (!TASK_STORE.saveToFile()) {
    LOG_ERR(TAG, "Could not save the task index; nothing applied");
    TASK_STORE.discardStaged();
    shownRejections_ = roundShownStart_;
    rejectedCount_ = roundRejectedStart_;
    return Failure::StorageFailed;
  }
  if (roundReset_) TASK_STORE.clearAllNotes();
  TASK_STORE.commitStagedNotes();
  TASK_STORE.writeCursor(roundCursor_);
  received_ += roundReceived_;
  return Failure::None;
}

// ------------------------------------------------------------ rappels du lecteur

void TaskSyncActivity::onHeader(void* ctx, const char* cursor, const bool more, const bool reset) {
  auto* self = static_cast<TaskSyncActivity*>(ctx);
  snprintf(self->roundCursor_, sizeof(self->roundCursor_), "%s", cursor);
  self->roundMore_ = more;
  self->roundReset_ = reset;
  // Sur reset, l'index repart de zero AVANT la premiere ligne. En RAM
  // seulement : la carte ne change qu'au commit du tour.
  if (reset) TASK_STORE.stageReset();
}

void TaskSyncActivity::onTask(void* ctx, const TaskRecord& rec) {
  auto* self = static_cast<TaskSyncActivity*>(ctx);
  TASK_STORE.stageUpsert(rec);
  self->roundReceived_++;
}

void TaskSyncActivity::onDeleted(void* ctx, const char* id) {
  auto* self = static_cast<TaskSyncActivity*>(ctx);
  TASK_STORE.stageRemove(id);
  self->roundReceived_++;
}

void TaskSyncActivity::onNoteChunk(void* ctx, const char* id, const char* data, const size_t len, const bool last) {
  auto* self = static_cast<TaskSyncActivity*>(ctx);
  if (!self->noteFile_ || strcmp(self->noteId_, id) != 0) {
    self->closeNoteFile();
    char path[64];
    if (!TaskStore::stagedNotePath(id, path, sizeof(path)) || !Storage.openFileForWrite(TAG, path, self->noteFile_)) {
      LOG_ERR(TAG, "Could not open a staged note for %s", id);
      self->noteFailed_ = true;
      return;
    }
    snprintf(self->noteId_, sizeof(self->noteId_), "%s", id);
  }
  if (self->noteFile_.write(data, len) != len) {
    LOG_ERR(TAG, "Short write on the staged note for %s", id);
    self->noteFailed_ = true;
  }
  if (last) self->closeNoteFile();
}

void TaskSyncActivity::onRejected(void* ctx, const char* id, const char* reason) {
  auto* self = static_cast<TaskSyncActivity*>(ctx);
  self->rejectedCount_++;
  LOG_INF(TAG, "Server refused op on %s: %s", id, reason);
  if (self->shownRejections_ >= kMaxShownRejections) return;
  // Emis avant la premiere ligne de la reponse, donc avant tout reset : la
  // tache est encore dans l'index et on peut la nommer par son titre.
  Rejection& slot = self->rejections_[self->shownRejections_++];
  const TaskRecord* rec = TASK_STORE.find(id);
  snprintf(slot.title, sizeof(slot.title), "%s", rec != nullptr ? rec->title : id);
  snprintf(slot.reason, sizeof(slot.reason), "%s", reason);
}

// ------------------------------------------------------------ entrees

void TaskSyncActivity::loop() {
  // Pas de mappedInput.update() ici, meme raison que Raindrop : un second
  // update sous le debounce consomme le front de relachement.
  if (state_ != State::DONE && state_ != State::FAILED) return;
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

// ------------------------------------------------------------ rendu

void TaskSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  GUI.drawHeader(renderer, header, tr(STR_TASK_SYNC));

  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const int statusTop = safe.y + safe.height - metrics.verticalSpacing - smallLineHeight;
  int y = header.y + header.height + 2 * lineHeight;

  auto drawSmall = [&](const char* text) {
    const auto shown = renderer.truncatedText(SMALL_FONT_ID, text, contentWidth);
    renderer.drawCenteredText(SMALL_FONT_ID, y, shown.c_str());
    y += smallLineHeight;
  };

  char line[96];
  if (state_ != State::WIFI_SELECTION && heapFree_ > 0) {
    snprintf(line, sizeof(line), tr(STR_TASK_SYNC_HEAP), static_cast<unsigned>(heapFree_ / 1024),
             static_cast<unsigned>(heapMaxAlloc_ / 1024));
    renderer.drawCenteredText(SMALL_FONT_ID, statusTop, line);
  }

  switch (state_) {
    case State::WIFI_SELECTION:
      break;
    case State::SYNCING:
      renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_TASK_SYNCING));
      break;
    case State::DONE: {
      renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_TASK_SYNC_DONE));
      y += 2 * lineHeight;
      snprintf(line, sizeof(line), tr(STR_TASK_SYNC_COUNTS), received_, sent_, total_);
      const auto counts = renderer.truncatedText(UI_10_FONT_ID, line, contentWidth);
      renderer.drawCenteredText(UI_10_FONT_ID, y, counts.c_str());
      y += 2 * lineHeight;
      if (rejectedCount_ > 0) {
        drawSmall(tr(STR_TASK_SYNC_REJECTED));
        for (size_t i = 0; i < shownRejections_; i++) {
          // Le motif ne doit jamais etre rogne : seul le titre cede la place.
          char reason[40];
          snprintf(reason, sizeof(reason), " (%s)", rejectionReasonText(rejections_[i].reason));
          const int titleWidth = contentWidth - renderer.getTextWidth(SMALL_FONT_ID, reason);
          const auto title = renderer.truncatedText(SMALL_FONT_ID, rejections_[i].title, titleWidth);
          snprintf(line, sizeof(line), "%s%s", title.c_str(), reason);
          drawSmall(line);
        }
        const int hidden = rejectedCount_ - static_cast<int>(shownRejections_);
        if (hidden > 0) {
          snprintf(line, sizeof(line), tr(STR_TASK_SYNC_MORE_REJECTED), hidden);
          drawSmall(line);
        }
      }
      break;
    }
    case State::FAILED: {
      const bool pairing = failure_ == Failure::PairingRequired || failure_ == Failure::NotPaired;
      renderer.drawCenteredText(UI_10_FONT_ID, y, pairing ? tr(STR_TASK_PAIRING_REQUIRED) : tr(STR_TASK_SYNC_FAILED));
      y += 2 * lineHeight;
      const char* detail = nullptr;
      switch (failure_) {
        case Failure::None:
          break;
        case Failure::WifiFailed:
          detail = tr(STR_WIFI_CONN_FAILED);
          break;
        case Failure::NotConfigured:
          detail = tr(STR_TASK_SYNC_NOT_CONFIGURED);
          break;
        case Failure::NotPaired:
        case Failure::PairingRequired:
          detail = tr(STR_TASK_SYNC_REPAIR);
          break;
        case Failure::LowMemory:
          detail = tr(STR_TASK_SYNC_LOW_MEMORY);
          break;
        case Failure::Unreachable:
          detail = tr(STR_TASK_SYNC_UNREACHABLE);
          break;
        case Failure::ServerError:
          snprintf(line, sizeof(line), tr(STR_TASK_SYNC_SERVER_ERROR), httpCode_);
          detail = line;
          break;
        case Failure::BadResponse:
          detail = tr(STR_TASK_SYNC_BAD_RESPONSE);
          break;
        case Failure::StorageFailed:
          detail = tr(STR_TASK_TICK_FAILED);
          break;
      }
      if (detail != nullptr) drawSmall(detail);
      // Vrai pour tout echec : la file d'ops n'est videe qu'apres succes.
      if (failure_ != Failure::NotConfigured && failure_ != Failure::WifiFailed) {
        y += smallLineHeight;
        drawSmall(tr(STR_TASK_SYNC_KEPT));
      }
      break;
    }
  }

  if (state_ == State::DONE || state_ == State::FAILED) {
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_OK), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  // Sans ce flush, la dalle garderait la derniere image de l'ecran Wi-Fi.
  renderer.displayBuffer(screenTransitionRefresh_.modeFor(static_cast<uint8_t>(state_)));
}
