#include "TaskListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "TaskStore.h"
#include "activities/home/BookActions.h"
#include "activities/tasks/TaskDetailActivity.h"
#include "activities/tasks/TaskPriorityChoices.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "tasks/TaskOpQueue.h"

namespace fui = freeink::ui;

namespace {
constexpr char TAG[] = "TASKLIST";

// Pastilles circulaires de la maquette. 20x20, 1 bit par pixel, row-major
// MSB-first (lignes arrondies a l'octet : 3 octets, 4 bits de bourrage), bit a 1 = encre : c'est le contrat de
// fui::BitmapRef/BW1, que FreeInkUIGfxRenderer::bitmap() echantillonne via forEachBitmapPixel() puis pose avec
// drawPixel(). Ce n'est PAS le contrat pre-tourne de GfxRenderer::drawIcon (cf. la note de rotation de
// .claude/CONTEXT.md) : rien ici ne doit etre stocke tourne — et un disque est de toute facon invariant par rotation.
//
// `static const` donc en flash (.rodata), pas en DRAM : regle de ressources 3
// du CLAUDE.md. Le champ BitmapRef::progmem n'est lu nulle part dans le SDK
// (verifie : il n'apparait qu'a sa declaration) et sur ESP32 la flash est
// mappee en lecture directe, donc le dereferencement de `data` est sans
// danger — aucun pgm_read_byte necessaire, contrairement a l'AVR.
constexpr int16_t TASK_BULLET_PX = 20;

// Anneau ouvert : tache pas encore faite.
static const uint8_t TASK_BULLET_OPEN[] = {
    0x00, 0x00, 0x00, 0x03, 0xfc, 0x00, 0x0f, 0xff, 0x00, 0x1e, 0x07, 0x80, 0x38, 0x01, 0xc0,
    0x30, 0x00, 0xc0, 0x70, 0x00, 0xe0, 0x60, 0x00, 0x60, 0x60, 0x00, 0x60, 0x60, 0x00, 0x60,
    0x60, 0x00, 0x60, 0x60, 0x00, 0x60, 0x60, 0x00, 0x60, 0x70, 0x00, 0xe0, 0x30, 0x00, 0xc0,
    0x38, 0x01, 0xc0, 0x1e, 0x07, 0x80, 0x0f, 0xff, 0x00, 0x03, 0xfc, 0x00, 0x00, 0x00, 0x00,
};

// Disque plein : tache faite.
static const uint8_t TASK_BULLET_DONE[] = {
    0x00, 0x00, 0x00, 0x03, 0xfc, 0x00, 0x0f, 0xff, 0x00, 0x1f, 0xff, 0x80, 0x3f, 0xff, 0xc0,
    0x3f, 0xff, 0xc0, 0x7f, 0xff, 0xe0, 0x7f, 0xff, 0xe0, 0x7f, 0xff, 0xe0, 0x7f, 0xff, 0xe0,
    0x7f, 0xff, 0xe0, 0x7f, 0xff, 0xe0, 0x7f, 0xff, 0xe0, 0x7f, 0xff, 0xe0, 0x3f, 0xff, 0xc0,
    0x3f, 0xff, 0xc0, 0x1f, 0xff, 0x80, 0x0f, 0xff, 0x00, 0x03, 0xfc, 0x00, 0x00, 0x00, 0x00,
};

// Marque d'etat de la ligne "N faites" : U+203A replie, U+00BB deplie. Toutes
// deux presentes dans les polices integrees (scripts/measure_label.py) ; la
// maquette montrait U+25B8, absent, qui aurait ete saute en silence. Aucun
// triangle vers le bas n'y figure, d'ou deux chevrons plutot que la paire
// droite/bas habituelle — le pied de page dit deplier/replier en toutes lettres.
constexpr char DONE_ROW_COLLAPSED_MARK[] = "\u203A";
constexpr char DONE_ROW_EXPANDED_MARK[] = "\u00BB";

// Seuil de l'appui long sur Confirmer : celui de la Library, pour que le geste
// se ressente pareil d'un ecran a l'autre.
constexpr unsigned long kHoldMs = 800;

fui::BitmapRef taskBullet(const bool done) {
  fui::BitmapRef ref;
  ref.data = done ? TASK_BULLET_DONE : TASK_BULLET_OPEN;
  ref.width = TASK_BULLET_PX;
  ref.height = TASK_BULLET_PX;
  ref.format = fui::BitmapFormat::BW1;
  return ref;
}
}  // namespace

