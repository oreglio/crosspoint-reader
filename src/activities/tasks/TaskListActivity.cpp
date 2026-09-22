#include "TaskListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "TaskStore.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "tasks/TaskOpQueue.h"

namespace fui = freeink::ui;

namespace {
constexpr char TAG[] = "TASKLIST";
}  // namespace

TaskListActivity::TaskListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("TaskList", renderer, mappedInput, /*wantsTouchLongPress=*/false) {}

void TaskListActivity::onEnter() {
  // Un seul verrou sur le cycle de vie de base ET la phase de donnees : le
  // onEnter de base programme un rendu, et la tache de rendu ne doit pas lire
  // records/rows avant qu'ils soient en place (meme rituel que
  // LibraryListActivity::onEnter).
  RenderLock lock(*this);
  UiListActivity::onEnter();

  // Le store ne se charge pas tout seul au premier acces (getInstance() ne
  // fait rien lire) : c'est a l'ecran de le demander ici, jamais depuis le
  // rendu. Voir PersistableStore::ensureLoaded().
  TASK_STORE.ensureLoaded();

  records = TASK_STORE.all();
  tickedHere.clear();
  showDone = false;
  dirty = true;
  rebuildOrder();
}

void TaskListActivity::onExit() {
  // Rien ne doit rester resident pendant la lecture : clear() seul garde la
  // capacite reservee, shrink_to_fit() la rend vraiment.
  records.clear();
  records.shrink_to_fit();
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
  Activity::onExit();
}

void TaskListActivity::rebuildOrder() {
  if (!dirty) return;
  buildTaskOrder(records, tickedHere, order, openCount);
  buildTaskListRows(order, openCount, showDone, rows);
  doneCount = static_cast<int>(records.size()) - openCount;
  dirty = false;
}

int TaskListActivity::listCount() const { return static_cast<int>(rows.size()); }

void TaskListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Contenu sous l'en-tete + la sous-ligne de fraicheur, au-dessus des
  // indices de boutons -- ces deux bandes sont peintes hors fui, dans
  // render(), donc reservees ici pour que la liste ne les recouvre pas
  // (meme rituel que LibraryListActivity::buildScreen).
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) +
                                       metrics.tabBarHeight),
                  0, static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing), 0});

  if (rows.empty()) {
    screen.centeredText(tr(STR_TASK_EMPTY), screen.theme().bodyText);
    return;
  }
  buildRows(screen);
}

void TaskListActivity::buildRows(UiScreen& screen) {
  auto& nav = activeNav();
  const int count = listCount();

  fui::ListProps props;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  // Les titres peuvent s'enrouler sur deux lignes plutot que d'etre tronques
  // -- une exigence de la maquette, pas une simplification.
  props.labelText.maxLines = 2;
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
    moveSelectionTo(taskListNormalizeSelection(rows, activeNav().selected));
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
  // Saut de page a l'appui long : taskListPageJump() reproduit
  // ButtonNavigator::nextPageIndex/previousPageIndex puis corrige le
  // resultat par-dessus la ligne d'en-tete "terminees" (que rien cote
  // ButtonNavigator ne sait eviter) -- compose et teste a l'hote dans
  // TaskListModel, puisque cette activite ne l'est pas elle-meme. Sans ce
  // saut, un appui long sur 120 taches n'aurait plus que le pas-a-pas pour
  // traverser la liste.
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

  TaskRecord& rec = records[static_cast<size_t>(row.recordIndex)];
  rec.done = !rec.done;

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

  TaskOp op{};
  op.kind = TaskOpKind::Done;
  std::snprintf(op.id, sizeof(op.id), "%s", rec.id);
  op.done = rec.done;
  if (!TASK_STORE.appendOp(op)) {
    LOG_ERR(TAG, "Echec de l'enregistrement de la bascule 'fait' pour %s", rec.id);
  }

  dirty = true;
  rebuildOrder();
  moveSelectionTo(taskListNormalizeSelection(rows, activeNav().selected));
}

void TaskListActivity::openDetailAt(int index) {
  if (index < 0 || index >= static_cast<int>(rows.size()) ||
      rows[static_cast<size_t>(index)].kind != TaskRowKind::Task) {
    return;
  }
  // TaskDetailActivity arrive avec la Tache 6 ; ce bouton n'a pas encore
  // d'ecran a ouvrir.
  LOG_INF(TAG, "Detail demande pour %s (TaskDetailActivity : Tache 6)",
          records[static_cast<size_t>(rows[static_cast<size_t>(index)].recordIndex)].id);
}

void TaskListActivity::startSync() {
  // TaskSyncActivity arrive avec la Tache 9, qui devra aussi choisir quel
  // bouton la declenche : les boutons avant sont deja pris par
  // Retour / cocher / detail / bascule "terminees" (voir le rapport).
  LOG_INF(TAG, "Sync demandee (TaskSyncActivity : Tache 9)");
}

void TaskListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  char title[48];
  std::snprintf(title, sizeof(title), "%s (%d)", tr(STR_TASK_TITLE), openCount);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, title, false);
  } else {
    GUI.drawHeader(renderer, header, title);
  }

  // Fraicheur de sync : seule l'information qu'on a vraiment (appairee ou
  // non) est affichee ici. Un horodatage relatif ("il y a 2h") demanderait un
  // champ que rien n'ecrit encore -- voir le rapport.
  const int subHeaderTop = header.y + header.height;
  const char* freshness = TASK_STORE.hasSecret() ? tr(STR_TASK_UP_TO_DATE) : tr(STR_TASK_PAIRING_REQUIRED);
  const Rect subHeader{0, subHeaderTop, renderer.getScreenWidth(), metrics.tabBarHeight};
  GUI.drawSubHeader(renderer, subHeader, "", freshness);

  renderUi();
  // Lignes de hauteur variable (titres qui s'enroulent) : la premiere passe
  // peut mesurer moins de lignes que l'estimation a hauteur fixe. On rejoue
  // dans le meme cadre plutot que de laisser une selection hors-champ (meme
  // rituel que LibraryListActivity::render).
  for (int pass = 0; activeNav().consumeRebuildNeeded() && pass < 8; ++pass) {
    renderer.clearScreen();
    if (mappedInput.hasTouchHardware()) {
      TouchHeaderBackButton::draw(renderer, uiTarget, header, title, false);
    } else {
      GUI.drawHeader(renderer, header, title);
    }
    GUI.drawSubHeader(renderer, subHeader, "", freshness);
    renderUi();
  }

  char doneLabel[16];
  std::snprintf(doneLabel, sizeof(doneLabel), "\xE2\x98\x91 %d", doneCount);  // "[checkbox] N"
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_TASK_TICK), doneLabel, tr(STR_TASK_DETAIL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
