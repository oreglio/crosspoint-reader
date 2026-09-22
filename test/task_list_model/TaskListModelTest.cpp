#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "TaskListModel.h"

namespace {

TaskRecord make(const char* id, const char* title, uint8_t priority, bool done = false) {
  TaskRecord r{};
  std::snprintf(r.id, sizeof(r.id), "%s", id);
  std::snprintf(r.title, sizeof(r.title), "%s", title);
  r.priority = priority;
  r.done = done;
  return r;
}

}  // namespace

// --- buildTaskOrder ---------------------------------------------------------
// Reuses taskOrderBefore (Task 1); these tests pin the ADDITIONAL rule this
// file adds on top of it (tickedHere), plus a couple of sanity checks that
// the canonical comparator is genuinely being called and not re-derived.

TEST(BuildTaskOrder, OpenFirstByPriorityThenId) {
  std::vector<TaskRecord> records = {
      make("w3", "basse", 2),
      make("w1", "haute", 0),
      make("w2", "normale", 1),
  };
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, {}, order, openCount);

  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(openCount, 3);
  EXPECT_EQ(order[0], 1);  // w1, haute
  EXPECT_EQ(order[1], 2);  // w2, normale
  EXPECT_EQ(order[2], 0);  // w3, basse
}

TEST(BuildTaskOrder, DoneTasksSinkAfterOpenOnesAndOpenCountExcludesThem) {
  std::vector<TaskRecord> records = {
      make("w1", "faite haute", 0, true),
      make("w2", "ouverte basse", 2),
  };
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, {}, order, openCount);

  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(openCount, 1);
  EXPECT_EQ(order[0], 1);  // w2 ouverte, malgre sa priorite plus basse
  EXPECT_EQ(order[1], 0);  // w1 faite, en fin de liste
}

TEST(BuildTaskOrder, TickedHereStaysInPlaceInsteadOfSinking) {
  // w1 est coche PENDANT cette visite : il doit rester tri comme une tache
  // ouverte de priorite haute, pas tomber apres w2.
  std::vector<TaskRecord> records = {
      make("w1", "cochee a l'instant", 0, true),
      make("w2", "ouverte normale", 1),
  };
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, {"w1"}, order, openCount);

  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(openCount, 2);  // les deux comptent comme ouvertes pour l'affichage
  EXPECT_EQ(order[0], 0);   // w1 garde sa place de priorite haute
  EXPECT_EQ(order[1], 1);
}

TEST(BuildTaskOrder, UntickingSomethingTickedThisVisitLetsItSinkAgain) {
  // La TRANSITION, pas seulement son etat d'arrivee : les MEMES records sont
  // tries deux fois, d'abord avec w1 dans tickedHere (il garde sa place de
  // priorite haute), puis sans (il retombe derriere w2). Un test qui n'aurait
  // que le second appel passerait meme si tout le mecanisme tickedHere etait
  // supprime -- c'est exactement le defaut qu'il avait.
  std::vector<TaskRecord> records = {
      make("w1", "cochee puis decochee dans la meme visite", 0, true),
      make("w2", "ouverte normale", 1),
  };
  std::vector<int> order;
  int openCount = 0;

  buildTaskOrder(records, /*tickedHere=*/{"w1"}, order, openCount);
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(openCount, 2);  // cochee ici : encore comptee comme ouverte
  EXPECT_EQ(order[0], 0);   // w1 n'a pas bouge
  EXPECT_EQ(order[1], 1);

  buildTaskOrder(records, /*tickedHere=*/{}, order, openCount);
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(openCount, 1);  // decochee du registre de visite : elle coule
  EXPECT_EQ(order[0], 1);   // w2 ouverte d'abord
  EXPECT_EQ(order[1], 0);   // w1 faite, en fin de liste
}

TEST(BuildTaskOrder, EqualPriorityFallsBackToId) {
  std::vector<TaskRecord> records = {make("w2", "b", 1), make("w1", "a", 1)};
  std::vector<int> order;
  int openCount = 0;
  buildTaskOrder(records, {}, order, openCount);
  EXPECT_EQ(order[0], 1);  // w1 < w2
  EXPECT_EQ(order[1], 0);
}

