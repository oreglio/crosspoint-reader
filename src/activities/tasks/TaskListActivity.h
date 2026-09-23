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
// ~52 Ko (2 x 120 x 216 octets) pendant la visite. onEnter() charge le store
// (rien ne le charge a un demarrage normal) et onExit() appelle
// TaskStore::unload(), donc apres la sortie de l'ecran l'index ne reste pas
// resident, pendant la lecture d'un livre comprise.
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
  // Right (detail) et Left (ajouter) : aucun des deux n'est
  // Back/Confirm/Haut/Bas, donc pas couvert par le contrat de base. Back et
  // Confirmer restent ceux de la base (Confirmer au front d'appui) : sans appui
  // long, rien ne demande plus d'attendre le relachement, et activateIndex()
  // route selon la nature de la ligne.
  bool handleCustomInput() override;
  // En-tete + sous-ligne d'etat, rejoues par la base a chaque passe de
  // reconstruction ; pieds : les quatre libelles propres a cet ecran.
  void drawChrome() override;
  void drawFooter() override;

 private:
  void rebuildOrder();
  void buildRows(UiScreen& screen);
  void toggleAt(int index);
  // Confirmer (ou un toucher) sur la ligne "N faites" : accordeon.
  void toggleDoneSection();
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
  enum class StatusNotice : uint8_t { None, WriteFailed, ListFull, LowMemory };
  StatusNotice notice = StatusNotice::None;
  // Une op attend encore le serveur : la sous-ligne ne dit alors pas « a
  // jour ». Lu sur la carte en onEnter() (jamais depuis le rendu), puis leve
  // par chaque op ajoutee pendant la visite — rien ne vide la file tant que
  // l'ecran est ouvert, la sync vivant dans les Parametres.
  bool pendingOps = false;

  // Fenetre de ListItem materialisee pour la page visible seulement (comme
  // LibraryListActivity::winItems), pas un tableau de la taille totale.
  std::vector<freeink::ui::ListItem> winItems;
  // Stockage stable pour le libelle "N faites" tant qu'il est reference par un
  // ListItem::label (un seul a la fois, la ligne est unique).
  std::string doneRowLabel;
};
