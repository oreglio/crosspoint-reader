#include "TaskListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "MappedInputManager.h"
#include "TaskStore.h"
#include "activities/tasks/TaskDetailActivity.h"
#include "activities/tasks/TaskPriorityChoices.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "tasks/TaskOpQueue.h"

namespace fui = freeink::ui;

namespace {
constexpr char TAG[] = "TASKLIST";

// Pastilles circulaires de la maquette. 16x16, 1 bit par pixel, row-major
// MSB-first, bit a 1 = encre : c'est le contrat de fui::BitmapRef/BW1, que
// FreeInkUIGfxRenderer::bitmap() echantillonne via forEachBitmapPixel() puis
// pose avec drawPixel(). Ce n'est PAS le contrat pre-tourne de
// GfxRenderer::drawIcon (cf. la note de rotation de .claude/CONTEXT.md) : rien
// ici ne doit etre stocke tourne — et un disque est de toute facon invariant
// par rotation.
//
// `static const` donc en flash (.rodata), pas en DRAM : regle de ressources 3
// du CLAUDE.md. Le champ BitmapRef::progmem n'est lu nulle part dans le SDK
// (verifie : il n'apparait qu'a sa declaration) et sur ESP32 la flash est
// mappee en lecture directe, donc le dereferencement de `data` est sans
// danger — aucun pgm_read_byte necessaire, contrairement a l'AVR.
constexpr int16_t TASK_BULLET_PX = 16;

// Anneau ouvert : tache pas encore faite.
static const uint8_t TASK_BULLET_OPEN[] = {
    0x00, 0x00, 0x07, 0xe0, 0x1f, 0xf8, 0x38, 0x1c, 0x30, 0x0c, 0x60, 0x06, 0x60, 0x06, 0x60, 0x06,
    0x60, 0x06, 0x60, 0x06, 0x60, 0x06, 0x30, 0x0c, 0x38, 0x1c, 0x1f, 0xf8, 0x07, 0xe0, 0x00, 0x00,
};

// Disque plein : tache faite (ou cochee pendant cette visite).
static const uint8_t TASK_BULLET_DONE[] = {
    0x00, 0x00, 0x07, 0xe0, 0x1f, 0xf8, 0x3f, 0xfc, 0x3f, 0xfc, 0x7f, 0xfe, 0x7f, 0xfe, 0x7f, 0xfe,
    0x7f, 0xfe, 0x7f, 0xfe, 0x7f, 0xfe, 0x3f, 0xfc, 0x3f, 0xfc, 0x1f, 0xf8, 0x07, 0xe0, 0x00, 0x00,
};

// Marque d'etat de la ligne "N faites" : U+203A replie, U+00BB deplie. Toutes
// deux presentes dans les polices integrees (scripts/measure_label.py) ; la
// maquette montrait U+25B8, absent, qui aurait ete saute en silence. Aucun
// triangle vers le bas n'y figure, d'ou deux chevrons plutot que la paire
// droite/bas habituelle — le pied de page dit deplier/replier en toutes lettres.
constexpr char DONE_ROW_COLLAPSED_MARK[] = "\u203A";
constexpr char DONE_ROW_EXPANDED_MARK[] = "\u00BB";

fui::BitmapRef taskBullet(const bool done) {
  fui::BitmapRef ref;
  ref.data = done ? TASK_BULLET_DONE : TASK_BULLET_OPEN;
  ref.width = TASK_BULLET_PX;
  ref.height = TASK_BULLET_PX;
  ref.format = fui::BitmapFormat::BW1;
  return ref;
}
}  // namespace

TaskListActivity::TaskListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("TaskList", renderer, mappedInput, /*wantsTouchLongPress=*/false) {}

void TaskListActivity::onEnter() {
  // Un seul verrou sur le cycle de vie de base ET la phase de donnees : le
  // onEnter de base programme un rendu, et la tache de rendu ne doit pas lire
  // order/rows avant qu'ils soient en place (meme rituel que
  // LibraryListActivity::onEnter).
  RenderLock lock(*this);
  UiListActivity::onEnter();

  // onExit() decharge le store, donc ce n'est pas seulement le premier acces
  // qui a besoin d'un chargement : ensureLoaded() relit bien la carte a chaque
  // reouverture de l'ecran. Jamais depuis le rendu — voir
  // PersistableStore::ensureLoaded().
  TASK_STORE.ensureLoaded();

  tickedHere.clear();
  showDone = false;
  notice = StatusNotice::None;
  dirty = true;
  rebuildOrder();
  // Selection par defaut : la ligne 0, que le onEnter de base vient de poser.
  // buildTaskListRows() garantit qu'elle est la premiere tache ouverte s'il y
  // en a une, la ligne d'ajout sur un index vide (TaskListModelTest,
  // RowZeroIsTheDefaultSelectionForEveryState).
}