int TaskListActivity::taskListFontId() {
  // Tailles fixes 10 / 12 / 14. Les polices d'interface s'arretent a l'Inter
  // 12 : la 14 est la Lexend Deca integree, celle des notes de l'ecran de
  // detail, avec son gras (taches prioritaires).
  switch (SETTINGS.taskFontSize) {
    case 1:
      return UI_12_FONT_ID;
    case 2:
      return LEXENDDECA_14_FONT_ID;
    default:
      return UI_10_FONT_ID;
  }
}

TaskListActivity::TaskListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    // Appui long tactile : les lignes portent InputLongPress (buildRows), qui
    // ouvre le menu de la tache comme l'appui long sur Confirmer.
    : UiListActivity("TaskList", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

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
  pendingOps = TASK_STORE.hasPendingOps();

  // Reglage « Taille du texte des taches » : la liste seule change de police,
  // en reliant l'emplacement BODY de CET ecran (meme mecanisme que
  // LibraryListActivity pour SMALL) ; en-tete, pied et popup gardent la leur.
  // Relu a chaque entree : un reglage change entre deux visites s'applique.
  listFontId = taskListFontId();
  uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, listFontId);

  showDone = false;
  task_keep_awake::active = false;
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
  order.clear();
  order.shrink_to_fit();
  rows.clear();
  rows.shrink_to_fit();
  winItems.clear();
  winItems.shrink_to_fit();
  doneRowLabel.clear();
  doneRowLabel.shrink_to_fit();
  // L'index lui-meme appartient au store, pas a l'activite : sans cet appel
  // ses ~26 Ko, charges par onEnter(), survivraient a l'ecran pour toute la
  // session. Cet ecran est le seul detenteur d'indices dedans, et ils viennent
  // d'etre liberes ci-dessus.
  TASK_STORE.unload();
  Activity::onExit();
}

void TaskListActivity::rebuildOrder() {
  if (!dirty) return;
  const std::vector<TaskRecord>& records = TASK_STORE.all();
  buildTaskOrder(records, order, openCount);
  buildTaskListRows(order, openCount, showDone, rows);
  doneCount = static_cast<int>(records.size()) - openCount;
  dirty = false;
}

int TaskListActivity::listCount() const { return static_cast<int>(rows.size()); }

void TaskListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Contenu sous l'en-tete, au-dessus des indices de boutons -- ces deux
  // bandes sont peintes hors fui, dans drawChrome() /
  // drawFooter(), donc reservees ici pour que la liste ne les recouvre pas
  // (meme rituel que LibraryListActivity::buildScreen).
  // En espacement « aere », un peu d'air aussi entre le filet de l'en-tete et
  // la premiere tache, pour qu'elle ne paraisse pas plus serree que les autres.
  const int16_t airTop =
      SETTINGS.taskRowSpacing == 2 ? static_cast<int16_t>(UiThemeTokensDetail::scaledListMetric(8)) : 0;
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) + airTop), 0,
      static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});

  // Jamais vide : un index vide donne la ligne d'ajout, un index tout fait la
  // ligne "N faites".
  buildRows(screen);
}

void TaskListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();
  const std::vector<TaskRecord>& records = TASK_STORE.all();

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  // Les titres peuvent s'enrouler sur deux lignes plutot que d'etre tronques
  // -- une exigence de la maquette, pas une simplification.
  props.labelText.maxLines = 2;
  // Taille native des pastilles : list() les pose en BitmapMode::Contain, donc
  // une iconSize differente les ferait redimensionner au plus proche voisin et
  // l'anneau prendrait une epaisseur irreguliere sur un panneau 1 bit.
  props.iconSize = TASK_BULLET_PX;
  // Les taches se rapprochent du bord gauche de kLeftShiftPx sans s'y coller.
  // Pris d'abord sur le retrait des lignes (Lyra : 20 px), pour que le texte
  // garde son air a l'interieur du surlignage de selection ; a defaut (themes
  // sans retrait), sur la marge interne, dont on garde au moins 6 px.
  constexpr int16_t kLeftShiftPx = 8;
  const fui::ThemeTokens& tokens = screen.theme();
  if (tokens.listInset >= kLeftShiftPx) {
    props.rowInset = static_cast<int16_t>(tokens.listInset - kLeftShiftPx);
  } else {
    props.sidePadding = std::max<int16_t>(6, static_cast<int16_t>(tokens.listSidePadding - kLeftShiftPx));
  }
  // Reglage « Espacement des taches » : un ecart uniforme entre les lignes,
  // quelle que soit leur hauteur (voir syncListViewport). Echelonne comme la
  // hauteur de base (scaledListMetric) pour garder la proportion en grande
  // taille d'UI.
  static constexpr int kSpacingPx[] = {0, 10, 20};
  const uint8_t spacing = SETTINGS.taskRowSpacing < 3 ? SETTINGS.taskRowSpacing : 0;
  // Une police plus grande (ou plus petite) decale la hauteur de ligne d'autant
  // que sa hauteur de ligne differe de la police de base : meme air au-dessus
  // et au-dessous du texte qu'a la taille normale.
  const int16_t fontDelta =
      static_cast<int16_t>(renderer.getLineHeight(listFontId) - renderer.getLineHeight(uiScaleSpec().bodyFontId));
  syncListViewport(screen, props, /*hasSubtitle=*/false, UiThemeTokensDetail::scaledListMetric(kSpacingPx[spacing]),
                   fontDelta);

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
      // marque, pas la regle. Une tache faite cesse de crier : le gras ne vaut que pour une tache ouverte.
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

void TaskListActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
  // Le message se pose sur la liste deja affichee, comme les toasts de la
  // Library ; handleCustomInput() l'efface apres kNoticeMs.
  if (notice != StatusNotice::None) BookActions::drawToast(renderer, noticeText());
}

const char* TaskListActivity::noticeText() const {
  switch (notice) {
    case StatusNotice::ListFull:
      return tr(STR_TASK_LIST_FULL);
    case StatusNotice::WriteFailed:
      return tr(STR_TASK_TICK_FAILED);
    case StatusNotice::LowMemory:
      return tr(STR_TASK_SYNC_LOW_MEMORY);
    case StatusNotice::None:
      break;
  }
  return "";
}

void TaskListActivity::showNotice(const StatusNotice next) {
  notice = next;
  noticeSince = millis();
  requestUpdate();
}

bool TaskListActivity::handleCustomInput() {
  if (notice != StatusNotice::None && millis() - noticeSince >= kNoticeMs) {
    notice = StatusNotice::None;
    requestUpdate();
  }
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
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

bool TaskListActivity::handleButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int selected = activeNav().selected;
    if (selected < 0 || selected >= listCount()) return true;
    // Sur « + Ajouter » et « N faites », un appui long fait l'action normale :
    // le menu ne concerne qu'une tache.
    if (mappedInput.getHeldTime() >= kHoldMs && rows[static_cast<size_t>(selected)].kind == TaskRowKind::Task) {
      openTaskMenu(selected);
    } else {
      activateIndex(selected);
    }
    return true;
  }
  return false;
}

void TaskListActivity::onRowLongPress(const int index) { openTaskMenu(index); }

