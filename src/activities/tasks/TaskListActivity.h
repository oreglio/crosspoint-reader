#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "activities/tasks/TaskListModel.h"
#include "components/OptionPopup.h"
#include "tasks/TaskRecord.h"

// Ecran principal des taches. Derive de UiListActivity (pas d'Activity nue) :
// selection, defilement, pagination, routage tactile ET le squelette de rendu
// (clearScreen / chrome / renderUi / boucle de reconstruction / hints /
// displayBuffer) viennent de la meme fabrique que la quarantaine d'ecrans de
// liste du depot (CONTEXT.md) — cet ecran ne reimplemente que ce qui lui est
// propre : quelles lignes montrer etant donne la bascule "terminees"
// (TaskListModel.h), plus drawChrome() / drawFooter() pour son titre et ses
// libelles de boutons.
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
  // La popup se peint par-dessus l'image retenue, sans effacer la liste :
  // meme idiome que LibraryListActivity::render().
  void render(RenderLock&& lock) override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // La popup d'abord (elle possede tous les boutons tant qu'elle est ouverte),
  // puis Right (detail) et Left (ajouter), hors du contrat de base.
  bool handleCustomInput() override;
  // Confirmer se lit au RELACHEMENT, pas au front d'appui comme dans la base :
  // la duree de l'appui choisit entre l'action de la ligne et son menu. Meme
  // idiome et meme seuil que la Library (dispatch a la relache selon
  // getHeldTime()).
  bool handleButtons() override;
  // Appui long tactile sur une ligne : le meme menu.
  void onRowLongPress(int index) override;
  // En-tete (compteur + marque d'envoi en attente), rejoue par la base a chaque passe de
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
  // Menu d'une tache : cocher/rouvrir, voir, modifier, synchroniser,
  // supprimer. Les choix retrouvent la ligne par l'id de la tache, jamais par
  // un indice retenu : l'ordre peut avoir change entre l'ouverture et le choix.
  void openTaskMenu(int index);
  void editTaskTitle(const std::string& id);
  void editTaskPriority(const std::string& id);
  void promptDeleteTask(const std::string& id);
  void deleteTask(const std::string& id);
  // Apres une modification d'une tache existante : reconstruit l'ordre et
  // garde la selection sur elle (la priorite la deplace dans le tri).
  void refreshAfterEdit(const char* id);
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

  // Indices dans TASK_STORE.all(), jamais une copie des enregistrements.
  std::vector<int> order;
  std::vector<TaskListRow> rows;
  int openCount = 0;
  int doneCount = 0;
  bool showDone = false;
  bool dirty = true;
  // Erreur a montrer en toast par-dessus la liste : une ecriture refusee par
  // la file d'ops, une creation refusee au plafond, ou le tas trop fragmente.
  // Un seul champ : un toast n'en montre qu'un, le dernier gagne. Pose par
  // showNotice(), efface par handleCustomInput() apres kNoticeMs.
  enum class StatusNotice : uint8_t { None, WriteFailed, ListFull, LowMemory };
  StatusNotice notice = StatusNotice::None;
  unsigned long noticeSince = 0;
  static constexpr unsigned long kNoticeMs = 2000;
  void showNotice(StatusNotice next);
  const char* noticeText() const;
  // Une op attend encore le serveur : le bord droit du titre porte alors U+21BB. Lu sur la
  // carte en onEnter() (jamais depuis le rendu), puis leve
  // par chaque op ajoutee pendant la visite — rien ne vide la file tant que
  // l'ecran est ouvert (la sync redemarre l'appareil).
  bool pendingOps = false;

  // Fenetre de ListItem materialisee pour la page visible seulement (comme
  // LibraryListActivity::winItems), pas un tableau de la taille totale.
  std::vector<freeink::ui::ListItem> winItems;
  // Stockage stable pour le libelle "N faites" tant qu'il est reference par un
  // ListItem::label (un seul a la fois, la ligne est unique).
  std::string doneRowLabel;

  OptionPopup popup;
};