void TaskListActivity::onExit() {
  // Rien ne doit rester resident pendant la lecture : clear() seul garde la
  // capacite reservee, shrink_to_fit() la rend vraiment.
  tickedHere.clear();
  tickedHere.shrink_to_fit();
  order.clear();
  order.shrink_to_fit();
  rows.clear();
  rows.shrink_to_fit();
  winItems.clear();
  winItems.shrink_to_fit();
  doneRowLabel.clear();
  doneRowLabel.shrink_to_fit();
  // L'index lui-meme appartient au store, pas a l'activite : sans cet appel
  // ses ~26 Ko survivraient a l'ecran pour toute la session (main.cpp charge
  // le store a chaque demarrage). Cet ecran est le seul detenteur d'indices
  // dedans, et ils viennent d'etre liberes ci-dessus.
  TASK_STORE.unload();
  Activity::onExit();
}

void TaskListActivity::rebuildOrder() {
  if (!dirty) return;
  const std::vector<TaskRecord>& records = TASK_STORE.all();
  buildTaskOrder(records, tickedHere, order, openCount);
  buildTaskListRows(order, openCount, showDone, rows);
  doneCount = static_cast<int>(records.size()) - openCount;
  dirty = false;
}

int TaskListActivity::listCount() const { return static_cast<int>(rows.size()); }

void TaskListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Contenu sous l'en-tete + la sous-ligne d'etat, au-dessus des indices de
  // boutons -- ces deux bandes sont peintes hors fui, dans drawChrome() /
  // drawFooter(), donc reservees ici pour que la liste ne les recouvre pas
  // (meme rituel que LibraryListActivity::buildScreen).
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) +
                                       metrics.tabBarHeight),
                  0, static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});

  // Jamais vide : un index vide donne la ligne d'ajout, un index tout fait la
  // ligne "N faites". Les messages "aucune tache" / "tout est fait" sont dans
  // la sous-ligne d'etat (drawChrome()).
  buildRows(screen);
}

void TaskListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  const std::vector<TaskRecord>& records = TASK_STORE.all();

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  // Les titres peuvent s'enrouler sur deux lignes plutot que d'etre tronques
  // -- une exigence de la maquette, pas une simplification.
  props.labelText.maxLines = 2;
  // Taille native des pastilles : list() les pose en BitmapMode::Contain, donc
  // une iconSize differente les ferait redimensionner au plus proche voisin et
  // l'anneau prendrait une epaisseur irreguliere sur un panneau 1 bit.
  props.iconSize = TASK_BULLET_PX;
  syncListViewport(screen, props, /*hasSubtitle=*/false);

  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1);
  if (winItems.capacity() < cap) winItems.reserve(cap);
  winItems.clear();

  const int windowStart = static_cast<int>(props.topIndex);
  int built = 0;
  for (int i = windowStart; i < count && built < static_cast<int>(cap); ++i) {
    const TaskListRow& row = rows[static_cast<size_t>(i)];
    fui::ListItem item;
    item.actionValue = static_cast<int16_t>(i);
    if (row.kind == TaskRowKind::DoneSection) {
      // Ligne ordinaire et non ListItem::isHeader : le SDK dessine une en-tete
      // non selectionnable et n'enregistre pas son action (list.h:442), or cet
      // accordeon doit se selectionner et se toucher. Ce qui la distingue d'une
      // tache est l'absence de pastille (son texte s'aligne sur la colonne des
      // pastilles) et sa marque d'etat ; hauteur, selection et zone tactile
      // restent celles de list().
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%s %d %s", showDone ? DONE_ROW_EXPANDED_MARK : DONE_ROW_COLLAPSED_MARK,
                    doneCount, tr(STR_TASK_DONE_COUNT));
      doneRowLabel = buf;
      item.label = doneRowLabel.c_str();
    } else if (row.kind == TaskRowKind::AddTask) {
      // Index vide seulement (buildTaskListRows()) : sur une liste peuplee,
      // l'ajout passe par le bouton Gauche.
      item.label = tr(STR_TASK_ADD_ROW);
    } else {
      const TaskRecord& rec = records[static_cast<size_t>(row.recordIndex)];
      item.label = rec.title;
      // Pastille circulaire de la maquette : anneau ouvert / disque plein.
      item.icon = taskBullet(rec.done);
      // Priorite haute seulement : ListItem::emphasis est un booleen par
      // ligne (freeink-sdk, ajoute pour ce chantier -- fui::ListProps::
      // labelText ne portait qu'un TextStyle unique pour tout l'appel de
      // list(), jamais par ligne, ce qui rendait un "gras" par ligne
      // impossible sans segmenter l'appel par bande de priorite et donc
      // re-derive la pagination que ListNav possede deja). Deux paliers, pas
      // trois : la priorite basse ne se distingue que par sa place en fin de
      // tri (taskOrderBefore), pas par un style — l'exception (haute) se
      // marque, pas la regle. Une tache faite (ou cochee pendant cette
      // visite) cesse de crier : le gras ne vaut que pour une tache ouverte.
      item.emphasis = (rec.priority == TASK_PRIORITY_HIGH) && !rec.done;
      // Pas de fui::StateDisabled sur une tache faite : BoxStyle::resolve()
      // (FreeInkUICore.h:606-615) teste Disabled AVANT Selected, donc une ligne
      // faite ET selectionnee prend le style "disabled", sans fond de
      // selection — le curseur disparaissait apres chaque coche, sur tous les
      // themes (visite simulateur, 02-ticked-in-place). Et sur ces themes 1 bit
      // le style disabled n'attenue rien de visible. C'est donc le disque plein
      // qui dit "faite", et la perte du gras pour une priorite haute.
    }
    winItems.push_back(item);
    ++built;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
}

void TaskListActivity::activateIndex(int index) {
  if (index < 0 || index >= static_cast<int>(rows.size())) return;
  switch (rows[static_cast<size_t>(index)].kind) {
    case TaskRowKind::Task:
      toggleAt(index);
      return;
    case TaskRowKind::DoneSection:
      toggleDoneSection();
      return;
    case TaskRowKind::AddTask:
      createTask();
      return;
  }
}

bool TaskListActivity::handleCustomInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    openDetailAt(activeNav().selected);
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    // Ajouter, sur une liste peuplee comme sur une liste vide : une seule
    // regle. La ligne "+ Ajouter une tache" d'un index vide mene au meme
    // endroit, pour qui ne cherche pas le bouton.
    createTask();
    return true;
  }
  // Gauche/Droite appartiennent aussi aux ensembles precedent/suivant de
  // ButtonNavigator (getPreviousButtons/getNextButtons). Sans cette
  // consommation, MAINTENIR "ajouter" ferait defiler la liste page par page via
  // navigateButtons() avant que le relachement n'ouvre la creation — et,
  // parce que la release est consommee ici, ButtonNavigator::lastContinuousNavTime
  // ne serait jamais remis a zero, ce qui avalerait en silence l'appui
  // Haut/Bas suivant. Sur CET ecran ces deux boutons veulent dire "ajouter"
  // et "detail", jamais page precedente/suivante. Correction locale :
  // ni UiListActivity ni LibraryListActivity ne sont touches.
  if (mappedInput.isPressed(MappedInputManager::Button::Left) ||
      mappedInput.isPressed(MappedInputManager::Button::Right)) {
    return true;
  }
  return false;
}

void TaskListActivity::toggleDoneSection() {
  showDone = !showDone;
  dirty = true;
  rebuildOrder();
  // Ne s'execute que selection SUR la ligne "N faites" (activateIndex()).
  // Cette ligne suit les taches ouvertes, que la bascule ne touche pas : son
  // indice ne bouge pas et la selection y reste. Le clamp n'est qu'une garde
  // de bornes, pas un cas attendu.
  const int next = taskListClampSelection(rows, activeNav().selected);
  if (next >= 0) moveSelectionTo(next);
}

