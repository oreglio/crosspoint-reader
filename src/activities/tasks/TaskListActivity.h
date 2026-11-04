#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "activities/tasks/TaskKeepAwake.h"
#include "activities/tasks/TaskListModel.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"
#include "tasks/TaskRecord.h"

// Ecran principal des taches. Derive de UiListActivity (pas d'Activity nue) :
// selection, defilement, pagination, routage tactile ET le squelette de rendu
// (clearScreen / chrome / renderUi / boucle de reconstruction / hints /
// displayBuffer) viennent de la meme fabrique que la quarantaine d'ecrans de
// liste du depot (CONTEXT.md) — cet ecran ne reimplemente que ce qui lui est
// propre : quelles lignes montrer etant donne la vue ouvertes / faites et les
// titres de groupe (TaskListModel.h), plus drawChrome() / drawFooter() pour son titre, son
// compteur d'en-tete et ses libelles de boutons.
//
// Ne detient AUCUNE copie de l'index : `order` et TaskListRow::recordIndex
// indexent directement TASK_STORE.all(). Une copie membre doublait le pic a
// ~52 Ko (2 x 120 x 228 octets) pendant la visite. onEnter() charge le store
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
  // « Garder l'ecran allume » du menu : bloque la mise en veille tant que la
  // liste est au premier plan. Rien ne tourne pendant ce temps, donc le
  // processeur peut descendre a son horloge basse entre deux gestes, comme
  // pour le compte a rebours (CountdownActivity).
  bool preventAutoSleep() override { return task_keep_awake::active; }
  bool allowPowerSavingWhileAwake() const override { return true; }

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // La popup d'abord (elle possede tous les boutons tant qu'elle est ouverte),
  // puis Right (detail, ou cocher la selection) et Left (ajouter, ou effacer
  // la selection), hors du contrat de base. Confirmer sur un titre de groupe
  // ajoute une tache dans ce groupe ; le mode selection saute les titres.
  bool handleCustomInput() override;
  // Confirmer se lit au RELACHEMENT, pas au front d'appui comme dans la base :
  // la duree de l'appui choisit entre l'action de la ligne et son menu. Meme
  // idiome et meme seuil que la Library (dispatch a la relache selon
  // getHeldTime()).
  bool handleButtons() override;
  // Appui long tactile sur une tache : son menu (en mode selection : choisir la
  // ligne, comme un toucher). Sur un titre de groupe ou la ligne d'ajout : un
  // toucher.
  void onRowLongPress(int index) override;
  // Un toucher sur une ligne rend le curseur a la liste.
  void onRowAction(const freeink::ui::ActionEvent& event) override;
  // Retour (bouton ou fleche de l'en-tete) quitte d'abord le mode selection,
  // puis la vue des faites, puis l'ecran.
  void onBackButton() override;
  // Haut depuis la premiere ligne monte sur le compteur de l'en-tete ; Bas ou
  // Haut depuis le compteur redescend dans la liste. En mode selection, les
  // titres de groupe sont sautes. Le reste comme la base.
  void navigateButtons() override;
  // En-tete (titre de la vue, compteur de l'autre vue + marque d'envoi en
  // attente), rejoue par la base a chaque passe de reconstruction ; pieds :
  // les quatre libelles propres a cet ecran.
  void drawChrome() override;
  void drawFooter() override;

 private:
  void rebuildOrder();
  void buildRows(UiScreen& screen);
  // Titres de groupe : list() ne pose qu'une ligne vide (fond, selection,
  // zone tactile) ; le nom, le compteur et le filet sont peints ici, par-dessus,
  // a la geometrie que list() vient d'employer. Voir le .cpp.
  void drawGroupHeaders(UiScreen& screen, const freeink::ui::ListProps& props) const;
  // Pas de Haut/Bas : ButtonNavigator hors mode selection, les titres sautes
  // en mode selection (Gauche et Droite y effacent et cochent).
  int stepFrom(int from, int direction) const;
  // Premiere ligne ou le curseur peut se poser : 0, ou la premiere qui n'est
  // pas un titre en mode selection.
  int firstCursorRow() const;
  // `index`, ou la tache qui suit si c'est un titre en mode selection : aucun
  // chemin ne laisse le curseur sur un titre pendant la selection.
  int cursorRowFor(int index) const;
  // Ligne selectionnee si c'est un titre de groupe hors mode selection et hors
  // compteur, -1 sinon.
  int focusedGroupHeader();
  void toggleAt(int index);
  // Confirmer sur le compteur de l'en-tete (ou un toucher dessus) : remplace
  // la liste par l'autre vue. Les taches qui quittent l'ecran quittent aussi
  // la selection ; le curseur revient sur la ligne 0.
  void switchView();
  // Le compteur de l'en-tete existe : en vue faites toujours (il ramene a la
  // liste ouverte), en vue ouverte s'il y a au moins une tache faite.
  bool hasHeaderCounter() const;
  // Libelle droit de l'en-tete : « 3 faites », « 2 a faire », suivi de U+21BB
  // si une op attend le serveur (U+21BB seul sans compteur). Vide sinon.
  void formatHeaderRight(char* out, size_t size) const;
  void openDetailAt(int index);
  // Menu d'une tache : cocher/rouvrir, voir, modifier, groupe (s'il y en a),
  // selectionner, supprimer les faites (s'il y en a), garder l'ecran allume, taille du
  // texte, supprimer, synchroniser. Les choix retrouvent la ligne par l'id de la
  // tache, jamais par un indice retenu : l'ordre peut avoir change entre
  // l'ouverture et le choix.
  void openTaskMenu(int index);
  void editTaskTitle(const std::string& id);
  // « Taille du texte des taches » depuis le menu : ouvre le choix 10/12/14.
  void chooseTextSize();
  void editTaskPriority(const std::string& id);
  // « Groupe » du menu : « Sans groupe » puis les groupes, dans l'ordre des
  // titres ; puis op Group, index, et le curseur suit la tache.
  void chooseGroup(const std::string& taskId);
  void promptDeleteTask(const std::string& id);
  void deleteTask(const std::string& id);
  // « Supprimer les faites (N) » du menu, apres confirmation.
  void promptDeleteDone();

  // Mode selection (menu > Selectionner) : Confirmer ou un toucher choisit la
  // ligne, Droite coche les taches choisies, Gauche les supprime apres
  // confirmation, Retour quitte le mode. Rien n'est ecrit avant l'action,
  // qui quitte le mode : `selection` reste donc indexee comme TASK_STORE.all().
  void startSelection(const std::string& id);
  void stopSelection();
  void toggleSelectionAt(int index);
  void tickSelection();
  void promptDeleteSelection();
  enum class BatchAction : uint8_t { Tick, Delete };
  // Une op par cible dans UNE ouverture de la file, puis l'index une seule
  // fois. `targets` : indices de TASK_STORE.all() du plus grand au plus petit
  // (taskSelectionTargets / taskDoneTargets), pour que les effacements ne
  // decalent pas les suivants.
  void applyBatch(BatchAction action, const uint8_t* targets, size_t count);
  bool selecting = false;
  TaskSelection selection;
  int selectionCount = 0;
  // Apres une modification d'une tache existante : reconstruit l'ordre et
  // garde la selection sur elle (la priorite la deplace dans le tri).
  void refreshAfterEdit(const char* id);
  // Creation sur l'appareil, sans serveur ni Wi-Fi : refus du plafond, puis
  // clavier, puis choix de priorite. Les deux etapes sont chainees par leurs
  // gestionnaires de resultat, comme TaskDetailActivity::editTitle() enchaine
  // sur editPriority(). `groupId` : « ajouter » sur un titre de groupe, la
  // tache nait dans ce groupe ; nullptr (Gauche « Nouv. ») = sans groupe.
  void createTask(const char* groupId = nullptr);
  void askPriorityForNewTask(std::string title, std::string groupId);
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
  // Vue exclusive : les taches ouvertes ou les faites, jamais les deux.
  TaskListView view = TaskListView::Open;
  // Le curseur est sur le compteur de l'en-tete, hors de la liste fui : aucune
  // ligne n'est alors surlignee, et la selection de la liste reste sur 0.
  bool headerFocused = false;
  // Zone tactile du compteur, posee par drawChrome() sous le verrou de rendu
  // et relue sous ce meme verrou par handleButtons().
  Rect counterTouchRect;
  // Police de la liste, choisie par le reglage « Taille du texte des taches ».
  int listFontId = 0;
  static int taskListFontId();
  // Police des titres de groupe, liee a l'emplacement FONT_LABEL de cet ecran.
  static int groupTitleFontId();
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

  OptionPopup popup;
};