void TaskListActivity::openTaskMenu(const int index) {
  if (index < 0 || index >= static_cast<int>(rows.size()) ||
      rows[static_cast<size_t>(index)].kind != TaskRowKind::Task) {
    return;
  }
  const TaskRecord& rec = TASK_STORE.all()[static_cast<size_t>(rows[static_cast<size_t>(index)].recordIndex)];
  const std::string id = rec.id;
  // L'ordre du menu tient dans ce seul tableau : le rappel dispatche sur
  // l'action, jamais sur un indice de ligne, donc deplacer une entree ne
  // decale pas les autres. Supprimer n'est jamais la selection par defaut ;
  // Synchroniser ferme la liste, qui redemarre l'appareil : en bas, a part.
  enum class MenuAction : uint8_t { Tick, View, Edit, KeepAwake, TextSize, Delete, Sync };
  static constexpr MenuAction kMenu[] = {MenuAction::Tick,      MenuAction::View,     MenuAction::Edit,
                                         MenuAction::KeepAwake, MenuAction::TextSize, MenuAction::Delete,
                                         MenuAction::Sync};
  std::vector<std::string> options;
  options.reserve(sizeof(kMenu) / sizeof(kMenu[0]));
  for (const MenuAction action : kMenu) {
    switch (action) {
      case MenuAction::Tick:
        options.emplace_back(rec.done ? tr(STR_TASK_MENU_REOPEN) : tr(STR_TASK_MENU_TICK));
        break;
      case MenuAction::View:
        options.emplace_back(tr(STR_TASK_MENU_VIEW));
        break;
      case MenuAction::Edit:
        options.emplace_back(tr(STR_TASK_MENU_EDIT));
        break;
      case MenuAction::KeepAwake:
        options.emplace_back(task_keep_awake::active ? tr(STR_TASK_MENU_ALLOW_SLEEP) : tr(STR_TASK_MENU_KEEP_AWAKE));
        break;
      case MenuAction::TextSize:
        options.emplace_back(tr(STR_TASK_FONT_SIZE));
        break;
      case MenuAction::Delete:
        options.emplace_back(tr(STR_DELETE));
        break;
      case MenuAction::Sync:
        options.emplace_back(tr(STR_TASK_SYNC));
        break;
    }
  }
  app.clearTapFlash();
  popup.show(rec.title, options, 0, [this, id](const int choice) {
    if (choice < 0 || static_cast<size_t>(choice) >= sizeof(kMenu) / sizeof(kMenu[0])) return;
    const int row = rowOfTask(id.c_str());
    switch (kMenu[choice]) {
      case MenuAction::Tick:
        toggleAt(row);
        break;
      case MenuAction::View:
        openDetailAt(row);
        break;
      case MenuAction::Edit:
        editTaskTitle(id);
        break;
      case MenuAction::KeepAwake:
        // Pour la visite seulement : onEnter() le remet a faux, donc quitter la
        // liste rend la veille, et la batterie ne paie jamais un oubli.
        task_keep_awake::active = !task_keep_awake::active;
        break;
      case MenuAction::TextSize:
        chooseTextSize();
        break;
      case MenuAction::Delete:
        promptDeleteTask(id);
        break;
      case MenuAction::Sync:
        // Chaque op est deja sur la carte (appendOp ecrit a chaque geste) :
        // redemarrer maintenant ne perd rien. Retour a cette liste ensuite.
        silentRestartToNetwork(NetworkBootTarget::TASK_SYNC, TASK_SYNC_RETURN_TO_LIST);
        break;
    }
  });
  requestUpdate();
}

void TaskListActivity::chooseTextSize() {
  // Le meme reglage que Parametres > Systeme > Taches, choisi sans quitter la
  // liste : il s'applique au retour et s'enregistre comme depuis les Parametres.
  std::vector<std::string> labels{tr(STR_TASK_FONT_10), tr(STR_TASK_FONT_12), tr(STR_TASK_FONT_14)};
  const int current = SETTINGS.taskFontSize < labels.size() ? SETTINGS.taskFontSize : 0;
  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "TaskFontSize", StrId::STR_TASK_FONT_SIZE,
                                                std::move(labels), current),
      [this](const ActivityResult& result) {
        const auto* choice = std::get_if<OptionSelectionResult>(&result.data);
        if (!result.isCancelled && choice != nullptr && choice->index >= 0 && choice->index < 3 &&
            choice->index != SETTINGS.taskFontSize) {
          SETTINGS.taskFontSize = static_cast<uint8_t>(choice->index);
          if (!SETTINGS.saveGlobalDefaults()) {
            LOG_ERR(TAG, "Could not save the task text size");
            showNotice(StatusNotice::WriteFailed);
          }
          // La tache de rendu lit les polices de uiTarget : la
          // rebrancher sous le verrou, jamais pendant un rendu.
          RenderLock lock(*this);
          listFontId = taskListFontId();
          uiTarget.setFont(fui::GfxRendererTarget::FONT_BODY, listFontId);
        }
        requestUpdate(true);
      });
}