void TaskListActivity::toggleAt(int index) {
  if (index < 0 || index >= static_cast<int>(rows.size())) return;
  const TaskListRow row = rows[static_cast<size_t>(index)];
  if (row.kind != TaskRowKind::Task) return;

  // Copie (216 octets de pile, sous le seuil de 256 du CLAUDE.md) et non une
  // reference : upsert() peut push_back et donc reallouer le vecteur du store.
  // Invariant du jour : l'id existe deja dans l'index (il vient de la), donc
  // upsert() prend la branche "remplace en place" et ne realloue pas — mais
  // c'est ecrit ici plutot que suppose en silence.
  TaskRecord rec = TASK_STORE.all()[static_cast<size_t>(row.recordIndex)];
  rec.done = !rec.done;

  // La file d'ops AVANT l'index, et non l'inverse. Ce sont deux ecritures SD
  // distinctes : une coupure entre les deux doit laisser l'etat reparable. Une
  // op en file sans changement local l'est (Done(id, true) est idempotente, la
  // prochaine sync l'applique) ; un index qui dit "faite" avec une file vide
  // ne l'est pas — le serveur n'apprendrait jamais la coche et le prochain
  // replaceAll() rouvrirait la tache sans laisser de trace du geste.
  TaskOp op{};
  op.kind = TaskOpKind::Done;
  std::snprintf(op.id, sizeof(op.id), "%s", rec.id);
  op.done = rec.done;
  if (!TASK_STORE.appendOp(op)) {
    // Echec d'ecriture (carte pleine, fichier verrouille) : ne rien appliquer
    // du tout. `rec` est une copie locale, donc l'index n'a pas bouge ; il
    // reste a le dire a l'ecran, pas seulement au port serie. Le depot n'a pas
    // de primitive de toast reutilisable hors du lecteur, donc le message
    // prend la sous-ligne d'etat que drawChrome() peint deja.
    LOG_ERR(TAG, "Echec de l'enregistrement de la bascule 'fait' pour %s", rec.id);
    notice = StatusNotice::WriteFailed;
    requestUpdate();
    return;
  }
  // Une ecriture qui passe perime tout message d'erreur precedent, y compris un
  // refus de creation : la carte repond, et le plafond n'est plus la chose que
  // la sous-ligne doit dire en priorite.
  notice = StatusNotice::None;

  const std::string id(rec.id);
  const auto already = std::find(tickedHere.begin(), tickedHere.end(), id);
  if (rec.done) {
    // Reste en place, attenuee, jusqu'a onExit() : voir le commentaire sur
    // tickedHere dans le .h.
    if (already == tickedHere.end()) tickedHere.push_back(id);
  } else if (already != tickedHere.end()) {
    // Decochage d'une erreur commise pendant cette visite : elle redevient
    // une tache ouverte ordinaire, plus besoin de la retenir a part.
    tickedHere.erase(already);
  }

  TASK_STORE.upsert(rec);

  dirty = true;
  rebuildOrder();
  // La selection suit la TACHE, pas l'indice. Decocher une tache de la section
  // depliee la fait remonter parmi les ouvertes, et "N faites" glisse a son
  // ancien indice : garder l'indice ferait replier la section au Confirmer
  // suivant, au lieu d'agir sur la tache qu'on vient de toucher.
  const int moved = rowOfTask(id.c_str());
  const int next = moved >= 0 ? moved : taskListClampSelection(rows, activeNav().selected);
  if (next >= 0) moveSelectionTo(next);
}

