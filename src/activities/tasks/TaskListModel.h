#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tasks/TaskRecord.h"

// Logique pure de CONTENU pour l'ecran de liste des taches : quelles lignes
// l'ecran montre, dans quel ordre, etant donne l'etat de la bascule
// "terminees" et ce qui a ete coche pendant cette visite.
//
// Ce fichier ne fait PAS de geometrie : ni hauteur de ligne, ni defilement, ni
// pagination. UiListActivity (via fui::ListNav / syncListViewport / list())
// possede deja tout cela, et le dupliquer ici serait une deuxieme
// implementation a maintenir en plus des ~40 ecrans qui partagent la
// premiere. Extrait pour rester testable a l'hote sans renderer ni TaskStore.

// Une ligne d'ecran est soit une tache, soit l'en-tete non selectionnable de
// la section "terminees" (repliee par defaut).
enum class TaskRowKind : uint8_t { Task, DoneHeader };

struct TaskListRow {
  TaskRowKind kind = TaskRowKind::Task;
  // Indice dans `records` (le tableau passe a buildTaskOrder) ; non
  // significatif quand kind == DoneHeader.
  int recordIndex = -1;
};

// Trie les indices de `records` avec le comparateur canonique taskOrderBefore
// (Tache 1) — jamais re-derive ici — sauf que les taches dont l'id figure
// dans `tickedHere` sont comparees comme si `.done` valait encore faux : les
// cocher pendant cette visite ne doit pas les faire sauter au bas de l'ecran,
// elles restent a leur place et se contentent d'un rendu attenue (voir le
// .cpp de l'activite). `openCount` recoit le nombre de taches en tete de
// `order` qui sont ouvertes au sens ci-dessus.
void buildTaskOrder(const std::vector<TaskRecord>& records, const std::vector<std::string>& tickedHere,
                    std::vector<int>& order, int& openCount);

// Aplati `order` en lignes d'ecran : les `openCount` premieres taches, puis -
// seulement si showDone - une ligne d'en-tete suivie du reste (deja trie par
// priorite/id, puisque buildTaskOrder les laisse a la fin du tableau).
void buildTaskListRows(const std::vector<int>& order, int openCount, bool showDone, std::vector<TaskListRow>& rows);

// Avance `index` d'un cran (`direction` = +1 ou -1) dans `rows` sans jamais
// s'arreter sur une ligne d'en-tete, en bouclant d'un bout a l'autre de la
// liste. Rend -1 si `rows` est vide. Sert a faire sauter Haut/Bas par-dessus
// la ligne "terminees", que fui::ListItem::isHeader rend non selectionnable a
// l'affichage mais que rien cote fui ne fait sauter dans l'arithmetique
// d'indices du bouton — c'est cette arithmetique-la, pas une geometrie.
int taskListStepSelection(const std::vector<TaskListRow>& rows, int index, int direction);

// Ramene `selected` sur une ligne Tache valide apres une reconstruction de
// `rows` (bornes + saut par-dessus une en-tete). Rend -1 si `rows` est vide.
int taskListNormalizeSelection(const std::vector<TaskListRow>& rows, int selected);

// Saut de page (appui long sur Haut/Bas) : avance de `pageRows` lignes
// (`direction` = +1 ou -1), puis ramene le resultat sur une ligne Tache via
// taskListNormalizeSelection — un saut de page peut atterrir pile sur
// l'en-tete "terminees", que rien cote ButtonNavigator ne sait eviter.
// L'arithmetique elle-meme vient de util/PageIndex.h, le meme header dont
// derivent ButtonNavigator::nextPageIndex/previousPageIndex : une seule
// implementation pour les ~40 ecrans de liste et pour celui-ci, et elle reste
// compilable a l'hote (PageIndex.h n'inclut ni Arduino ni le HAL, contrairement
// a util/ButtonNavigator.h -> MappedInputManager.h -> HalGPIO.h -> Arduino.h).
// Rend -1 si `rows` est vide.
int taskListPageJump(const std::vector<TaskListRow>& rows, int selected, int pageRows, int direction);
