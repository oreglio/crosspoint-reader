#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

// Largeurs fixes plutôt que std::string : 120 enregistrements tiennent dans un
// tableau plat que l'écran de liste alloue en onEnter() et rend en onExit(),
// sans une seule allocation par tâche ni fragmentation du tas du C3.
inline constexpr size_t MAX_TASKS = 120;
inline constexpr size_t TASK_ID_LEN = 9;       // 'd' + 8 hex
inline constexpr size_t TASK_TITLE_MAX = 200;  // octets, hors terminateur
inline constexpr size_t TASK_NOTE_MAX = 4096;

inline constexpr uint8_t TASK_PRIORITY_HIGH = 0;
inline constexpr uint8_t TASK_PRIORITY_NORMAL = 1;
inline constexpr uint8_t TASK_PRIORITY_LOW = 2;

struct TaskRecord {
  char id[TASK_ID_LEN + 1];
  char title[TASK_TITLE_MAX + 1];
  uint8_t priority;
  bool done;
  uint16_t noteBytes;
};

// Ordre d'affichage : les ouvertes d'abord, par priorité croissante (0 = haute),
// puis l'id pour que le tri soit total — deux tâches de même priorité ne doivent
// jamais changer de place d'un rendu à l'autre.
//
// doneA/doneB séparés de a.done/b.done : l'écran de liste doit pouvoir trier
// une tâche cochée pendant la visite en cours comme si elle était encore
// ouverte (elle reste en place, atténuée, jusqu'à la sortie de l'écran) sans
// construire une copie de TaskRecord par comparaison rien que pour forcer ce
// bit — sizeof(TaskRecord) est 216 octets, et un comparateur de tri en copie
// deux par appel.
inline bool taskOrderBefore(const TaskRecord& a, const TaskRecord& b, bool doneA, bool doneB) {
  if (doneA != doneB) return !doneA;
  if (a.priority != b.priority) return a.priority < b.priority;
  return std::strcmp(a.id, b.id) < 0;
}

inline bool taskOrderBefore(const TaskRecord& a, const TaskRecord& b) { return taskOrderBefore(a, b, a.done, b.done); }
