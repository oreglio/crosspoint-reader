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
  // Rang manuel DANS la priorite, pose par le serveur (glisser-deposer du web).
  // TASK_ORDER_UNSET : pas encore classee — tache creee ou changee de priorite
  // sur l'appareil, ou index d'avant l'ordre manuel. Elle se range alors au
  // bout de son groupe jusqu'a ce qu'une sync lui rende son rang.
  uint16_t order;
};

inline constexpr uint16_t TASK_ORDER_UNSET = 0xFFFF;

// Ordre d'affichage : les ouvertes d'abord, par priorite croissante (0 = haute),
// puis le rang manuel du web, puis l'id pour que le tri soit total — deux
// taches de meme rang ne doivent jamais changer de place d'un rendu a l'autre.
// Le serveur trie pareil (TaskDb.listAll : priorite, position, id).
inline bool taskOrderBefore(const TaskRecord& a, const TaskRecord& b) {
  if (a.done != b.done) return !a.done;
  if (a.priority != b.priority) return a.priority < b.priority;
  if (a.order != b.order) return a.order < b.order;
  return std::strcmp(a.id, b.id) < 0;
}