void TaskListActivity::editTaskTitle(const std::string& id) {
  const TaskRecord* rec = TASK_STORE.find(id.c_str());
  if (rec == nullptr) return;
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TASK_EDIT_TITLE), rec->title,
                                              TASK_TITLE_MAX, InputType::Text, /*minLength=*/1),
      [this, id](const ActivityResult& result) {
        const auto* entered = std::get_if<KeyboardResult>(&result.data);
        // Annuler le titre annule toute la modification, comme sur l'ecran de
        // detail : la priorite n'est pas une seconde chance apres un retour.
        if (result.isCancelled || entered == nullptr) return;
        const TaskRecord* current = TASK_STORE.find(id.c_str());
        if (current == nullptr) return;

        if (std::strcmp(entered->text.c_str(), current->title) != 0) {
          TaskRecord next = *current;
          std::snprintf(next.title, sizeof(next.title), "%s", entered->text.c_str());
          TaskOp op{};
          op.kind = TaskOpKind::Title;
          std::snprintf(op.id, sizeof(op.id), "%s", next.id);
          std::snprintf(op.title, sizeof(op.title), "%s", next.title);
          // La file avant l'index, jamais l'inverse (voir toggleAt()).
          if (!TASK_STORE.appendOp(op)) {
            LOG_ERR(TAG, "Could not queue the title edit for %s; nothing applied", next.id);
            showNotice(StatusNotice::WriteFailed);
            return;
          }
          pendingOps = true;
          if (!TASK_STORE.upsert(next)) showNotice(StatusNotice::WriteFailed);
          refreshAfterEdit(id.c_str());
        }
        editTaskPriority(id);
      });
}

void TaskListActivity::editTaskPriority(const std::string& id) {
  const TaskRecord* rec = TASK_STORE.find(id.c_str());
  if (rec == nullptr) return;
  std::vector<std::string> labels;
  labels.reserve(TASK_PRIORITY_CHOICE_COUNT);
  uint8_t selected = TASK_DEFAULT_PRIORITY_CHOICE;
  for (size_t i = 0; i < TASK_PRIORITY_CHOICE_COUNT; i++) {
    labels.emplace_back(I18N.get(taskPriorityLabelId(TASK_PRIORITY_CHOICES[i])));
    if (TASK_PRIORITY_CHOICES[i] == rec->priority) selected = static_cast<uint8_t>(i);
  }

  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "TaskPriority", StrId::STR_TASK_PRIORITY,
                                                std::move(labels), selected),
      [this, id](const ActivityResult& result) {
        const auto* choice = std::get_if<OptionSelectionResult>(&result.data);
        if (result.isCancelled || choice == nullptr ||
            static_cast<size_t>(choice->index) >= TASK_PRIORITY_CHOICE_COUNT) {
          return;
        }
        const TaskRecord* current = TASK_STORE.find(id.c_str());
        if (current == nullptr) return;
        const uint8_t priority = TASK_PRIORITY_CHOICES[choice->index];
        if (priority == current->priority) return;

        TaskRecord next = *current;
        next.priority = priority;
        // Le serveur la range au bout de son nouveau groupe ; d'ici la sync,
        // elle s'y range aussi.
        next.order = TASK_ORDER_UNSET;
        TaskOp op{};
        op.kind = TaskOpKind::Prio;
        std::snprintf(op.id, sizeof(op.id), "%s", next.id);
        op.priority = priority;
        if (!TASK_STORE.appendOp(op)) {
          LOG_ERR(TAG, "Could not queue the priority edit for %s; nothing applied", next.id);
          showNotice(StatusNotice::WriteFailed);
          return;
        }
        pendingOps = true;
        if (!TASK_STORE.upsert(next)) showNotice(StatusNotice::WriteFailed);
        refreshAfterEdit(id.c_str());
      });
}

void TaskListActivity::refreshAfterEdit(const char* id) {
  dirty = true;
  rebuildOrder();
  const int moved = rowOfTask(id);
  const int next = moved >= 0 ? moved : taskListClampSelection(rows, activeNav().selected);
  if (next >= 0) moveSelectionTo(next);
  requestUpdate();
}

