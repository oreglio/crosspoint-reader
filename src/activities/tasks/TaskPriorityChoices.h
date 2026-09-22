#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>

#include "tasks/TaskRecord.h"

// La correspondance priorite -> libelle traduit, et l'ordre dans lequel les
// trois priorites sont proposees. Partage par les deux ecrans qui ouvrent le
// selecteur : TaskListActivity (creation) et TaskDetailActivity (modification).
//
// Un en-tete a part plutot qu'une copie dans chaque .cpp : la table est courte
// aujourd'hui, ce qui est exactement le moment ou une duplication parait
// inoffensive. Les deux fichiers sont edites par des taches differentes, et
// deux copies qui divergent se verraient comme un ecran affichant "Haute" quand
// l'autre affiche autre chose -- sans qu'aucun test ni aucune compilation
// n'echoue.
//
// Il ne vit PAS dans TaskListModel.h, qui n'inclut volontairement ni I18n ni
// renderer pour rester compilable dans la suite de tests hote.

inline StrId taskPriorityLabelId(const uint8_t priority) {
  if (priority == TASK_PRIORITY_HIGH) return StrId::STR_TASK_PRIORITY_HIGH;
  if (priority == TASK_PRIORITY_LOW) return StrId::STR_TASK_PRIORITY_LOW;
  return StrId::STR_TASK_PRIORITY_NORMAL;
}

// Les trois valeurs de TaskRecord::priority, dans l'ordre ou elles sont
// proposees — l'indice rendu par OptionSelectionActivity est un rang dans
// cette table, pas une priorite.
inline constexpr uint8_t TASK_PRIORITY_CHOICES[] = {TASK_PRIORITY_HIGH, TASK_PRIORITY_NORMAL, TASK_PRIORITY_LOW};
inline constexpr size_t TASK_PRIORITY_CHOICE_COUNT = sizeof(TASK_PRIORITY_CHOICES) / sizeof(TASK_PRIORITY_CHOICES[0]);

// Rang preselectionne pour une tache neuve : personne ne s'est encore prononce,
// donc "normale". Le static_assert garde ce rang et la table synchronises.
inline constexpr uint8_t TASK_DEFAULT_PRIORITY_CHOICE = 1;
static_assert(TASK_PRIORITY_CHOICES[TASK_DEFAULT_PRIORITY_CHOICE] == TASK_PRIORITY_NORMAL,
              "TASK_DEFAULT_PRIORITY_CHOICE doit designer TASK_PRIORITY_NORMAL dans TASK_PRIORITY_CHOICES");
