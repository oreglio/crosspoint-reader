#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "activities/tasks/TaskListModel.h"
#include "tasks/TaskRecord.h"

// Ecran principal des taches. Derive de UiListActivity (pas d'Activity nue) :
// selection, defilement, pagination, routage tactile ET le squelette de rendu
// (clearScreen / chrome / renderUi / boucle de reconstruction / hints /
// displayBuffer) viennent de la meme fabrique que la quarantaine d'ecrans de
// liste du depot (CONTEXT.md) — cet ecran ne reimplemente que ce qui lui est
// propre : quelles lignes montrer etant donne la bascule "terminees" et ce
// qui a ete coche pendant cette visite (TaskListModel.h), plus drawChrome() /
// drawFooter() pour sa sous-ligne de fraicheur et ses libelles de boutons.
//
// Ne detient AUCUNE copie de l'index : `order` et TaskListRow::recordIndex
// indexent directement TASK_STORE.all(). Une copie membre doublait le pic a
// ~52 Ko (2 x 120 x 216 octets) pendant la visite. onExit() appelle
// TaskStore::unload(), donc apres la sortie de l'ecran l'index ne reste pas
// resident non plus — ce que le chargement de main.cpp laissait sinon en place
// pour toute la session, pendant la lecture d'un livre comprise.
//
// Priorite haute portee par ListItem::emphasis (gras, freeink-sdk) ; voir
// buildRows() dans le .cpp pour pourquoi c'est un booleen par ligne plutot
// qu'un style sur ListProps.
class TaskListActivity final : public UiListActivity {
 public:
  TaskListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Right (detail) et Left (bascule "terminees") : aucun des deux n'est
  // Back/Confirm/Haut/Bas, donc pas couvert par le contrat de base — voir le
  // rapport pour pourquoi ce sont les deux boutons avant qui restent une fois
  // Retour/Confirmer pris.
  bool handleCustomInput() override;
  // Remplace entierement le Back/Confirmer de la base. UiListActivity active la
  // ligne sur le FRONT D'APPUI de Confirmer (UiListActivity.cpp:50-54), ce qui
  // rend un appui long indiscernable d'un appui court : la bascule "fait"
  // partirait des l'enfoncement, et l'ecran de creation s'ouvrirait par-dessus
  // une tache qu'on vient de cocher par accident. Ici c'est donc le
  // RELACHEMENT qui decide, et sa duree qui choisit entre cocher et creer --
  // exactement l'idiome de LibraryListActivity::handleButtons().
  bool handleButtons() override;
  // Remplace le pas Suivant/Precedent de base : celui-ci ne sait pas que la
  // ligne d'en-tete "terminees" n'est pas selectionnable (fui::ListItem
  // l'affiche non selectionnee, mais rien cote fui ne fait sauter l'index du
  // bouton par-dessus) — taskListStepSelection le fait.
  void navigateButtons() override;
  // En-tete + sous-ligne d'etat, rejoues par la base a chaque passe de
  // reconstruction ; pieds : les quatre libelles propres a cet ecran.
  void drawChrome() override;
  void drawFooter() override;

 private:
  void rebuildOrder();
  void buildRows(UiScreen& screen);
  void toggleAt(int index);
  void openDetailAt(int index);
  // Creation sur l'appareil, sans serveur ni Wi-Fi : refus du plafond, puis
  // clavier, puis choix de priorite. Les deux etapes sont chainees par leurs
  // gestionnaires de resultat, comme TaskDetailActivity::editTitle() enchaine
  // sur editPriority().
  void createTask();
  void askPriorityForNewTask(std::string title);
  // Ligne d'ecran portant la tache `id`, ou -1 si aucune. Un balayage de `rows`
  // (121 lignes au plus) plutot qu'un indice retenu : l'enregistrement est
  // ajoute en fin d'index mais le tri le place selon sa priorite, donc sa
  // position a l'ecran n'est connue qu'apres rebuildOrder().
  int rowOfTask(const char* id) const;
  void startSync();

  // Ids coches pendant cette visite : ils restent en place, attenues, pour
  // qu'un decochage apres erreur ne demande pas d'ouvrir la bascule.
  std::vector<std::string> tickedHere;
  // Indices dans TASK_STORE.all(), jamais une copie des enregistrements.
  std::vector<int> order;
  std::vector<TaskListRow> rows;
  int openCount = 0;
  int doneCount = 0;
  bool showDone = false;
  bool dirty = true;
  // Message d'erreur en cours dans la sous-ligne d'etat : une ecriture refusee
  // par la file d'ops, ou une creation refusee parce que l'index est deja au
  // plafond. Un seul champ et non deux booleens paralleles : la sous-ligne ne
  // peut en afficher qu'un, et deux drapeaux poseraient la question de savoir
  // lequel gagne et lequel efface l'autre. Voir toggleAt(), createTask() et
  // drawChrome().
  enum class StatusNotice : uint8_t { None, WriteFailed, ListFull };
  StatusNotice notice = StatusNotice::None;

  // Fenetre de ListItem materialisee pour la page visible seulement (comme
  // LibraryListActivity::winItems), pas un tableau de la taille totale.
  std::vector<freeink::ui::ListItem> winItems;
  // Stockage stable pour le libelle "N terminees" tant qu'il est reference
  // par un ListItem::label (un seul a la fois, ligne d'en-tete unique).
  std::string doneHeaderLabel;
};
