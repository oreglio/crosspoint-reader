#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "activities/tasks/TaskListModel.h"
#include "tasks/TaskRecord.h"

// Le choix du groupe d'une tache, partage par les deux ecrans qui l'ouvrent :
// le menu de la liste (TaskListActivity::chooseGroup) et la troisieme etape de
// « modifier » sur le detail (TaskDetailActivity::editGroup). Meme raison
// d'etre que TaskPriorityChoices.h : deux copies qui divergent proposeraient
// les groupes dans deux ordres differents sans qu'aucun test n'echoue.
//
// Rang 0 = « Sans groupe », puis les groupes dans l'ordre de leurs titres a
// l'ecran (taskGroupDisplayOrder). `order` garde les indices de
// TASK_STORE.groups() : rien ne modifie la liste des groupes pendant que le
// selecteur est ouvert (seule la sync en change, et elle redemarre l'appareil).
struct TaskGroupChoices {
  uint8_t order[MAX_GROUPS] = {};
  size_t count = 0;
};

// Remplit les libelles du selecteur et rend le rang du groupe actuel de la
// tache (`current`, vide = sans groupe).
inline uint8_t buildTaskGroupChoices(const std::vector<TaskGroup>& groups, const char* current,
                                     TaskGroupChoices& choices, std::vector<std::string>& labels) {
  choices.count = taskGroupDisplayOrder(groups, choices.order);
  labels.clear();
  labels.reserve(choices.count + 1);
  labels.emplace_back(tr(STR_TASK_NO_GROUP));
  uint8_t selected = 0;
  for (size_t i = 0; i < choices.count; ++i) {
    const TaskGroup& g = groups[choices.order[i]];
    labels.emplace_back(g.name);
    if (current != nullptr && std::strcmp(g.id, current) == 0) selected = static_cast<uint8_t>(i + 1);
  }
  return selected;
}

// Id du groupe choisi au rang `index` : "" pour « Sans groupe », nullptr pour
// un rang hors du selecteur.
inline const char* taskGroupChoiceId(const std::vector<TaskGroup>& groups, const TaskGroupChoices& choices,
                                     const int index) {
  if (index == 0) return "";
  if (index < 0 || static_cast<size_t>(index) > choices.count) return nullptr;
  const size_t g = choices.order[static_cast<size_t>(index) - 1];
  return g < groups.size() ? groups[g].id : nullptr;
}