void TaskListActivity::promptDeleteTask(const std::string& id) {
  const TaskRecord* rec = TASK_STORE.find(id.c_str());
  if (rec == nullptr) return;
  // Meme en-tete que la suppression d'un livre dans la Library.
  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, rec->title),
                         [this, id](const ActivityResult& result) {
                           if (!result.isCancelled) deleteTask(id);
                           requestUpdate(true);
                         });
}

void TaskListActivity::deleteTask(const std::string& id) {
  TaskOp op{};
  op.kind = TaskOpKind::Del;
  std::snprintf(op.id, sizeof(op.id), "%s", id.c_str());
  // La file avant l'index : une coupure entre les deux laisse une op en file
  // pour une tache encore affichee, que la sync suivante supprime partout. Dans
  // l'autre ordre, la tache disparaitrait ici et reviendrait a la sync.
  if (!TASK_STORE.appendOp(op)) {
    LOG_ERR(TAG, "Could not queue the delete op for %s; task kept", id.c_str());
    showNotice(StatusNotice::WriteFailed);
    return;
  }
  pendingOps = true;

  {
    // erase() decale les enregistrements suivants : `rows` et `order`
    // indexent ce vecteur, donc un rendu entre l'effacement et la
    // reconstruction lirait des indices faux, voire hors limites. Les deux
    // sous le meme verrou ; l'ecriture SD reste dehors (voir
    // TaskDetailActivity::appendAndApply pour ce choix).
    RenderLock lock(*this);
    TASK_STORE.stageRemove(id.c_str());
    dirty = true;
    rebuildOrder();
  }
  // La note eventuelle reste sur la carte jusqu'a la sync suivante :
  // commitStagedNotes() y supprime toute note dont la tache n'est plus
  // indexee. Seules les taches venues du serveur en ont une.
  if (!TASK_STORE.saveIndex()) showNotice(StatusNotice::WriteFailed);
  const int next = taskListClampSelection(rows, activeNav().selected);
  if (next >= 0) moveSelectionTo(next);
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
    // reste a le dire a l'ecran (toast), pas seulement au port serie.
    LOG_ERR(TAG, "Echec de l'enregistrement de la bascule 'fait' pour %s", rec.id);
    showNotice(StatusNotice::WriteFailed);
    return;
  }
  pendingOps = true;

  const std::string id(rec.id);

  // Un index non ecrit se dit : la RAM porte la coche, la carte non, et une
  // reouverture la montrerait decochee jusqu'a la prochaine sync.
  if (!TASK_STORE.upsert(rec)) showNotice(StatusNotice::WriteFailed);

  dirty = true;
  rebuildOrder();
  // La selection suit la TACHE, pas l'indice. Decocher une tache de la section
  // depliee la fait remonter parmi les ouvertes, et "N faites" glisse a son
  // ancien indice : garder l'indice ferait replier la section au Confirmer
  // suivant, au lieu d'agir sur la tache qu'on vient de toucher.
  //
  // Sauf pour une coche : la tache coule dans la section terminees, et la
  // selection reste a son indice, donc sur la tache qui a pris sa place. On
  // coche ainsi une liste de haut en bas sans jamais deplacer le curseur.
  const int moved = rec.done ? -1 : rowOfTask(id.c_str());
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

  startActivityForResult(std::make_unique<TaskDetailActivity>(renderer, mappedInput, id.c_str()),
                         [this, id](const ActivityResult& result) {
                           // Rien ne rappelle onEnter() sur une activite depilee : ActivityManager
                           // la restaure par std::move et ne fait tourner que ce gestionnaire
                           // (chemin Pop). `order`, `rows`, `openCount` et `showDone`
                           // sont donc exactement ceux d'avant la visite — ce qu'on
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
                           // Le detail n'annonce un changement qu'apres une op ajoutee.
                           pendingOps = true;

                           dirty = true;
                           rebuildOrder();
                           // La tache a pu changer de place (priorite, coche) : la selection la
                           // suit. Si elle a quitte l'ecran (cochee, section repliee), le clamp
                           // garde un indice valide.
                           const int moved = rowOfTask(id.c_str());
                           const int next = moved >= 0 ? moved : taskListClampSelection(rows, activeNav().selected);
                           if (next >= 0) moveSelectionTo(next);
                           requestUpdate();
                         });
}

