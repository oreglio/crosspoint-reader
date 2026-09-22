#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tasks/TaskRecord.h"

// Logique pure de l'ecran de liste des taches : tri, decoupage en lignes
// d'ecran et defilement. Aucune fonction ici ne touche au renderer ni au
// TaskStore, pour que les regles de tri et de pagination restent testables
// sur l'hote, comme CountdownLayout et ReaderDrawerModel.

// Une ligne d'ecran est soit une tache, soit l'en-tete non selectionnable de
// la section "terminees" (repliee par defaut).
enum class TaskRowKind : uint8_t { Task, DoneHeader };

struct TaskListRow {
  TaskRowKind kind = TaskRowKind::Task;
  // Indice dans `records` (le tableau passe a buildTaskOrder) ; non
  // significatif quand kind == DoneHeader.
  int recordIndex = -1;
};

// Trie les indices de `records` selon taskOrderBefore (ouvertes d'abord, puis
// priorite croissante, puis id), sauf que les taches dont l'id figure dans
// `tickedHere` sont traitees comme encore ouvertes pour le tri : les cocher
// pendant cette visite ne doit pas les faire sauter au bas de l'ecran, elles
// restent a leur place et se contentent d'un texte barre. `openCount` recoit
// le nombre de taches en tete de `order` qui sont ouvertes au sens ci-dessus.
void buildTaskOrder(const std::vector<TaskRecord>& records, const std::vector<std::string>& tickedHere,
                    std::vector<int>& order, int& openCount);

// Aplati `order` en lignes d'ecran : les `openCount` premieres taches, puis -
// seulement si showDone - une ligne d'en-tete suivie du reste (deja trie par
// priorite/id, puisque buildTaskOrder les laisse a la fin du tableau).
void buildTaskListRows(const std::vector<int>& order, int openCount, bool showDone, std::vector<TaskListRow>& rows);

// Avance `index` d'un cran (`direction` = +1 ou -1) dans `rows` sans jamais
// s'arreter sur une ligne d'en-tete, en bouclant d'un bout a l'autre de la
// liste. Rend -1 si `rows` est vide.
int taskListStepSelection(const std::vector<TaskListRow>& rows, int index, int direction);

// Ramene `selected` sur une ligne Tache valide apres une reconstruction de
// `rows` (bornes + saut par-dessus une en-tete). Rend -1 si `rows` est vide.
int taskListNormalizeSelection(const std::vector<TaskListRow>& rows, int selected);

// Combien de lignes a partir de `scrollTop` tiennent dans `contentHeight`,
// d'apres leurs hauteurs mesurees `heights` (rembourrage deja inclus par
// l'appelant). Toujours au moins 1 des que `scrollTop` designe une ligne
// valide, meme si cette ligne seule depasse `contentHeight` : une ligne
// partiellement visible vaut mieux qu'un ecran vide.
int taskListVisibleRows(const std::vector<int>& heights, int scrollTop, int contentHeight);

// Ramene `scrollTop` de sorte que la ligne `selected` soit entierement visible
// dans `contentHeight`, en la deplacant le moins possible (defilement en
// escalier, pas un recentrage systematique).
int taskListClampScrollTop(const std::vector<int>& heights, int selected, int scrollTop, int contentHeight);