void TaskListActivity::openDetailAt(int index) {
  if (index < 0 || index >= static_cast<int>(rows.size()) ||
      rows[static_cast<size_t>(index)].kind != TaskRowKind::Task) {
    return;
  }
  // L'id, pas l'indice : l'ecran de detail modifie le store, et un indice ne
  // survivrait pas a une reorganisation. Copie dans une std::string locale
  // parce que le constructeur du detail lit la chaine avant que quoi que ce
  // soit ne bouge.
  const TaskRecord& opened = TASK_STORE.all()[static_cast<size_t>(rows[static_cast<size_t>(index)].recordIndex)];
  const std::string id = opened.id;
  // Etat AVANT la visite. tickedHere veut dire "cochee pendant cette visite",
  // c'est-a-dire une TRANSITION de non-faite a faite : l'etat final seul ne
  // peut pas l'exprimer. Voir le gestionnaire ci-dessous.
  const bool wasDone = opened.done;

  startActivityForResult(std::make_unique<TaskDetailActivity>(renderer, mappedInput, id.c_str()),
                         [this, id, wasDone](const ActivityResult& result) {
                           // Rien ne rappelle onEnter() sur une activite depilee : ActivityManager
                           // la restaure par std::move et ne fait tourner que ce gestionnaire
                           // (chemin Pop). `order`, `rows`, `openCount`, `showDone` et
                           // `tickedHere` sont donc exactement ceux d'avant la visite — ce qu'on
                           // veut pour l'etat d'affichage, mais ce qui afficherait un titre ou une
                           // priorite perimes si le detail a modifie la tache. C'est ici ou nulle
                           // part que la liste l'apprend.
                           const auto* edit = std::get_if<TaskEditResult>(&result.data);
                           if (edit == nullptr) {
                             // Invariant tenu par construction aujourd'hui : TaskDetailActivity ne
                             // sort que par finishWithResult(). S'il gagnait une quatrieme sortie
                             // qui l'oublie, la liste resterait perimee toute la visite sans log,
                             // sans assert et sans indice visuel — la panne la plus penible a
                             // diagnostiquer. Une ligne la rend audible.
                             LOG_ERR(TAG, "Detail screen returned no TaskEditResult; the list may now be stale");
                             return;
                           }
                           if (!edit->changed) return;  // simple coup d'oeil : rien a refaire

                           // Une coche faite dans le detail reste en place, attenuee, exactement
                           // comme une coche faite ici : tickedHere existe pour qu'un geste
                           // regrette se defasse sans ouvrir la section repliee, et cette raison
                           // ne depend pas de l'ecran ou le geste a eu lieu.
                           //
                           // La condition est une TRANSITION, pas un etat final. buildTaskOrder
                           // traite tickedHere comme un contournement de `done`
                           // (TaskListModel.cpp:32-35), donc y pousser une tache DEJA faite avant
                           // la visite la ferait ressortir de la section "terminees" vers la liste
                           // des ouvertes et ferait baisser le compte "N faites" — rien que pour
                           // l'avoir renommee. D'ou `!wasDone && isDone`.
                           //
                           // Le retrait, lui, n'a lieu que si la tache n'est plus faite : une
                           // tache cochee ICI puis seulement renommee dans le detail doit garder
                           // son entree, sinon elle plongerait dans la section repliee. Meme
                           // symetrie que toggleAt(). AVANT rebuildOrder(), qui lit tickedHere.
                           const TaskRecord* rec = TASK_STORE.find(id.c_str());
                           const bool isDone = rec != nullptr && rec->done;
                           const auto already = std::find(tickedHere.begin(), tickedHere.end(), id);
                           if (isDone && !wasDone) {
                             if (already == tickedHere.end()) tickedHere.push_back(id);
                           } else if (!isDone && already != tickedHere.end()) {
                             tickedHere.erase(already);
                           }

                           dirty = true;
                           rebuildOrder();
                           // La tache a pu changer de place (priorite, decochage) : la selection
                           // la suit, comme dans toggleAt(). Si elle a quitte l'ecran (cochee
                           // alors que deja faite, section repliee), le clamp garde un indice
                           // valide.
                           const int moved = rowOfTask(id.c_str());
                           const int next = moved >= 0 ? moved : taskListClampSelection(rows, activeNav().selected);
                           if (next >= 0) moveSelectionTo(next);
                           requestUpdate();
                         });
}

void TaskListActivity::createTask() {
  // Le plafond se verifie AVANT d'ouvrir le clavier, et non apres la saisie.
  // TaskStore::upsert() rend void et jette l'enregistrement en silence des que
  // l'index est a MAX_TASKS (TaskStore.cpp:172-175) : l'appelant ne peut pas
  // s'en apercevoir apres coup. Le decouvrir seulement une fois le titre tape
  // laisserait une op Add en file pour une tache que l'appareil n'affichera
  // jamais — le serveur l'accepterait, et l'ecran resterait vide jusqu'a ce
  // qu'une sync la ramene. Refuser d'entree est moins mauvais que cela.
  if (TASK_STORE.all().size() >= MAX_TASKS) {
    LOG_ERR(TAG, "Index already at the %zu-task cap; not opening the keyboard", MAX_TASKS);
    notice = StatusNotice::ListFull;
    requestUpdate();
    return;
  }

  // Un toucher sur "+ Ajouter une tache" quitte l'ecran : sans cela un flash de
  // toucher residuel griserait la ligne 0 au retour (UiListActivity.h:41-43).
  app.clearTapFlash();

  // minLength=1 : c'est le clavier qui refuse un titre vide, comme pour la
  // modification d'un titre existant (TaskDetailActivity::editTitle()).
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TASK_NEW), "",
                                                                 TASK_TITLE_MAX, InputType::Text, /*minLength=*/1),
                         [this](const ActivityResult& result) {
                           const auto* entered = std::get_if<KeyboardResult>(&result.data);
                           // Annulation : rien n'a ete ecrit, ni op ni index, et il n'y a donc
                           // rien a reconstruire. Le repaint est deja programme par le depilage
                           // (ActivityManager.cpp:400-406), comme au retour de l'ecran de detail.
                           if (result.isCancelled || entered == nullptr) return;
                           askPriorityForNewTask(entered->text);
                         });
}

