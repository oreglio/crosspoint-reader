#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/tasks/TaskListModel.h"
#include "tasks/TaskRecord.h"

// Ecran principal des taches. Charge l'index en onEnter(), le libere en
// onExit() : rien ne reste resident quand on lit un livre (jusqu'a 120
// enregistrements de 216 octets, soit ~26 Ko).
//
// Priorite portee uniquement par la graisse de police (gras/normal/italique -
// voir le .cpp pour pourquoi "italique" remplace le "light" des maquettes),
// jamais par un badge, un pip ou un filet entre les lignes : sur e-ink chaque
// filet est un bruit visuel permanent, la ou une difference de graisse
// survit au fantomage.
class TaskListActivity final : public Activity {
 public:
  TaskListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TaskList", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void toggleSelected();
  void openDetail();
  void startSync();
  // Retrie/reconstruit `order` et `rows` a partir de `records`, seulement si
  // `dirty` est vrai. Logique pure (TaskListModel) : aucun acces renderer ici.
  void rebuildOrder();
  // Mesure la hauteur (1 ou 2 lignes) de chaque ligne de `rows`, dependante du
  // renderer donc separee de rebuildOrder(). Appelee depuis render() quand
  // `rowHeights` a ete vide par un rebuildOrder() recent.
  void remeasureRowHeights(int lineHeight, int smallLineHeight, int maxTextWidth);

  std::vector<TaskRecord> records;
  // Ids coches pendant cette visite : ils restent en place, barres, pour
  // qu'un decochage apres erreur ne demande pas d'ouvrir la bascule.
  std::vector<std::string> tickedHere;
  std::vector<int> order;
  std::vector<TaskListRow> rows;
  // Hauteur en pixels de chaque ligne de `rows`, dans le meme ordre.
  std::vector<int> rowHeights;
  int openCount = 0;
  int doneCount = 0;
  int selected = 0;
  int scrollTop = 0;
  bool showDone = false;
  bool dirty = true;
};
