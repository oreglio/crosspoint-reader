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

// Une ligne d'ecran est une tache, la ligne "N faites" qui deplie ou replie la
// section terminees, ou la ligne "+ Ajouter une tache" d'un index vide. Les
// trois sont selectionnables : aucune n'est un fui::ListItem::isHeader, que le
// SDK dessine non selectionnable et sans action (list.h:442). Il n'y a donc
// plus aucune ligne a sauter, et la navigation de base (ButtonNavigator) sert
// telle quelle.
enum class TaskRowKind : uint8_t { Task, DoneSection, AddTask };

struct TaskListRow {
  TaskRowKind kind = TaskRowKind::Task;
  // Indice dans `records` (le tableau passe a buildTaskOrder) ; non
  // significatif quand kind != Task.
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

// Aplati `order` en lignes d'ecran :
//  - index vide : une seule ligne AddTask, rien d'autre ;
//  - sinon les `openCount` premieres taches, puis - des qu'il reste au moins
//    une tache terminee, section repliee OU depliee - la ligne DoneSection,
//    suivie du reste (deja trie par priorite/id) seulement si showDone.
// La ligne 0 est donc la premiere tache ouverte s'il y en a une, la ligne
// AddTask si l'index est vide, et la ligne DoneSection si tout est fait :
// c'est la selection par defaut, puisque UiListActivity::onEnter() remet la
// selection a 0.
void buildTaskListRows(const std::vector<int>& order, int openCount, bool showDone, std::vector<TaskListRow>& rows);

// Ramene `selected` dans les bornes de `rows` apres une reconstruction (une
// coche ou un repli peut raccourcir la liste). Aucune ligne n'est sautee,
// quelle que soit sa nature. Rend -1 si `rows` est vide.
int taskListClampSelection(const std::vector<TaskListRow>& rows, int selected);