void TaskListActivity::askPriorityForNewTask(std::string title) {
  // `labels` et non `rows` : ce dernier est le membre que le gestionnaire
  // ci-dessous reconstruit, et le masquer ici se relirait mal.
  std::vector<std::string> labels;
  labels.reserve(TASK_PRIORITY_CHOICE_COUNT);
  for (const uint8_t priority : TASK_PRIORITY_CHOICES) {
    labels.emplace_back(I18N.get(taskPriorityLabelId(priority)));
  }

  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "TaskPriority", StrId::STR_TASK_PRIORITY,
                                                std::move(labels), TASK_DEFAULT_PRIORITY_CHOICE),
      [this, title = std::move(title)](const ActivityResult& result) {
        const auto* choice = std::get_if<OptionSelectionResult>(&result.data);
        // Annuler la priorite annule toute la creation. Rien n'a encore ete
        // ecrit a ce stade — ni op, ni index — donc il n'y a rien a defaire :
        // c'est bien ce qui justifie que les deux ecritures viennent apres les
        // deux ecrans, et pas entre les deux.
        if (result.isCancelled || choice == nullptr ||
            static_cast<size_t>(choice->index) >= TASK_PRIORITY_CHOICE_COUNT) {
          return;
        }

        TaskRecord rec{};
        const std::string id = TaskStore::newDeviceId();
        std::snprintf(rec.id, sizeof(rec.id), "%s", id.c_str());
        std::snprintf(rec.title, sizeof(rec.title), "%s", title.c_str());
        rec.priority = TASK_PRIORITY_CHOICES[choice->index];
        rec.done = false;
        rec.noteBytes = 0;

        TaskOp op{};
        op.kind = TaskOpKind::Add;
        std::memcpy(op.id, rec.id, sizeof(op.id));
        std::memcpy(op.title, rec.title, sizeof(op.title));
        op.priority = rec.priority;

        // La file d'ops AVANT l'index, comme toggleAt() et comme
        // TaskDetailActivity::appendAndApply(). Ce sont deux ecritures SD
        // distinctes : une coupure entre les deux doit laisser un etat
        // reparable. Une op Add en file sans enregistrement local l'est (la
        // prochaine sync la renvoie, puis le replaceAll() ramene la tache) ; un
        // enregistrement local sans op ne l'est pas — le serveur n'apprendrait
        // jamais la tache, et le premier replaceAll() l'effacerait.
        if (!TASK_STORE.appendOp(op)) {
          LOG_ERR(TAG, "Could not queue the add op for %s; task not created", rec.id);
          notice = StatusNotice::WriteFailed;
          requestUpdate();
          return;
        }
        // Pas de saveToFile() explicite : upsert() l'appelle deja sur ses deux
        // branches (TaskStore.cpp:168, 177), et un second appel reecrirait tout
        // l'index une deuxieme fois par tache creee, pour rien.
        TASK_STORE.upsert(rec);
        notice = StatusNotice::None;

        // Rien ne rappelle onEnter() sur une activite depilee : sans cette
        // reconstruction la tache existe sur la carte mais n'apparait dans
        // aucune ligne. Meme quatuor qu'au retour de l'ecran de detail.
        dirty = true;
        rebuildOrder();
        const int created = rowOfTask(rec.id);
        // La tache neuve est ouverte, donc toujours dans la partie visible de
        // la liste : rowOfTask() ne rend -1 que si upsert() l'a refusee, et la
        // selection reprend alors simplement sa route normale.
        const int next = created >= 0 ? created : taskListClampSelection(rows, activeNav().selected);
        if (next >= 0) moveSelectionTo(next);
        requestUpdate();
      });
}

