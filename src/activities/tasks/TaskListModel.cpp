#include "TaskListModel.h"

#include <algorithm>

#include "util/PageIndex.h"

namespace {

bool isTickedHere(const std::vector<std::string>& tickedHere, const char* id) {
  for (const auto& t : tickedHere) {
    if (t == id) return true;
  }
  return false;
}

}  // namespace

void buildTaskOrder(const std::vector<TaskRecord>& records, const std::vector<std::string>& tickedHere,
                    std::vector<int>& order, int& openCount) {
  order.clear();
  order.reserve(records.size());
  for (int i = 0; i < static_cast<int>(records.size()); ++i) order.push_back(i);

  // "Fait" au sens du tri : vrai `.done`, sauf pour une tache cochee PENDANT
  // cette visite, qui reste triee comme ouverte (elle reste en place,
  // attenuee, jusqu'a la sortie de l'ecran). Precalcule une fois en O(n) --
  // isTickedHere fait un parcours lineaire avec comparaison de string, donc
  // l'appeler O(n log n) fois depuis le comparateur referait le meme travail
  // en pire, sans compter qu'un comparateur qui copierait un TaskRecord
  // (216 octets) par argument couterait ~432 octets de pile transitoires par
  // comparaison. Le comparateur canonique (Tache 1) prend les deux drapeaux
  // en parametres justement pour eviter cette copie.
  std::vector<uint8_t> effectiveDone(records.size());
  for (size_t i = 0; i < records.size(); ++i) {
    effectiveDone[i] = (records[i].done && !isTickedHere(tickedHere, records[i].id)) ? 1 : 0;
  }

  // std::sort et non std::stable_sort : taskOrderBefore est un ordre TOTAL
  // (le dernier depart est strcmp sur des ids uniques), donc la stabilite
  // n'ajoute aucune garantie -- et std::stable_sort demande un tampon
  // temporaire (~480 octets pour 120 int), une allocation evitable sur un C3
  // sans PSRAM dans un chemin rejoue a chaque coche.
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    return taskOrderBefore(records[a], records[b], effectiveDone[a] != 0, effectiveDone[b] != 0);
  });

  openCount = 0;
  while (openCount < static_cast<int>(order.size()) && effectiveDone[order[openCount]] == 0) {
    ++openCount;
  }
}

void buildTaskListRows(const std::vector<int>& order, int openCount, bool showDone, std::vector<TaskListRow>& rows) {
  rows.clear();
  const int total = static_cast<int>(order.size());
  if (openCount < 0) openCount = 0;
  if (openCount > total) openCount = total;
  rows.reserve(showDone ? total + 1 : openCount);

  for (int i = 0; i < openCount; ++i) rows.push_back({TaskRowKind::Task, order[i]});

  if (showDone && openCount < total) {
    rows.push_back({TaskRowKind::DoneHeader, -1});
    for (int i = openCount; i < total; ++i) rows.push_back({TaskRowKind::Task, order[i]});
  }
}

int taskListStepSelection(const std::vector<TaskListRow>& rows, int index, int direction) {
  const int total = static_cast<int>(rows.size());
  if (total == 0) return -1;
  int next = index;
  for (int step = 0; step < total; ++step) {
    next = (next + direction + total) % total;
    if (rows[next].kind == TaskRowKind::Task) return next;
  }
  // Rien de selectionnable (que des en-tetes) : ne devrait jamais arriver
  // puisqu'une en-tete n'existe que quand il y a au moins une tache derriere.
  return index;
}

int taskListNormalizeSelection(const std::vector<TaskListRow>& rows, int selected) {
  const int total = static_cast<int>(rows.size());
  if (total == 0) return -1;
  if (selected < 0) selected = 0;
  if (selected >= total) selected = total - 1;
  if (rows[selected].kind == TaskRowKind::Task) return selected;
  return taskListStepSelection(rows, selected, +1);
}

int taskListPageJump(const std::vector<TaskListRow>& rows, int selected, int pageRows, int direction) {
  const int total = static_cast<int>(rows.size());
  if (total == 0) return -1;
  if (pageRows <= 0) pageRows = 1;
  if (selected < 0) selected = 0;
  if (selected >= total) selected = total - 1;

  // Meme arithmetique que ButtonNavigator::nextPageIndex/previousPageIndex :
  // les deux delegent a util/PageIndex.h, qui n'inclut ni Arduino ni le HAL et
  // reste donc compilable a l'hote. Les gardes degenerees restent ici (voir le
  // contrat de PageIndex.h) : cet appelant rend -1 sur une liste vide, pas 0.
  const int jumped =
      direction > 0 ? nextPageIndexPure(selected, total, pageRows) : previousPageIndexPure(selected, total, pageRows);
  // Un saut de page ignore tout des lignes non selectionnables et peut
  // atterrir pile sur l'en-tete "terminees" : cette correction est ce qui
  // rend la propriete testable a l'hote (voir TaskListModelTest.cpp).
  return taskListNormalizeSelection(rows, jumped);
}
