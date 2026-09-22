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

enum class Act : uint8_t { OpenList, Tap, SeedAllDone, SeedEmpty, OpenPair, Finish };

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
    {"01-list", Act::OpenList, Button::Confirm},
    {"move-down", Act::Tap, Button::Down},
    {"02-ticked-in-place", Act::Tap, Button::Confirm},
    {"move-up", Act::Tap, Button::Up},
    {"03-done-row-selected", Act::Tap, Button::Up},
    {"04-done-expanded", Act::Tap, Button::Confirm},
    {"reopen", Act::OpenList, Button::Confirm},
    {"05-detail-with-note", Act::Tap, Button::Right},
    {"back-to-list", Act::Tap, Button::Back},
    {"06-all-done", Act::SeedAllDone, Button::Confirm},
    {"07-empty", Act::SeedEmpty, Button::Confirm},
    {"08-add-keyboard", Act::Tap, Button::Left},
    {"09-pair", Act::OpenPair, Button::Confirm},
    {"10-pair-renewed", Act::Tap, Button::Right},
    {"end", Act::Finish, Button::Confirm},
};
constexpr size_t kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

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
  static size_t fired = 0;  // etapes dont l'action a ete lancee
  // Un front d'entree doit durer UNE trame : injecte, il reste vrai a chaque
  // boucle jusqu'a ce qu'on l'efface. Par le temps, il durait ~20 trames et
  // Confirmer basculait la tache une vingtaine de fois. D'ou un compteur de
  // trames : appui -> (trame suivante) relachement -> (suivante) trame propre.
  static uint8_t tapStage = 0;  // 0 rien, 1 appui injecte, 2 relachement injecte

  if (!initialized) {
    initialized = true;
    active = enabled();
    if (active) LOG_INF("TOUR", "tasks tour enabled, %u steps", static_cast<unsigned>(kStepCount));
  }
  if (!active) return;

  const unsigned long now = millis();
  if (now < kStartMs) return;

  const size_t index = (now - kStartMs) / kPeriodMs;
  const unsigned long phase = (now - kStartMs) % kPeriodMs;
  if (index >= kStepCount) return;

  if (index >= fired) {
    // Nouvelle etape : lancer son action une seule fois.
    fired = index + 1;
    tapStage = 0;
    const TourStep& step = kSteps[index];
    LOG_INF("TOUR", "step %u %s", static_cast<unsigned>(index), step.name);
    switch (step.act) {
      case Act::OpenList:
        if (index == 0) {
          applyRequestedLanguage();
          applyRequestedTheme();
        }
        openList();
        break;
      case Act::Tap:
        mappedInputManager.simulatorInjectPress(step.button);
        tapStage = 1;
        break;
      case Act::SeedAllDone:
        seedAllDone();
        openList();
        break;
      case Act::SeedEmpty:
        TASK_STORE.replaceAll({});
        openList();
        break;
      case Act::OpenPair:
        activityManager.replaceActivity(std::make_unique<TaskPairActivity>(renderer, mappedInputManager));
        break;
      case Act::Finish:
        LOG_INF("TOUR", "tour complete");
        std::_Exit(0);  // pas std::exit : ~ActivityManager affirme ne jamais etre detruit
    }
    return;
  }

  // Chaque appel = une iteration de la boucle principale, apres
  // activityManager.loop() : ce qu'on injecte ici est vu par la boucle suivante.
  if (tapStage == 1) {
    tapStage = 2;
    mappedInputManager.simulatorClearInputFrame();
    mappedInputManager.simulatorInjectRelease(kSteps[index].button);
  } else if (tapStage == 2) {
    tapStage = 0;
    mappedInputManager.simulatorClearInputFrame();
  }
  (void)phase;
}

#endif