int TaskListActivity::rowOfTask(const char* id) const {
  const std::vector<TaskRecord>& records = TASK_STORE.all();
  for (size_t i = 0; i < rows.size(); i++) {
    if (rows[i].kind != TaskRowKind::Task) continue;
    if (std::strcmp(records[static_cast<size_t>(rows[i].recordIndex)].id, id) == 0) return static_cast<int>(i);
  }
  return -1;
}

void TaskListActivity::startSync() {
  // TaskSyncActivity arrive avec la Tache 9, qui devra aussi choisir quel
  // bouton la declenche : les boutons avant sont deja pris par
  // Retour / cocher / ajouter / detail (voir le rapport).
  LOG_INF(TAG, "Sync demandee (TaskSyncActivity : Tache 9)");
}

void TaskListActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  char title[48];
  std::snprintf(title, sizeof(title), "%s (%d)", tr(STR_TASK_TITLE), openCount);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, title, false);
  } else {
    GUI.drawHeader(renderer, header, title);
  }

  // Sous-ligne d'etat. Normalement la fraicheur de sync : seule l'information
  // qu'on a vraiment (appairee ou non) y est affichee, un horodatage relatif
  // ("il y a 2h") demanderait un champ que rien n'ecrit encore -- voir le
  // rapport. Elle sert aussi de surface d'erreur pour une coche refusee : ce
  // depot n'a pas de toast reutilisable hors du lecteur, et cette ligne est
  // deja peinte a chaque rendu.
  //
  // Priorite : erreur > appairage requis > "tout est fait" > "a jour". Une
  // erreur demande une action immediate ; l'appairage aussi, et rien d'autre
  // a l'ecran ne le dit. "Tout est fait" se DEDUIT des compteurs a chaque
  // rendu — ce n'est pas un evenement, donc pas une valeur de StatusNotice,
  // qu'une coche remettrait a None. Un index vide n'a pas de message propre :
  // la ligne "+ Ajouter une tache" le dit deja.
  const bool allDone = openCount == 0 && doneCount > 0;
  const char* status = notice == StatusNotice::ListFull      ? tr(STR_TASK_LIST_FULL)
                       : notice == StatusNotice::WriteFailed ? tr(STR_TASK_TICK_FAILED)
                       : !TASK_STORE.hasSecret()             ? tr(STR_TASK_PAIRING_REQUIRED)
                       : allDone                             ? tr(STR_TASK_ALL_DONE)
                                                             : tr(STR_TASK_UP_TO_DATE);
  const Rect subHeader{0, header.y + header.height, renderer.getScreenWidth(), metrics.tabBarHeight};
  GUI.drawSubHeader(renderer, subHeader, "", status);
}

void TaskListActivity::drawFooter() {
  // Confirmer dit ce qu'il fera sur la ligne selectionnee ; Droite ne s'affiche
  // que sur une tache, seule ligne qui a un detail (une case vide est effacee
  // par drawButtonHints plutot que laissee a un libelle qui ne ferait rien).
  // Gauche dit "ajouter", sauf sur la ligne d'ajout elle-meme, ou Confirmer le
  // dit deja : deux fois le meme libelle se lisait comme un bogue (Gauche
  // ajoute quand meme si on l'appuie). Tous mesures a moins de 80 px en
  // inter_8_regular, la case du pire theme (.claude/CONTEXT.md).
  const int selected = activeNav().selected;
  const TaskRowKind kind = selected >= 0 && selected < static_cast<int>(rows.size())
                               ? rows[static_cast<size_t>(selected)].kind
                               : TaskRowKind::Task;
  const char* confirmLabel = tr(STR_TASK_TICK);
  const char* rightLabel = tr(STR_TASK_DETAIL);
  const char* leftLabel = tr(STR_TASK_NEW_SHORT);
  if (kind == TaskRowKind::DoneSection) {
    confirmLabel = showDone ? tr(STR_TASK_COLLAPSE) : tr(STR_TASK_EXPAND);
    rightLabel = "";
  } else if (kind == TaskRowKind::AddTask) {
    confirmLabel = tr(STR_TASK_NEW_SHORT);
    leftLabel = "";
    rightLabel = "";
  }
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel, leftLabel, rightLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
