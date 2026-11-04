#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "tasks/TaskRecord.h"

// Logique pure de CONTENU pour l'ecran de liste des taches : quelles lignes
// l'ecran montre, dans quel ordre, etant donne la vue (ouvertes ou faites) et
// les groupes.
//
// Ce fichier ne fait PAS de geometrie : ni hauteur de ligne, ni defilement, ni
// pagination. UiListActivity (via fui::ListNav / syncListViewport / list())
// possede deja tout cela, et le dupliquer ici serait une deuxieme
// implementation a maintenir en plus des ~40 ecrans qui partagent la
// premiere. Extrait pour rester testable a l'hote sans renderer ni TaskStore.

// Une ligne d'ecran est une tache ou la ligne "+ Ajouter une tache" d'un
// index vide, ou un titre de groupe (GroupHeader). Toutes sont selectionnables :
// aucune n'est un fui::ListItem::isHeader, que le SDK dessine non selectionnable
// et sans action (list.h:442). La navigation de base (ButtonNavigator) sert donc
// telle quelle, sauf en mode selection, qui saute les titres (ils portent les
// actions du groupe, pas une tache : taskListStepSkippingHeaders).
enum class TaskRowKind : uint8_t { Task, AddTask, GroupHeader };

// Vue exclusive de l'ecran : les taches ouvertes OU les taches faites, jamais
// les deux dans la meme liste.
enum class TaskListView : uint8_t { Open, Done };

struct TaskListRow {
  TaskRowKind kind = TaskRowKind::Task;
  // Indice dans `groups` pour un GroupHeader ; -1 sinon. MAX_GROUPS = 16 tient
  // dans un int8_t.
  int8_t groupIndex = -1;
  // Indice dans `records` (le tableau passe a buildTaskOrder) ; non
  // significatif quand kind != Task. MAX_TASKS = 120 tient dans un int16_t.
  int16_t recordIndex = -1;
};
// 4 octets par ligne : `rows` pese (120 + 16) x 4 au pire, moins que les
// 121 x 8 d'avant les groupes.
static_assert(sizeof(TaskListRow) == 4, "TaskListRow must stay packed to 4 bytes");

// Trie les indices de `records`. Les ouvertes d'abord : rang de groupe (sans
// groupe ou groupe inconnu = avant tout, puis `position` du groupe, puis son
// id), puis le comparateur canonique taskOrderBefore (Tache 1) — jamais
// re-derive ici. Les faites suivent, triees par taskOrderBefore seul : le
// groupe est ignore. Une tache cochee quitte la vue ouverte pour la vue des faites.
// `openCount` recoit le nombre de taches ouvertes en tete de `order`.
void buildTaskOrder(const std::vector<TaskRecord>& records, const std::vector<TaskGroup>& groups,
                    std::vector<int>& order, int& openCount);

// Aplati `order` en lignes d'ecran selon la vue :
//  - Open : les taches sans groupe (sans titre), puis, pour chaque groupe ayant
//    au moins une tache ouverte, un GroupHeader suivi de ses taches ; une seule
//    ligne AddTask s'il n'y a aucune tache ouverte ;
//  - Done : le reste de `order` (deja trie par priorite/id), sans titre, vide
//    s'il n'y a aucune tache faite ; la ligne AddTask n'apparait que si l'index
//    entier est vide.
// La ligne 0 est la selection par defaut (UiListActivity::onEnter() remet la
// selection a 0) : la premiere ligne de la vue.
void buildTaskListRows(const std::vector<int>& order, int openCount, TaskListView view,
                       const std::vector<TaskRecord>& records, const std::vector<TaskGroup>& groups,
                       std::vector<TaskListRow>& rows);

// Indice du GroupHeader precedent (direction -1) ou suivant (+1) depuis
// `selected`, -1 s'il n'y en a pas (pas de bouclage).
int taskListGroupJump(const std::vector<TaskListRow>& rows, int selected, int direction);

// Pas de navigation du mode selection : ligne suivante (+1) ou precedente (-1)
// qui n'est pas un GroupHeader, avec bouclage comme ButtonNavigator. -1 si
// aucune ligne n'est un candidat. `selected` hors liste (-1) part d'avant la
// premiere ligne (+1) ou d'apres la derniere (-1).
int taskListStepSkippingHeaders(const std::vector<TaskListRow>& rows, int selected, int direction);

// Indices de `groups` dans l'ordre d'affichage (position, puis id), le meme
// que celui des titres de la liste. `out` doit tenir MAX_GROUPS entrees ; rend
// le nombre ecrit (au plus MAX_GROUPS).
size_t taskGroupDisplayOrder(const std::vector<TaskGroup>& groups, uint8_t* out);

// Taches qui suivent le titre `headerRow` jusqu'au prochain titre.
int taskGroupOpenCount(const std::vector<TaskListRow>& rows, int headerRow);

// Ramene `selected` dans les bornes de `rows` apres une reconstruction (une
// coche ou un repli peut raccourcir la liste). Aucune ligne n'est sautee,
// quelle que soit sa nature. Rend -1 si `rows` est vide.
int taskListClampSelection(const std::vector<TaskListRow>& rows, int selected);

// Curseur `next` (deja dans les bornes) apres qu'une coche a retire une tache
// de la vue : s'il tombe sur un GroupHeader, passe a la ligne suivante (un titre
// est toujours suivi d'une de ses taches). Sans suivante, ou hors titre, rend
// `next` tel quel.
int taskListCursorAfterRemoval(const std::vector<TaskListRow>& rows, int next);

// Mode selection de la liste : un bit par enregistrement de `records`, indexe
// comme TaskListRow::recordIndex. 16 octets en membre, sans tas. Valable tant
// que l'index ne change pas : le mode n'ecrit rien avant l'action finale, qui
// le quitte.
using TaskSelection = std::bitset<MAX_TASKS>;

// Cibles d'une action en lot, en indices de `records`, du plus GRAND au plus
// petit : effacer dans cet ordre ne decale aucun indice qui reste a traiter.
// `out` doit tenir MAX_TASKS entrees ; rend le nombre ecrit. Les bits au-dela
// de `recordCount` sont ignores.
size_t taskSelectionTargets(const TaskSelection& selection, size_t recordCount, uint8_t* out);
// Les taches faites, dans le meme ordre et au meme contrat.
size_t taskDoneTargets(const std::vector<TaskRecord>& records, uint8_t* out);
