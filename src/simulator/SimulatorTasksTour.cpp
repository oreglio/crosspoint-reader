#ifdef SIMULATOR

#include "SimulatorTasksTour.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "TaskStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/tasks/TaskPairActivity.h"
#include "components/UITheme.h"

extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;
extern ActivityManager activityManager;

namespace {

using Button = MappedInputManager::Button;

enum class Act : uint8_t { ColdPair, OpenList, Tap, Hold, SeedAllDone, SeedEmpty, OpenPair, Finish };

// Relache d'un Hold : au-dela du seuil de 800 ms de la liste, avant la capture
// (kShotOffsetMs) pour que la popup soit deja peinte.
constexpr unsigned long kHoldReleaseMs = 850;

struct TourStep {
  const char* name;
  Act act;
  Button button;
};

// Le script de capture (scripts/run_simulator_tasks_tour.py) tient la MEME
// liste, dans le meme ordre, avec les memes kStartMs / kPeriodMs : chaque
// capture tombe a kStartMs + i * kPeriodMs + kShotOffsetMs. Toute etape
// ajoutee ici doit l'etre la-bas aussi, sinon les noms de fichiers glissent.
constexpr TourStep kSteps[] = {
    // Premiere etape, avant toute visite de la liste : l'ecran d'appairage
    // s'ouvre sur un store jamais charge, comme apres un redemarrage. Il doit
    // montrer le secret seme par le script (SEEDED_SECRET), pas en tirer un.
    {"00-pair-cold-boot", Act::ColdPair, Button::Confirm},
    {"01-list", Act::OpenList, Button::Confirm},
    {"move-down", Act::Tap, Button::Down},
    {"02-ticked-sinks", Act::Tap, Button::Confirm},
    {"move-up", Act::Tap, Button::Up},
    {"03-done-row-selected", Act::Tap, Button::Up},
    {"04-done-expanded", Act::Tap, Button::Confirm},
    {"reopen", Act::OpenList, Button::Confirm},
    {"05-detail-with-note", Act::Tap, Button::Right},
    // Tache haute priorite cochee depuis son detail : le titre perd le gras,
    // comme sur la liste, et Confirmer se lit « rouvrir » (« reopen »).
    {"05b-detail-ticked", Act::Tap, Button::Confirm},
    {"back-to-list", Act::Tap, Button::Back},
    // Menu d'une tache par appui long, puis suppression confirmee : la tache
    // disparait et le titre garde la marque d'envoi en attente.
    {"13-task-menu", Act::Hold, Button::Confirm},
    {"menu-down-1", Act::Tap, Button::Down},
    {"menu-down-2", Act::Tap, Button::Down},
    {"menu-down-3", Act::Tap, Button::Down},
    {"menu-down-4", Act::Tap, Button::Down},
    {"14-menu-delete-selected", Act::Tap, Button::Down},
    {"15-delete-confirm", Act::Tap, Button::Confirm},
    // La confirmation s'ouvre sur Annuler, comme pour un livre : Bas d'abord.
    {"confirm-down", Act::Tap, Button::Down},
    {"16-deleted", Act::Tap, Button::Confirm},
    // Modifier depuis le menu : le clavier s'ouvre sur le titre actuel.
    {"menu-again", Act::Hold, Button::Confirm},
    {"menu-edit-1", Act::Tap, Button::Down},
    {"17-menu-edit-selected", Act::Tap, Button::Down},
    {"18-edit-keyboard", Act::Tap, Button::Confirm},
    {"leave-keyboard", Act::Tap, Button::Back},
    {"06-all-done", Act::SeedAllDone, Button::Confirm},
    {"07-empty", Act::SeedEmpty, Button::Confirm},
    {"08-add-keyboard", Act::Tap, Button::Left},
    {"09-pair", Act::OpenPair, Button::Confirm},
    {"10-pair-confirm", Act::Tap, Button::Right},
    {"11-pair-cancelled", Act::Tap, Button::Confirm},
    {"ask-again", Act::Tap, Button::Right},
    {"pick-confirm", Act::Tap, Button::Down},
    {"12-pair-renewed", Act::Tap, Button::Confirm},
    {"end", Act::Finish, Button::Confirm},
};
constexpr size_t kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

// CROSSINK_SIMULATOR_TASKS_TOUR=keepawake : active « Garder l'ecran allume »
// par le menu, puis laisse l'appareil inactif ; l'appelant regarde si le
// journal annonce une mise en veille. Pas d'etape Finish : c'est l'appelant
// qui arrete le simulateur, apres le delai de veille.
constexpr TourStep kKeepAwakeSteps[] = {
    {"list", Act::OpenList, Button::Confirm}, {"hold", Act::Hold, Button::Confirm},
    {"down-1", Act::Tap, Button::Down},       {"down-2", Act::Tap, Button::Down},
    {"down-3", Act::Tap, Button::Down},       {"toggle", Act::Tap, Button::Confirm},
    {"detail", Act::Tap, Button::Right},
};
constexpr size_t kKeepAwakeStepCount = sizeof(kKeepAwakeSteps) / sizeof(kKeepAwakeSteps[0]);

constexpr unsigned long kStartMs = 3500;
constexpr unsigned long kPeriodMs = 1300;

bool enabled() { return std::getenv("CROSSINK_SIMULATOR_TASKS_TOUR") != nullptr; }

void applyRequestedTheme() {
  const char* raw = std::getenv("CROSSINK_SIMULATOR_TASKS_THEME");
  if (raw == nullptr || raw[0] == '\0') return;
  const int theme = std::atoi(raw);
  if (theme < 0 || theme >= CrossPointSettings::UI_THEME_COUNT) {
    LOG_ERR("TOUR", "Invalid theme index %d", theme);
    return;
  }
  SETTINGS.uiTheme = static_cast<uint8_t>(theme);
  UITheme::getInstance().reload();
  LOG_INF("TOUR", "theme %d", theme);
  // Reglage « Espacement des taches » a capturer (0 par defaut).
  const char* spacing = std::getenv("CROSSINK_SIMULATOR_TASKS_SPACING");
  if (spacing != nullptr && spacing[0] >= '0' && spacing[0] <= '2') {
    SETTINGS.taskRowSpacing = static_cast<uint8_t>(spacing[0] - '0');
  }
  const char* font = std::getenv("CROSSINK_SIMULATOR_TASKS_FONT");
  if (font != nullptr && font[0] >= '0' && font[0] <= '2') {
    SETTINGS.taskFontSize = static_cast<uint8_t>(font[0] - '0');
  }
}

// Le francais est la langue ou les libelles debordent : c'est lui qu'il faut
// voir, pas seulement l'anglais par defaut du simulateur.
void applyRequestedLanguage() {
  const char* raw = std::getenv("CROSSINK_SIMULATOR_TASKS_LANG");
  if (raw == nullptr || raw[0] == '\0') return;
  const int lang = std::atoi(raw);
  SETTINGS.language = static_cast<uint8_t>(lang);
  I18N.setLanguage(static_cast<Language>(lang));
  LOG_INF("TOUR", "language %d", lang);
}

TaskRecord makeRecord(const char* id, const char* title, uint8_t priority, bool done) {
  TaskRecord rec{};
  std::snprintf(rec.id, sizeof(rec.id), "%s", id);
  std::snprintf(rec.title, sizeof(rec.title), "%s", title);
  rec.priority = priority;
  rec.done = done;
  rec.noteBytes = 0;
  return rec;
}

void seedAllDone() {
  std::vector<TaskRecord> all;
  all.push_back(makeRecord("d000000a1", "Envoyer le devis", TASK_PRIORITY_HIGH, true));
  all.push_back(makeRecord("d000000a2", "Relire le chapitre 3", TASK_PRIORITY_NORMAL, true));
  all.push_back(makeRecord("d000000a3", "Racheter du cafe", TASK_PRIORITY_LOW, true));
  TASK_STORE.replaceAll(std::move(all));
}

void openList() {
  TASK_STORE.ensureLoaded();
  activityManager.goToTaskList();
}

}  // namespace

