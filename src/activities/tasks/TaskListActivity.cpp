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

// Seuil de l'appui long sur Confirmer, qui ouvre la creation. Meme valeur que
// LibraryListActivity (LibraryListActivity.cpp:43) : le maintien doit avoir la
// meme duree d'un ecran de liste a l'autre, sinon la main apprend deux gestes.
constexpr unsigned long CREATE_HOLD_MS = 800;

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
  doneHeaderLabel.clear();
  doneHeaderLabel.shrink_to_fit();
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

  if (rows.empty()) {
    // Aucune ligne a l'ecran ne veut PAS dire aucune tache. Quand tout est fait
    // et la section repliee, buildTaskListRows() ne produit rien du tout
    // (TaskListModel.cpp:62 : l'en-tete et les faites n'apparaissent que si
    // showDone), alors que l'index en contient peut-etre vingt. Afficher
    // "aucune tache" a ce moment-la n'est pas seulement pauvre, c'est faux — et
    // faux precisement quand la fonction vient de faire son travail. Le pied de
    // page porte deja "N faites", qui dit comment les revoir.
    const char* message = TASK_STORE.all().empty() ? tr(STR_TASK_EMPTY) : tr(STR_TASK_ALL_DONE);
    screen.centeredText(message, screen.theme().bodyText);
    return;
  }
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
  configureUiListSectionHeaders(props, screen.theme());
  syncListViewport(screen, props, /*hasSubtitle=*/false);

  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1);
  if (winItems.capacity() < cap) winItems.reserve(cap);
  winItems.clear();

  const int windowStart = static_cast<int>(props.topIndex);
  int built = 0;
  for (int i = windowStart; i < count && built < static_cast<int>(cap); ++i) {
    const TaskListRow& row = rows[static_cast<size_t>(i)];
    fui::ListItem item;
    if (row.kind == TaskRowKind::DoneHeader) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%d %s", doneCount, tr(STR_TASK_DONE_COUNT));
      doneHeaderLabel = buf;
      item.isHeader = true;
      item.label = doneHeaderLabel.c_str();
    } else {
      const TaskRecord& rec = records[static_cast<size_t>(row.recordIndex)];
      item.label = rec.title;
      item.actionValue = static_cast<int16_t>(i);
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
      // marque, pas la regle.
      item.emphasis = (rec.priority == TASK_PRIORITY_HIGH);
      // Faite ou cochee pendant cette visite (tickedHere maintient rec.done
      // vrai dans les deux cas) : attenuee plutot que barree. list() ne rend
      // que effectiveTop/drawnRows agreges, jamais une position Y par ligne,
      // donc superposer un trait de barre demanderait de recalculer cette
      // geometrie nous-memes -- ce que StateDisabled evite.
      if (rec.done) {
        item.state = fui::StateDisabled;
      }
    }
    winItems.push_back(item);
    ++built;
  }

  props.items = winItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(winItems.size());
  screen.list(props);
}

void TaskListActivity::activateIndex(int index) { toggleAt(index); }

bool TaskListActivity::handleCustomInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    openDetailAt(activeNav().selected);
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    // Seul bouton avant restant une fois Retour/Confirmer/Droite pris (voir
    // le rapport) : bascule la section "terminees".
    showDone = !showDone;
    dirty = true;
    rebuildOrder();
    const int next = taskListNormalizeSelection(rows, activeNav().selected);
    if (next >= 0) moveSelectionTo(next);
    return true;
  }
  // Gauche/Droite appartiennent aussi aux ensembles precedent/suivant de
  // ButtonNavigator (getPreviousButtons/getNextButtons). Sans cette
  // consommation, MAINTENIR "n faites" ferait defiler la liste page par page via
  // navigateButtons() avant que le relachement ne bascule la section — et,
  // parce que la release est consommee ici, ButtonNavigator::lastContinuousNavTime
  // ne serait jamais remis a zero, ce qui avalerait en silence l'appui
  // Haut/Bas suivant. Sur CET ecran ces deux boutons veulent dire "cocher la
  // section" et "detail", jamais page precedente/suivante. Correction locale :
  // ni UiListActivity ni LibraryListActivity ne sont touches.
  if (mappedInput.isPressed(MappedInputManager::Button::Left) ||
      mappedInput.isPressed(MappedInputManager::Button::Right)) {
    return true;
  }
  return false;
}

