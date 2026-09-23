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
  // Avant toute lecture : hasSecret()/readSecret() ne chargent pas le store,
  // et un store jamais charge se lit comme non appaire — 401 pour toujours.
  // main.cpp le charge deja dans la branche TASK_SYNC ; sans effet dans ce cas.
  TASK_STORE.ensureLoaded();
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

  // Les lignes recues grossissent `records` depuis le rappel TLS : sa capacite
  // est portee a MAX_TASKS maintenant, avant toute connexion, quand le tas est
  // au mieux (voir TaskStore::reserveFullCapacity). La marge TLS, elle, est
  // reverifiee par la garde "before request" une fois ce bloc pris.
  if (!TASK_STORE.reserveFullCapacity()) {
    fail(Failure::LowMemory);
    return;
  }

  Storage.mkdir(TaskStore::stagedNotesDir());
  const std::string url = base + "/api/v1/tasks/sync";
  freeink::SecureHttpClient http;
  http.setCACert(ISRG_ROOT_X1_PEM);

  // Les ops deja acceptees par une sync precedente interrompue ne repartent
  // pas : leur rejeu n'est pas sans effet (voir TaskStore::readAckedOps).
  const size_t alreadyAcked = TASK_STORE.readAckedOps();
  size_t opsOffset = alreadyAcked;
  bool allOpsSent = false;
  for (int round = 0; round < MAX_ROUNDS; round++) {
    size_t opsSent = 0;
    bool opsExhausted = false;
    const Failure failure = runRound(http, url, auth, opsOffset, opsSent, opsExhausted);
    if (failure != Failure::None) {
      // Les tours precedents ont ete valides un par un, compteur d'ops
      // acquittees compris : la file reste entiere, mais la prochaine sync
      // reprend apres les ops deja acceptees au lieu de les rejouer.
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
    // Le compteur d'ops acquittees est a jour : la prochaine sync reprendra la.
    LOG_ERR(TAG, "Stopped after %d rounds with work left; it stays for the next sync", MAX_ROUNDS);
    incomplete_ = true;
  }

  sent_ = static_cast<int>(opsOffset - alreadyAcked);
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
  // Sans curseur, le serveur rend un instantane complet mais sans tombstones
  // (reset:false) : c'est l'index entier qu'il decrit, donc on le remplace
  // comme sur un reset. Sinon une tache supprimee sur le web pendant que le
  // curseur manquait resterait sur la liseuse pour toujours.
  roundFullSnapshot_ = cursor[0] == '\0';
  roundReceived_ = 0;
  roundShownStart_ = shownRejections_;
  roundRejectedStart_ = rejectedCount_;
  noteFailed_ = false;
  if (!TASK_STORE.clearStagedNotes()) {
    // Un fichier perime laisse dans ns/ pourrait etre promu par-dessus une
    // note a jour : on ne lance pas le tour.
    return Failure::StorageFailed;
  }

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
  // La connexion est fermee avant toute ecriture : la session TLS rend son tas
  // (tampons wolfSSL) au moment ou le commit en a le plus besoin — le
  // JsonDocument de l'index et le rechargement d'un echec. Le prix est une
  // poignee de main par tour, et la plupart des syncs n'ont qu'un tour.
  http.end();
  body.reset();
  closeNoteFile();
  httpCode_ = code;
  const bool readerComplete = reader->isComplete();
  const bool readerError = reader->hasError();
  reader.reset();

  const TaskSyncOutcome outcome = classifyTaskSyncResponse(code, bodyComplete, readerComplete);
  LOG_INF(TAG, "Response %d, body %s, reader %s", code, bodyComplete ? "complete" : "incomplete",
          readerComplete ? "complete" : (readerError ? "error" : "unfinished"));
  resolveRejectionTitles();

  // ------------------------------------------------------------ echec
  // Rien de persiste ne bouge : pas de curseur, pas d'index, pas de note, pas
  // de file d'ops. La RAM est relue depuis la carte et les notes en attente
  // sont jetees, comme si ce tour n'avait jamais eu lieu.
  if (outcome != TaskSyncOutcome::Commit || noteFailed_) {
    TASK_STORE.discardStaged();
    rollBackRoundRejections();
    switch (outcome) {
      case TaskSyncOutcome::Commit:
        LOG_ERR(TAG, "A synced note could not be written; nothing applied");
        return Failure::StorageFailed;
      case TaskSyncOutcome::PairingRequired:
        return Failure::PairingRequired;
      case TaskSyncOutcome::TransportFailed:
        return Failure::Unreachable;
      case TaskSyncOutcome::ServerError:
        // Seule exception a « rien ne bouge » : un 400 dit que la requete est
        // invalide, et la seule partie qui persiste d'une sync a l'autre est le
        // curseur. Le garder, c'est echouer ainsi pour toujours ; l'effacer
        // coute un instantane complet. La file, elle, reste intacte.
        if (code == 400) {
          LOG_ERR(TAG, "Server refused the request (400); dropping the cursor for a full snapshot");
          TASK_STORE.clearCursor();
        }
        return Failure::ServerError;
      case TaskSyncOutcome::BadResponse:
        return Failure::BadResponse;
    }
    return Failure::BadResponse;
  }

  // ------------------------------------------------------------ succes
  // Ordre choisi pour qu'une coupure a n'importe quel point laisse un etat
  // que la sync suivante repare d'elle-meme :
  //  1. le compteur d'ops acquittees EN PREMIER — un 2xx complet prouve que le
  //     serveur a accepte la tranche, quoi qu'il arrive ensuite a la carte, et
  //     ces ops ne doivent plus repartir (leur rejeu ecraserait une
  //     modification faite entre-temps sur le web). S'il ne s'ecrit pas, rien
  //     d'autre ne change ;
  //  2. l'index (tmp puis remplacement, promu au chargement s'il le faut) —
  //     s'il ne s'ecrit pas, le curseur est efface, et l'instantane complet de
  //     la sync suivante reflete deja les ops acquittees ;
  //  3. les notes (sur reset, les anciennes d'abord) — un echec arrete le tour
  //     sans avancer le curseur, et le serveur renverra les lignes et leurs
  //     notes ;
  //  4. le curseur en dernier — tant qu'il n'a pas avance, le serveur renvoie
  //     les memes lignes.
  // La file d'ops, elle, n'est videe qu'apres le dernier tour (runSync).
  LOG_INF(TAG, "Heap before commit: %u free, %u max alloc", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  if (!TASK_STORE.writeAckedOps(opsOffset + opsSent)) {
    TASK_STORE.discardStaged();
    rollBackRoundRejections();
    return Failure::StorageFailed;
  }
  // A partir d'ici les ops du tour sont acquittees et ne repartiront plus :
  // leurs rejets doivent rester affiches, meme si la suite echoue.
  if (!TASK_STORE.saveIndex()) {
    LOG_ERR(TAG, "Could not save the task index; nothing applied, cursor dropped");
    TASK_STORE.discardStaged();
    // Etat de l'index inconnu : sans curseur, la prochaine sync repart d'un
    // instantane complet, qui le reconstruit quel qu'il soit.
    TASK_STORE.clearCursor();
    return Failure::StorageFailed;
  }
  bool notesOk = roundReset_ ? TASK_STORE.clearAllNotes() : true;
  notesOk = TASK_STORE.commitStagedNotes() && notesOk;
  if (!notesOk) {
    LOG_ERR(TAG, "Synced notes not all in place; the cursor stays so the server resends them");
    return Failure::StorageFailed;
  }
  received_ += roundReceived_;
  if (!TASK_STORE.writeCursor(roundCursor_)) {
    LOG_ERR(TAG, "Could not save the cursor; dropping it for a full snapshot next time");
    TASK_STORE.clearCursor();
    return Failure::StorageFailed;
  }
  return Failure::None;
}

void TaskSyncActivity::rollBackRoundRejections() {
  shownRejections_ = roundShownStart_;
  rejectedCount_ = roundRejectedStart_;
}

// Hors du rappel TLS : une tache absente de l'index (retiree par un reset d'un
// tour precedent, par exemple) se nomme par le titre de son op en file — la
// lecture SD n'a rien a faire dans le rappel de reception.
void TaskSyncActivity::resolveRejectionTitles() {
  for (size_t i = roundShownStart_; i < shownRejections_; i++) {
    Rejection& slot = rejections_[i];
    if (slot.titled) continue;
    slot.titled = TASK_STORE.findQueuedTitle(slot.id, slot.title, sizeof(slot.title));
    if (!slot.titled) snprintf(slot.title, sizeof(slot.title), "%s", slot.id);
  }
}

// ------------------------------------------------------------ rappels du lecteur

void TaskSyncActivity::onHeader(void* ctx, const char* cursor, const bool more, const bool reset) {
  auto* self = static_cast<TaskSyncActivity*>(ctx);
  snprintf(self->roundCursor_, sizeof(self->roundCursor_), "%s", cursor);
  self->roundMore_ = more;
  // Sur reset, ou sur l'instantane complet d'une sync sans curseur, l'index
  // repart de zero AVANT la premiere ligne. En RAM seulement : la carte ne
  // change qu'au commit du tour.
  self->roundReset_ = reset || self->roundFullSnapshot_;
  if (self->roundReset_) TASK_STORE.stageReset();
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
  // Emis avant la premiere ligne de la reponse, donc avant le reset de CE
  // tour : si la tache est dans l'index, on la nomme tout de suite. Sinon
  // (retiree par un tour precedent), resolveRejectionTitles() cherchera son
  // titre dans la file, apres la reponse.
  Rejection& slot = self->rejections_[self->shownRejections_++];
  snprintf(slot.id, sizeof(slot.id), "%s", id);
  snprintf(slot.reason, sizeof(slot.reason), "%s", reason);
  const TaskRecord* rec = TASK_STORE.find(id);
  slot.titled = rec != nullptr;
  if (slot.titled) snprintf(slot.title, sizeof(slot.title), "%s", rec->title);
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
  auto drawRejections = [&]() {
    if (rejectedCount_ <= 0) return;
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
  };

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
      renderer.drawCenteredText(UI_10_FONT_ID, y, incomplete_ ? tr(STR_TASK_SYNC_INCOMPLETE) : tr(STR_TASK_SYNC_DONE));
      y += 2 * lineHeight;
      snprintf(line, sizeof(line), tr(STR_TASK_SYNC_COUNTS), received_, sent_, total_);
      const auto counts = renderer.truncatedText(UI_10_FONT_ID, line, contentWidth);
      renderer.drawCenteredText(UI_10_FONT_ID, y, counts.c_str());
      y += 2 * lineHeight;
      if (incomplete_) {
        drawSmall(tr(STR_TASK_SYNC_WORK_LEFT));
        y += smallLineHeight;
      }
      drawRejections();
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
      // Les rejets des tours deja valides restent a dire : ces ops sont
      // acquittees et ne repartiront pas.
      y += smallLineHeight;
      drawRejections();
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