// --- buildTaskListRows -------------------------------------------------------
// The "N faites" row is an accordion: it must be present whenever something is
// done, collapsed or expanded, and the add row exists only for an empty index.
// Each test below was checked by mutation on a scratch copy outside the git
// tree: it fails when the rule it names is broken, and passes again once
// restored.

TEST(BuildTaskListRows, CollapsedStillShowsTheDoneRowButNotTheDoneTasks) {
  // Avant la refonte, la ligne n'existait qu'une fois la section ouverte :
  // replie, rien ne permettait de l'ouvrir au doigt ni a Confirmer.
  std::vector<int> order = {0, 1, 2};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/2, /*showDone=*/false, rows);

  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[0].recordIndex, 0);
  EXPECT_EQ(rows[1].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[1].recordIndex, 1);
  EXPECT_EQ(rows[2].kind, TaskRowKind::DoneSection);
}

TEST(BuildTaskListRows, ExpandedPutsTheDoneRowBeforeTheDoneTail) {
  std::vector<int> order = {0, 1, 2, 3};
  std::vector<TaskListRow> rows;
  buildTaskListRows(order, /*openCount=*/2, /*showDone=*/true, rows);

  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[1].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[2].kind, TaskRowKind::DoneSection);
  EXPECT_EQ(rows[3].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[3].recordIndex, 2);
  EXPECT_EQ(rows[4].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[4].recordIndex, 3);
}

TEST(BuildTaskListRows, NoDoneRowWhenNothingIsDoneInEitherState) {
  std::vector<int> order = {0, 1};
  std::vector<TaskListRow> rows;
  for (const bool showDone : {false, true}) {
    buildTaskListRows(order, /*openCount=*/2, showDone, rows);
    ASSERT_EQ(rows.size(), 2u) << "showDone=" << showDone;
    for (const auto& row : rows) EXPECT_EQ(row.kind, TaskRowKind::Task) << "showDone=" << showDone;
  }
}

TEST(BuildTaskListRows, UntickingTheLastDoneTaskDropsTheCollapsedDoneRow) {
  // La TRANSITION, section repliee : openCount passe de 1 a 2 parce que la
  // derniere tache terminee vient d'etre decochee (depuis le detail). La ligne
  // est la au premier appel et doit avoir disparu au second, sans rester
  // orpheline sur un compte "0 faites". Deux appels : sans le premier, le test
  // ne serait qu'un doublon de NoDoneRowWhenNothingIsDoneInEitherState.
  std::vector<int> order = {0, 1};
  std::vector<TaskListRow> rows;

  buildTaskListRows(order, /*openCount=*/1, /*showDone=*/false, rows);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[1].kind, TaskRowKind::DoneSection);

  buildTaskListRows(order, /*openCount=*/2, /*showDone=*/false, rows);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[1].kind, TaskRowKind::Task);
}

TEST(BuildTaskListRows, EmptyIndexYieldsExactlyTheAddRow) {
  std::vector<int> order;
  std::vector<TaskListRow> rows;
  for (const bool showDone : {false, true}) {
    buildTaskListRows(order, /*openCount=*/0, showDone, rows);
    ASSERT_EQ(rows.size(), 1u) << "showDone=" << showDone;
    EXPECT_EQ(rows[0].kind, TaskRowKind::AddTask) << "showDone=" << showDone;
  }
}

TEST(BuildTaskListRows, NoAddRowOnceTheIndexHoldsAnything) {
  // "Tout est fait" n'est PAS un index vide : aucune tache ouverte, mais la
  // ligne "N faites" est la et l'ajout passe par le bouton Gauche. Seul un
  // index reellement vide porte la ligne d'ajout — choix explicite de
  // l'utilisateur contre une ligne toujours presente.
  std::vector<TaskListRow> rows;

  buildTaskListRows(/*order=*/{0, 1}, /*openCount=*/0, /*showDone=*/false, rows);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].kind, TaskRowKind::DoneSection);

  buildTaskListRows(/*order=*/{0, 1}, /*openCount=*/0, /*showDone=*/true, rows);
  ASSERT_EQ(rows.size(), 3u);
  for (const auto& row : rows) EXPECT_NE(row.kind, TaskRowKind::AddTask);

  buildTaskListRows(/*order=*/{0, 1}, /*openCount=*/1, /*showDone=*/false, rows);
  for (const auto& row : rows) EXPECT_NE(row.kind, TaskRowKind::AddTask);
}