void TaskListActivity::createTask() {
  // Le plafond se verifie AVANT d'ouvrir le clavier, et non apres la saisie.
  // TaskStore::upsert() refuse l'enregistrement des que l'index est a
  // MAX_TASKS (stageUpsert()), alors que l'op Add serait deja en file : le
  // serveur accepterait une tache que l'appareil n'afficherait jamais jusqu'a
  // ce qu'une sync la ramene. Refuser d'entree est moins mauvais que cela.
  if (TASK_STORE.all().size() >= MAX_TASKS) {
    LOG_ERR(TAG, "Index already at the %zu-task cap; not opening the keyboard", MAX_TASKS);
    showNotice(StatusNotice::ListFull);
    return;
  }
  // La capacite aussi, AVANT le clavier : fromJson() a reserve le nombre exact
  // de taches chargees, donc l'ajout reallouerait le vecteur au double par un
  // `new` qui aborte, pendant que l'ancien bloc vit encore (voir
  // TaskStore::reserveFullCapacity, la meme garde que la sync). Apres une
  // lecture, le tas peut ne plus avoir ce bloc : on le dit plutot que de
  // redemarrer.
  // Sous RenderLock : reserveFullCapacity() realloue le vecteur de l'index et
  // libere l'ancien bloc, alors que la tache de rendu lit TASK_STORE.all() pour
  // dessiner cette meme liste. Sans le verrou, une image en cours pourrait lire
  // l'ancien bloc apres sa liberation.
  bool reserved;
  {
    RenderLock lock(*this);
    reserved = TASK_STORE.reserveFullCapacity();
  }
  if (!reserved) {
    showNotice(StatusNotice::LowMemory);
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
        // Pas encore classee : au bout de sa priorite jusqu'a la sync.
        rec.order = TASK_ORDER_UNSET;

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
          showNotice(StatusNotice::WriteFailed);
          return;
        }
        pendingOps = true;
        // Pas de saveIndex() explicite : upsert() l'appelle deja, et un second
        // appel reecrirait tout l'index une deuxieme fois par tache creee.
        if (!TASK_STORE.upsert(rec)) showNotice(StatusNotice::WriteFailed);

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

void TaskListActivity::drawChrome() {
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  char title[48];
  std::snprintf(title, sizeof(title), "%s (%d)", tr(STR_TASK_TITLE), openCount);
  // Une op attend le serveur : une fleche circulaire au bord droit de la ligne
  // du titre. Passee comme `subtitle`, que les themes placent a droite, alignee
  // sur la ligne de base du titre, dans la petite police (inter_8_regular porte
  // U+21BB, scripts/measure_label.py). Rien quand tout est envoye : l'etat
  // normal ne se signale pas.
  const char* pendingMark = pendingOps ? "\u21BB" : nullptr;
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, title, false, 0, pendingMark);
  } else {
    GUI.drawHeader(renderer, header, title, pendingMark);
  }
}

void TaskListActivity::drawFooter() {
  // Confirmer dit ce qu'il fera sur la ligne selectionnee ; Droite ne s'affiche
  // que sur une tache, seule ligne qui a un detail (une case vide est effacee
  // par drawButtonHints plutot que laissee a un libelle qui ne ferait rien).
  // Gauche dit "ajouter", sauf sur la ligne d'ajout elle-meme, ou Confirmer le
  // dit deja : deux fois le meme libelle se lisait comme un bogue (Gauche
  // ajoute quand meme si on l'appuie). Tous mesures a moins de ~70 px en
  // inter_8_regular, la case de 80 px du pire theme moins sa bordure
  // (.claude/CONTEXT.md).
  const int selected = activeNav().selected;
  const TaskRowKind kind = selected >= 0 && selected < static_cast<int>(rows.size())
                               ? rows[static_cast<size_t>(selected)].kind
                               : TaskRowKind::Task;
  // Sur une tache deja faite, Confirmer la decoche : le libelle le dit.
  const bool selectedDone = kind == TaskRowKind::Task && selected >= 0 && selected < static_cast<int>(rows.size()) &&
                            TASK_STORE.all()[static_cast<size_t>(rows[static_cast<size_t>(selected)].recordIndex)].done;
  const char* confirmLabel = selectedDone ? tr(STR_TASK_UNTICK) : tr(STR_TASK_TICK);
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