void runSimulatorTasksTourTick() {
  static bool initialized = false;
  static bool active = false;
  static const TourStep* steps = kSteps;
  static size_t stepCount = kStepCount;
  static size_t fired = 0;  // etapes dont l'action a ete lancee
  // Un front d'entree doit durer UNE trame : injecte, il reste vrai a chaque
  // boucle jusqu'a ce qu'on l'efface. Par le temps, il durait ~20 trames et
  // Confirmer basculait la tache une vingtaine de fois. D'ou un compteur de
  // trames : appui -> (trame suivante) relachement -> (suivante) trame propre.
  static uint8_t tapStage = 0;  // 0 rien, 1 appui injecte, 2 relachement injecte, 3-4 maintien

  if (!initialized) {
    initialized = true;
    active = enabled();
    const char* mode = std::getenv("CROSSINK_SIMULATOR_TASKS_TOUR");
    if (mode != nullptr && std::strncmp(mode, "keepawake", 9) == 0) {
      steps = kKeepAwakeSteps;
      // « keepawake-control » : meme parcours sans la validation finale, le
      // temoin qui doit, lui, s'endormir.
      // « keepawake-detail » : puis ouvre le detail d'une tache (l'option doit
      // y valoir aussi) ; « keepawake » s'arrete sur la liste.
      stepCount = std::strcmp(mode, "keepawake-control") == 0  ? kKeepAwakeStepCount - 2
                  : std::strcmp(mode, "keepawake-detail") == 0 ? kKeepAwakeStepCount
                                                               : kKeepAwakeStepCount - 1;
    }
    if (active) LOG_INF("TOUR", "tasks tour enabled, %u steps", static_cast<unsigned>(stepCount));
  }
  if (!active) return;

  const unsigned long now = millis();
  if (now < kStartMs) return;

  const size_t index = (now - kStartMs) / kPeriodMs;
  const unsigned long phase = (now - kStartMs) % kPeriodMs;
  if (index >= stepCount) return;

  if (index >= fired) {
    // Nouvelle etape : lancer son action une seule fois.
    fired = index + 1;
    tapStage = 0;
    const TourStep& step = steps[index];
    LOG_INF("TOUR", "step %u %s", static_cast<unsigned>(index), step.name);
    if (index == 0) {
      applyRequestedLanguage();
      applyRequestedTheme();
    }
    switch (step.act) {
      case Act::ColdPair:
      case Act::OpenPair:
        activityManager.replaceActivity(std::make_unique<TaskPairActivity>(renderer, mappedInputManager));
        break;
      case Act::OpenList:
        openList();
        break;
      case Act::Tap:
        mappedInputManager.simulatorInjectPress(step.button);
        tapStage = 1;
        break;
      case Act::Hold:
        mappedInputManager.simulatorInjectPress(step.button);
        tapStage = 3;
        break;
      case Act::SeedAllDone:
        seedAllDone();
        openList();
        break;
      case Act::SeedEmpty:
        TASK_STORE.replaceAll({});
        openList();
        break;
      case Act::Finish:
        LOG_INF("TOUR", "tour complete");
        std::_Exit(0);  // pas std::exit : ~ActivityManager affirme ne jamais etre detruit
    }
    return;
  }

  // Chaque appel = une iteration de la boucle principale, apres
  // activityManager.loop() : ce qu'on injecte ici est vu par la boucle suivante.
  if (tapStage == 3) {
    // Front d'appui vu une seule fois, comme pour un Tap ; le bouton reste
    // tenu (simulatorHeld) jusqu'a la relache.
    tapStage = 4;
    mappedInputManager.simulatorClearInputFrame();
    return;
  }
  if (tapStage == 4) {
    if (phase < kHoldReleaseMs) return;
    tapStage = 1;
  }
  if (tapStage == 1) {
    tapStage = 2;
    mappedInputManager.simulatorClearInputFrame();
    mappedInputManager.simulatorInjectRelease(steps[index].button);
  } else if (tapStage == 2) {
    tapStage = 0;
    mappedInputManager.simulatorClearInputFrame();
  }
}

#endif