TEST(BuildTaskListRows, RowZeroIsTheDefaultSelectionForEveryState) {
  // UiListActivity::onEnter() remet la selection a 0 et l'ecran s'ouvre
  // replie : la ligne 0 EST la selection par defaut. Elle doit etre la
  // premiere tache quand il y en a une d'ouverte, la ligne d'ajout sur un
  // index vide - jamais la ligne "N faites" devant des taches ouvertes.
  std::vector<TaskListRow> rows;

  buildTaskListRows(/*order=*/{4, 2, 7}, /*openCount=*/1, /*showDone=*/false, rows);
  ASSERT_FALSE(rows.empty());
  EXPECT_EQ(rows[0].kind, TaskRowKind::Task);
  EXPECT_EQ(rows[0].recordIndex, 4);

  buildTaskListRows(/*order=*/{}, /*openCount=*/0, /*showDone=*/false, rows);
  ASSERT_FALSE(rows.empty());
  EXPECT_EQ(rows[0].kind, TaskRowKind::AddTask);
}

// --- taskListClampSelection --------------------------------------------------
// Every row is selectable now, so the only job left after a rebuild is to keep
// the index in bounds. The old normalizer also stepped off the header; these
// tests pin that it no longer steps off anything.

TEST(TaskListClampSelection, ClampsOutOfRangeIndex) {
  std::vector<TaskListRow> rows = {{TaskRowKind::Task, 0}, {TaskRowKind::Task, 1}};
  EXPECT_EQ(taskListClampSelection(rows, 99), 1);
  EXPECT_EQ(taskListClampSelection(rows, -5), 0);
  // La frontiere qui compte vraiment : une liste qui raccourcit d'une ligne
  // sous une selection posee sur sa derniere ligne laisse selected == size().
  EXPECT_EQ(taskListClampSelection(rows, 2), 1);

  std::vector<TaskListRow> addOnly = {{TaskRowKind::AddTask, -1}};
  EXPECT_EQ(taskListClampSelection(addOnly, 1), 0);
}

TEST(TaskListClampSelection, StaysOnTheDoneRow) {
  // Confirmer sur "N faites" deplie la section puis reconstruit : la selection
  // doit rester sur la ligne qu'on vient d'activer, pas glisser sur la
  // premiere tache terminee.
  std::vector<TaskListRow> rows = {
      {TaskRowKind::Task, 0},
      {TaskRowKind::DoneSection, -1},
      {TaskRowKind::Task, 1},
  };
  EXPECT_EQ(taskListClampSelection(rows, 1), 1);
}

TEST(TaskListClampSelection, AStaleIndexPastTheEndLandsOnTheLastRowWhateverItsKind) {
  // Garde de bornes pure, sans scenario d'ecran derriere : un indice perime
  // au-dela de la fin atterrit sur la derniere ligne telle qu'elle est — ici
  // "N faites" ou la ligne d'ajout — sans chercher une tache. (Le repli de la
  // section ne produit pas ce cas : il ne s'execute que selection sur
  // "N faites", dont l'indice ne bouge pas.)
  std::vector<TaskListRow> rows = {{TaskRowKind::Task, 0}, {TaskRowKind::DoneSection, -1}};
  EXPECT_EQ(taskListClampSelection(rows, 4), 1);

  std::vector<TaskListRow> addOnly = {{TaskRowKind::AddTask, -1}};
  EXPECT_EQ(taskListClampSelection(addOnly, 3), 0);
}

TEST(TaskListClampSelection, EmptyRowsReturnsNegativeOne) {
  std::vector<TaskListRow> rows;
  EXPECT_EQ(taskListClampSelection(rows, 0), -1);
}
