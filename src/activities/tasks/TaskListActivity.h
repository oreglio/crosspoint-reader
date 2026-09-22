#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "activities/tasks/TaskListModel.h"
#include "tasks/TaskRecord.h"

// Ecran principal des taches. Derive de UiListActivity (pas d'Activity nue) :
// selection, defilement, pagination et routage tactile viennent de la meme
// fabrique fui::ListNav / syncListViewport que la quarantaine d'ecrans de
// liste du depot (CONTEXT.md) — cet ecran ne reimplemente que ce qui lui est
// propre : quelles lignes montrer etant donne la bascule "terminees" et ce
// qui a ete coche pendant cette visite (TaskListModel.h).
//
// Charge l'index en onEnter(), le libere en onExit() : rien ne reste
// resident quand on lit un livre (jusqu'a 120 enregistrements de 216 octets,
// soit ~26 Ko).
//
// Priorite haute portee par ListItem::emphasis (gras, freeink-sdk) ; voir
// buildRows() dans le .cpp pour pourquoi c'est un booleen par ligne plutot
// qu'un style sur ListProps.
class TaskListActivity final : public UiListActivity {
 public:
  TaskListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Right (detail) et Left (bascule "terminees") : aucun des deux n'est
  // Back/Confirm/Haut/Bas, donc pas couvert par le contrat de base — voir le
  // rapport pour pourquoi ce sont les deux boutons avant qui restent une fois
  // Retour/Confirmer pris.
  bool handleCustomInput() override;
  // Remplace le pas Suivant/Precedent de base : celui-ci ne sait pas que la
  // ligne d'en-tete "terminees" n'est pas selectionnable (fui::ListItem
  // l'affiche non selectionnee, mais rien cote fui ne fait sauter l'index du
  // bouton par-dessus) — taskListStepSelection le fait.
  void navigateButtons() override;

 private:
  void rebuildOrder();
  void buildRows(UiScreen& screen);
  void toggleAt(int index);
  void openDetailAt(int index);
  void startSync();

  std::vector<TaskRecord> records;
  // Ids coches pendant cette visite : ils restent en place, attenues, pour
  // qu'un decochage apres erreur ne demande pas d'ouvrir la bascule.
  std::vector<std::string> tickedHere;
  std::vector<int> order;
  std::vector<TaskListRow> rows;
  int openCount = 0;
  int doneCount = 0;
  bool showDone = false;
  bool dirty = true;

  // Fenetre de ListItem materialisee pour la page visible seulement (comme
  // LibraryListActivity::winItems), pas un tableau de la taille totale.
  std::vector<freeink::ui::ListItem> winItems;
  // Stockage stable pour le libelle "N terminees" tant qu'il est reference
  // par un ListItem::label (un seul a la fois, ligne d'en-tete unique).
  std::string doneHeaderLabel;
};
