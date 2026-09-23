#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "tasks/TaskRecord.h"
#include "tasks/TaskSyncReader.h"

namespace freeink {
class SecureHttpClient;
}

// Synchronisation manuelle des taches avec le serveur CrossDrop, dans les deux
// sens : envoie la file d'ops, applique les changements du serveur, resume.
//
// Jumelle de RaindropSyncActivity : ne tourne que depuis un demarrage reseau
// minimal (NetworkBootTarget::TASK_SYNC), et redemarre vers l'appli complete a
// la sortie pour rendre le tas que la session Wi-Fi a fragmente.
//
// Regle de persistance : chaque requete est un tour qui ne touche la carte
// qu'une fois sa reponse ENTIEREMENT acceptee (voir TaskSyncOutcome.h). Tant
// que le tour n'est pas valide, la reponse ne vit qu'en RAM et dans le dossier
// de notes en attente de TaskStore. La file d'ops n'est videe qu'apres le
// dernier tour reussi.
class TaskSyncActivity final : public Activity {
 public:
  // returnToTaskList : lancee depuis le menu de la liste, la sync y ramene a la
  // fermeture ; depuis les Parametres, elle ramene a l'accueil comme avant.
  TaskSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool returnToTaskList = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // La sync tourne de facon bloquante dans le rappel Wi-Fi ; le gestionnaire
  // n'interroge donc ceci que sur les ecrans de fin, ou le resume doit rester.
  bool preventAutoSleep() override { return state_ == State::SYNCING || state_ == State::DONE; }

 private:
  const bool returnToTaskList_;
  enum class State : uint8_t { WIFI_SELECTION, SYNCING, DONE, FAILED };

  enum class Failure : uint8_t {
    None,
    WifiFailed,
    NotConfigured,
    NotPaired,
    PairingRequired,
    LowMemory,
    Unreachable,
    ServerError,
    BadResponse,
    StorageFailed,
  };

  // Le resume nomme chaque op refusee : l'appareil l'a deja retiree de sa file,
  // donc taire le refus serait la perdre sans le dire. Au-dela de cinq, un
  // compteur. Dans l'objet (tas) plutot que sur la pile : ~1,1 Ko.
  static constexpr size_t kMaxShownRejections = 5;
  struct Rejection {
    char id[TASK_ID_LEN + 1];
    char title[TASK_TITLE_MAX + 1];
    char reason[16];
    bool titled;
  };

  void onWifiSelectionComplete(bool connected);
  void runSync();
  Failure runRound(freeink::SecureHttpClient& http, const std::string& url, const std::string& auth, size_t opsOffset,
                   size_t& opsSent, bool& opsExhausted);
  bool heapAllowsTls(const char* stage);
  void rollBackRoundRejections();
  void resolveRejectionTitles();
  void fail(Failure failure);
  void closeNoteFile();

  static void onHeader(void* ctx, const char* cursor, bool more, bool reset);
  static void onTask(void* ctx, const TaskRecord& rec);
  static void onDeleted(void* ctx, const char* id);
  static void onNoteChunk(void* ctx, const char* id, const char* data, size_t len, bool last);
  static void onRejected(void* ctx, const char* id, const char* reason);

  State state_ = State::WIFI_SELECTION;
  Failure failure_ = Failure::None;
  int httpCode_ = 0;
  ScreenTransitionRefresh screenTransitionRefresh_;

  // Tas mesure juste avant la premiere requete TLS, affiche pour la
  // verification sur appareil.
  uint32_t heapFree_ = 0;
  uint32_t heapMaxAlloc_ = 0;

  // Resume, cumule sur les tours valides seulement.
  int received_ = 0;
  int sent_ = 0;
  int total_ = 0;
  // Vrai si MAX_ROUNDS a ete atteint avec du travail restant : le resume ne
  // doit pas dire « terminee ».
  bool incomplete_ = false;
  Rejection rejections_[kMaxShownRejections] = {};
  size_t shownRejections_ = 0;
  int rejectedCount_ = 0;

  // Etat du tour en cours ; jete si le tour echoue.
  char roundCursor_[TASK_SYNC_CURSOR_MAX + 1] = {};
  bool roundMore_ = false;
  bool roundReset_ = false;
  bool roundFullSnapshot_ = false;
  int roundReceived_ = 0;
  size_t roundShownStart_ = 0;
  int roundRejectedStart_ = 0;
  HalFile noteFile_;
  char noteId_[TASK_ID_LEN + 1] = {};
  bool noteFailed_ = false;
};