bool TaskListActivity::handleButtons() {
  // Retour : a l'identique de la base (UiListActivity.cpp:46-49). Il est recopie
  // plutot que delegue parce que le Confirmer de la base, lui, ne peut PAS
  // rester : il active sur le front d'appui.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }

  // Confirmer au RELACHEMENT, pas a l'appui. La base active sur le front
  // d'appui (UiListActivity.cpp:50-54) : la tache serait deja cochee, et une op
  // Done deja en file, avant meme que le maintien ait atteint son seuil. C'est
  // la duree du maintien qui separe les deux gestes, donc seul le relachement
  // peut trancher. Meme decoupe que LibraryListActivity::handleButtons().
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Liste vide : l'appui COURT cree, et drawFooter() l'annonce. Il n'y a
    // aucune ligne a cocher, donc le bouton fait la seule chose utile
    // disponible — et c'est exactement l'ecran ou quelqu'un qui vient
    // d'appairer un appareil vide chercherait comment ajouter une tache, sans
    // quoi il conclurait que la fonction est cassee. Des qu'il y a des lignes,
    // le bouton redevient "cocher" et la creation repasse a l'appui long, sans
    // etre annoncee : l'action est decouverte la ou elle manque, et nulle part
    // ailleurs.
    if (rows.empty() || mappedInput.getHeldTime() >= CREATE_HOLD_MS) {
      createTask();
      return true;
    }
    const int selected = activeNav().selected;
    if (selected >= 0 && selected < listCount()) activateIndex(selected);
    return true;
  }
  return false;
}

void TaskListActivity::navigateButtons() {
  auto& n = activeNav();
  buttonNavigator.onNextRelease([this, &n] {
    const int next = taskListStepSelection(rows, n.selected, +1);
    if (next >= 0) moveSelectionTo(next);
  });
  buttonNavigator.onPreviousRelease([this, &n] {
    const int prev = taskListStepSelection(rows, n.selected, -1);
    if (prev >= 0) moveSelectionTo(prev);
  });
  // Saut de page a l'appui long : taskListPageJump() partage l'arithmetique de
  // ButtonNavigator (util/PageIndex.h) puis corrige le resultat par-dessus la
  // ligne d'en-tete "terminees", que rien cote ButtonNavigator ne sait eviter
  // -- compose et teste a l'hote dans TaskListModel, puisque cette activite ne
  // l'est pas elle-meme. Sans ce saut, un appui long sur 120 taches n'aurait
  // plus que le pas-a-pas pour traverser la liste.
  buttonNavigator.onNextContinuous([this, &n] {
    const int jumped = taskListPageJump(rows, n.selected, n.pageRows(), +1);
    if (jumped >= 0) moveSelectionTo(jumped);
  });
  buttonNavigator.onPreviousContinuous([this, &n] {
    const int jumped = taskListPageJump(rows, n.selected, n.pageRows(), -1);
    if (jumped >= 0) moveSelectionTo(jumped);
  });
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
  const int next = taskListNormalizeSelection(rows, activeNav().selected);
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
                           // La tache a pu changer de place (priorite) ou quitter l'ecran : sans
                           // cette renormalisation, `selected` designerait une ligne disparue.
                           const int next = taskListNormalizeSelection(rows, activeNav().selected);
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
        const int next = created >= 0 ? created : taskListNormalizeSelection(rows, activeNav().selected);
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
  // Retour / cocher / detail / bascule "terminees" (voir le rapport).
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
  const char* status = notice == StatusNotice::ListFull      ? tr(STR_TASK_LIST_FULL)
                       : notice == StatusNotice::WriteFailed ? tr(STR_TASK_TICK_FAILED)
                       : TASK_STORE.hasSecret()              ? tr(STR_TASK_UP_TO_DATE)
                                                             : tr(STR_TASK_PAIRING_REQUIRED);
  const Rect subHeader{0, header.y + header.height, renderer.getScreenWidth(), metrics.tabBarHeight};
  GUI.drawSubHeader(renderer, subHeader, "", status);
}

void TaskListActivity::drawFooter() {
  // Meme forme que la ligne d'en-tete de la section qu'il ouvre ("3 faites"),
  // et aucun glyphe decoratif : les polices integrees s'arretent au Latin-1
  // plus une petite serie maths/monnaies, et un glyphe absent est saute en
  // silence par EpdFont.cpp. Le "☑" (U+2611) n'y figure pas — ce libelle
  // s'affichait donc " 3". Voir .claude/CONTEXT.md.
  char doneLabel[24];
  std::snprintf(doneLabel, sizeof(doneLabel), "%d %s", doneCount, tr(STR_TASK_DONE_COUNT));
  // Sur une liste vide, Confirmer cree au lieu de cocher (voir handleButtons())
  // et le libelle le dit. C'est le seul endroit ou la creation est annoncee.
  // STR_TASK_NEW_SHORT et non STR_TASK_NEW : la case ne fait que 106 px de
  // large (BaseTheme.cpp:174) et "Nouvelle tache" en mesure 145 dans la police
  // d'interface, ce qui deborderait sur les cases voisines — le titre long
  // reste au clavier, qui a la place.
  const char* confirmLabel = rows.empty() ? tr(STR_TASK_NEW_SHORT) : tr(STR_TASK_TICK);
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel, doneLabel, tr(STR_TASK_DETAIL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
