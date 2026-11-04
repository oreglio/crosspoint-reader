#include "TaskListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "TaskStore.h"
#include "activities/home/BookActions.h"
#include "activities/tasks/TaskDetailActivity.h"
#include "activities/tasks/TaskFontSizes.h"
#include "activities/tasks/TaskGroupChoices.h"
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

// Seuil de l'appui long sur Confirmer : celui de la Library, pour que le geste
// se ressente pareil d'un ecran a l'autre.
constexpr unsigned long kHoldMs = 800;

// Cases carrees du mode selection, meme format que les pastilles : la forme
// change pour que le mode se voie d'un coup d'oeil (un rond dit « faite », un
// carre dit « choisie »). Case vide : non choisie.
static const uint8_t TASK_BOX_EMPTY[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xff, 0xc0, 0x3f, 0xff, 0xc0, 0x30, 0x00, 0xc0,
    0x30, 0x00, 0xc0, 0x30, 0x00, 0xc0, 0x30, 0x00, 0xc0, 0x30, 0x00, 0xc0, 0x30, 0x00, 0xc0,
    0x30, 0x00, 0xc0, 0x30, 0x00, 0xc0, 0x30, 0x00, 0xc0, 0x30, 0x00, 0xc0, 0x30, 0x00, 0xc0,
    0x30, 0x00, 0xc0, 0x3f, 0xff, 0xc0, 0x3f, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Case pleine, coche en reserve blanche : choisie.
static const uint8_t TASK_BOX_CHECKED[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xff, 0xc0, 0x3f, 0xff, 0xc0, 0x3f, 0xff, 0xc0,
    0x3f, 0xff, 0xc0, 0x3f, 0xfc, 0xc0, 0x3f, 0xf8, 0xc0, 0x3f, 0xf1, 0xc0, 0x39, 0xe3, 0xc0,
    0x38, 0xc7, 0xc0, 0x38, 0x07, 0xc0, 0x3c, 0x0f, 0xc0, 0x3e, 0x1f, 0xc0, 0x3f, 0x3f, 0xc0,
    0x3f, 0xff, 0xc0, 0x3f, 0xff, 0xc0, 0x3f, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

fui::BitmapRef taskBitmap(const uint8_t* data) {
  fui::BitmapRef ref;
  ref.data = data;
  ref.width = TASK_BULLET_PX;
  ref.height = TASK_BULLET_PX;
  ref.format = fui::BitmapFormat::BW1;
  return ref;
}

fui::BitmapRef taskBullet(const bool done) { return taskBitmap(done ? TASK_BULLET_DONE : TASK_BULLET_OPEN); }

fui::BitmapRef taskBox(const bool picked) { return taskBitmap(picked ? TASK_BOX_CHECKED : TASK_BOX_EMPTY); }
}  // namespace

int TaskListActivity::taskListFontId() {
  // Tailles fixes 10 / 12 / grande. Les polices d'interface s'arretent a
  // l'Inter 12 : la grande est la Lexend Deca integree, avec son gras (taches
  // prioritaires) -- 14 ou 16 selon le build, voir TaskFontSizes.h.
  switch (SETTINGS.taskFontSize) {
    case 1:
      return UI_12_FONT_ID;
    case 2:
      return kLargeTaskFontId;
    default:
      return UI_10_FONT_ID;
  }
}

int TaskListActivity::groupTitleFontId() {
  // Un cran sous la police des taches quand ce cran a un gras : la petite
  // police d'interface (inter 8) n'en a pas, et un titre maigre ne se lirait
  // plus comme un titre. En taille 10, le titre garde donc la taille des
  // taches ; il s'en distingue par le gras, le filet et l'air au-dessus.
  switch (SETTINGS.taskFontSize) {
    case 1:
      return UI_10_FONT_ID;
    case 2:
      return UI_12_FONT_ID;
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
  uiTarget.setFont(fui::GfxRendererTarget::FONT_LABEL, groupTitleFontId());

  view = TaskListView::Open;
  headerFocused = false;
  selecting = false;
  selection.reset();
  selectionCount = 0;
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
  buildTaskOrder(records, TASK_STORE.groups(), order, openCount);
  doneCount = static_cast<int>(records.size()) - openCount;
  // La vue faites s'est videe (derniere decochee, faites supprimees) : retour
  // seul a la liste ouverte, plutot qu'une liste vide sans rien a y faire.
  if (view == TaskListView::Done && doneCount == 0) view = TaskListView::Open;
  // Plus de compteur, plus de focus dessus.
  if (!hasHeaderCounter()) headerFocused = false;
  buildTaskListRows(order, openCount, view, records, TASK_STORE.groups(), rows);
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

  // Jamais vide : une vue ouverte sans tache donne la ligne d'ajout, et la vue
  // faites n'existe que tant qu'il en reste une (rebuildOrder()).
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
  // Seuls les titres de groupe portent un sous-titre (blanc) : sa police
  // regle de combien leur ligne grandit (buildRows, drawGroupHeaders).
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.font = fui::GfxRendererTarget::FONT_LABEL;
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
  if (headerFocused) {
    // Le curseur est sur le compteur de l'en-tete : aucune ligne surlignee
    // (list() ne peint la selection que sur selectedIndex). La selection de
    // nav reste sur 0, ou la remettront Bas. Pas de suivi en attente : il
    // chercherait une ligne selectionnee jamais dessinee et redemanderait des
    // passes de reconstruction (ListNav::onListRendered).
    props.selectedIndex = -1;
    nav.followPending = false;
  }

  const size_t cap = static_cast<size_t>(nav.visibleRows > 0 ? nav.visibleRows : 1);
  if (winItems.capacity() < cap) winItems.reserve(cap);
  winItems.clear();

  const int windowStart = static_cast<int>(props.topIndex);
  int built = 0;
  for (int i = windowStart; i < count && built < static_cast<int>(cap); ++i) {
    const TaskListRow& row = rows[static_cast<size_t>(i)];
    fui::ListItem item;
    item.actionValue = static_cast<int16_t>(i);
    if (row.kind == TaskRowKind::AddTask) {
      // Index vide seulement (buildTaskListRows()) : sur une liste peuplee,
      // l'ajout passe par le bouton Gauche.
      item.label = tr(STR_TASK_ADD_ROW);
    } else if (row.kind == TaskRowKind::GroupHeader) {
      // Ligne sans libelle ni pastille : list() en pose le fond, la selection
      // et la zone tactile, drawGroupHeaders() y peint le titre. Le
      // sous-titre blanc ne s'affiche pas : il reserve une ligne de la police
      // des titres, dont list() grandit la ligne quand le libelle (vide) et
      // lui ne tiennent pas dans la hauteur d'une tache. C'est le seul moyen
      // d'agrandir UNE ligne sans toucher au SDK ; l'air gagne va au-dessus
      // du titre, cale en bas.
      item.subtitle = " ";
    } else {
      const TaskRecord& rec = records[static_cast<size_t>(row.recordIndex)];
      item.label = rec.title;
      // Pastille circulaire de la maquette : anneau ouvert / disque plein.
      // En mode selection, la case dit « choisie » a la place de la pastille.
      item.icon = selecting ? taskBox(selection.test(static_cast<size_t>(row.recordIndex))) : taskBullet(rec.done);
      // Priorite haute seulement : ListItem::emphasis est un booleen par
      // ligne (freeink-sdk, ajoute pour ce chantier -- fui::ListProps::
      // labelText ne portait qu'un TextStyle unique pour tout l'appel de
      // list(), jamais par ligne, ce qui rendait un "gras" par ligne
      // impossible sans segmenter l'appel par bande de priorite et donc
      // re-derive la pagination que ListNav possede deja). Deux paliers, pas
      // trois : la priorite basse ne se distingue que par sa place en fin de
      // tri (taskOrderBefore), pas par un style — l'exception (haute) se
      // marque, pas la regle. Une tache faite cesse de crier : le gras ne vaut que pour une tache ouverte.
      item.bold = (rec.priority == TASK_PRIORITY_HIGH) && !rec.done;
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
  drawGroupHeaders(screen, props);
}

// Le titre de groupe de la maquette (petite police grasse, compteur a droite,
// filet dessous, plus d'air au-dessus qu'entre deux taches) ne s'exprime pas
// avec un fui::ListItem : la police du libelle vaut pour toute la liste, le
// texte est toujours centre verticalement, et ListProps n'a pas de hauteur par
// ligne. ListItem::isHeader (police et filet propres) n'est ni selectionnable
// ni touchable, et le titre doit l'etre. Le titre garde donc une ligne de
// liste ordinaire, vide, et son contenu est peint ici, cale en BAS de la
// ligne : l'air libere passe au-dessus.
//
// La geometrie est celle de list() (list.h) : memes lignes depuis topIndex,
// mesurees par le meme fui::measureListRow, avec les valeurs que
// Screen::resolveListProps() substitue aux sentinelles que buildRows() laisse
// au theme. Aucune ligne n'a de sectionHeading ni d'isHeader, et l'ecran ne
// pose pas de ComponentStyle : list() recoit screen.body() tel quel.
void TaskListActivity::drawGroupHeaders(UiScreen& screen, const fui::ListProps& props) const {
  // Sans groupe (ou en vue Faites, sans titre) rien a peindre : on evite de
  // mesurer chaque ligne visible a chaque passe.
  if (TASK_STORE.groups().empty() || view == TaskListView::Done) return;
  if (props.count == 0 || winItems.empty()) return;
  const fui::ThemeTokens& theme = screen.theme();
  const bool touch = screen.device().hasTouch;

  // Copies sur la pile (~1 Ko avec `styles` plus bas ; la tache de rendu a
  // 16 Ko) : mesurer exige les valeurs resolues, comme la copie de Screen::list().
  fui::ListProps resolved = props;
  if (resolved.sidePadding < 0) resolved.sidePadding = theme.listSidePadding;
  if (resolved.rowInset < 0) resolved.rowInset = theme.listInset;
  if (resolved.rowGap < 0) {
    resolved.rowGap = theme.listRowGap;
    if (touch && resolved.rowGap < theme.listTouchRowGap) resolved.rowGap = theme.listTouchRowGap;
  }
  const int16_t scrollW = resolved.scrollIndicatorWidth < 0 ? theme.listScrollWidth : resolved.scrollIndicatorWidth;
  const int16_t scrollInset = resolved.scrollIndicatorInset < 0 ? theme.listScrollInset : resolved.scrollIndicatorInset;
  const uint8_t scrollSide = resolved.scrollIndicatorSide == 0xFF ? theme.listScrollSide : resolved.scrollIndicatorSide;
  const int16_t rowGap = resolved.rowGap;
  const int16_t sidePad = resolved.sidePadding;
  const int16_t rowInset = resolved.rowInset > 0 ? resolved.rowInset : 0;

  // Bande des lignes : retrait du theme, puis la piste de l'indicateur de
  // defilement, toujours reservee sur une liste geree par ListNav (props.nav).
  fui::Rect area = screen.body();
  area.x = static_cast<int16_t>(area.x + rowInset);
  area.width = static_cast<int16_t>(area.width - rowInset * 2);
  if (resolved.scrollIndicator && props.nav != nullptr && scrollW > 0) {
    const int16_t needed = static_cast<int16_t>(scrollW + scrollInset + 2);
    if (rowInset < needed) {
      const int16_t cut = static_cast<int16_t>(needed - rowInset);
      area.width = static_cast<int16_t>(area.width - cut);
      if (scrollSide == 1) area.x = static_cast<int16_t>(area.x + cut);
    }
  }

  // Couleurs de la ligne selon son etat, comme list() : le style de selection
  // du theme etendu sur ses lignes de base (Screen::resolveListProps()).
  fui::StyleSet styles = theme.listRow.unset() ? fui::defaultListRowStyles() : theme.listRow;
  switch (theme.listSelectionStyle) {
    case fui::SelectionStyle::LightPill:
      styles.selected.background = fui::Paint::dither(fui::Color::LightGray);
      styles.selected.foreground = fui::Paint::solid(fui::Color::Black);
      styles.active = styles.selected;
      break;
    case fui::SelectionStyle::Underline:
    case fui::SelectionStyle::Triangle:
      styles.selected = styles.normal;
      break;
    case fui::SelectionStyle::InvertFill:
      break;
  }

  fui::DrawTarget& target = screen.target();
  fui::TextStyle nameBase = theme.smallText;
  nameBase.font = fui::GfxRendererTarget::FONT_LABEL;
  nameBase.bold = true;
  fui::TextStyle countBase = nameBase;
  countBase.bold = false;
  countBase.align = fui::TextAlign::Right;
  const int16_t textH = target.lineHeight(nameBase.font);
  // Ecart entre le bas du texte et le filet.
  const int16_t ruleGap = UiThemeTokensDetail::scaledListMetric(4);

  const std::vector<TaskGroup>& groups = TASK_STORE.groups();
  const int count = props.count;
  const int top = std::min<int>(props.topIndex, count - 1);
  const int windowEnd = props.itemsWindowFirst + static_cast<int>(winItems.size());
  int16_t y = area.y;
  for (int i = top; i < count && i < windowEnd && i >= props.itemsWindowFirst; ++i) {
    if (y >= area.bottom()) break;
    const fui::ListItem& item = winItems[static_cast<size_t>(i - props.itemsWindowFirst)];
    const int16_t h = fui::measureListRow(target, screen.frame().assets(), area.width, resolved, item).height;
    // list() s'arrete a la premiere ligne qui ne tient pas entiere.
    if (y + h > area.bottom()) break;
    const TaskListRow& row = rows[static_cast<size_t>(i)];
    if (row.kind == TaskRowKind::GroupHeader && row.groupIndex >= 0 &&
        static_cast<size_t>(row.groupIndex) < groups.size()) {
      fui::State state = props.selectedIndex == i ? fui::StateSelected : fui::StateNormal;
      state = screen.frame().stateFor(props.action, item.actionValue, state);
      const fui::Paint ink = styles.resolve(state).foreground;
      const fui::Rect content{static_cast<int16_t>(area.x + sidePad), y, static_cast<int16_t>(area.width - sidePad * 2),
                              h};
      // Le filet occupe la derniere ligne de pixels de la ligne, le texte se
      // pose juste au-dessus : l'air de la ligne est au-dessus du titre.
      const int16_t ruleY = static_cast<int16_t>(y + h - 1);
      const int16_t textY = static_cast<int16_t>(std::max<int>(y, ruleY - ruleGap - textH));
      char countText[8];
      std::snprintf(countText, sizeof(countText), "%d", taskGroupOpenCount(rows, i));
      const fui::TextStyle countStyle = fui::textStyleWithForeground(countBase, ink);
      const int16_t countW = target.measureText(countStyle.font, countText, countStyle).width;
      constexpr int16_t kCountGap = 8;
      target.text(fui::Rect{content.x, textY, static_cast<int16_t>(content.width - countW - kCountGap), textH},
                  groups[static_cast<size_t>(row.groupIndex)].name, fui::textStyleWithForeground(nameBase, ink));
      target.text(fui::Rect{content.x, textY, content.width, textH}, countText, countStyle);
      // Toujours noir : le filet se fond dans le fond d'une ligne selectionnee
      // en video inverse, comme sur la maquette (.grp.sel).
      target.fill(fui::Rect{content.x, ruleY, content.width, 1}, fui::Paint::solid(fui::Color::Black));
    }
    y = static_cast<int16_t>(y + h + rowGap);
  }
}

void TaskListActivity::activateIndex(int index) {
  if (index < 0 || index >= static_cast<int>(rows.size())) return;
  switch (rows[static_cast<size_t>(index)].kind) {
    case TaskRowKind::Task:
      if (selecting) {
        toggleSelectionAt(index);
      } else {
        toggleAt(index);
      }
      return;
    case TaskRowKind::AddTask:
      createTask();
      return;
    case TaskRowKind::GroupHeader:
      // « Ajouter » dans ce groupe. Le mode selection ne s'arrete jamais sur
      // un titre ; s'il y parvenait, rien a choisir.
      if (!selecting)
        createTask(TASK_STORE.groups()[static_cast<size_t>(rows[static_cast<size_t>(index)].groupIndex)].id);
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
  if (selecting) {
    // Sans rien de choisi, les deux libelles sont vides et les boutons ne
    // font rien.
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (selectionCount > 0) tickSelection();
      return true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (selectionCount > 0) promptDeleteSelection();
      return true;
    }
  }
  // Sur un titre de groupe, Gauche et Droite sautent au titre precedent ou
  // suivant, sans boucler : rien au bout (le libelle y est vide).
  const int header = focusedGroupHeader();
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (header >= 0) {
      const int next = taskListGroupJump(rows, header, +1);
      if (next >= 0) moveSelectionTo(next);
    } else if (!headerFocused) {
      // Sur le compteur, Droite n'a pas de tache dont ouvrir le detail.
      openDetailAt(activeNav().selected);
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (header >= 0) {
      const int previous = taskListGroupJump(rows, header, -1);
      if (previous >= 0) moveSelectionTo(previous);
      return true;
    }
    // Ajouter, sur une liste peuplee comme sur une liste vide : une seule
    // regle. La ligne "+ Ajouter une tache" d'un index vide mene au meme
    // endroit, pour qui ne cherche pas le bouton. Depuis une tache : sans
    // groupe (« ajouter » sur un titre range la tache dans le groupe).
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
  // Toucher le compteur de l'en-tete bascule la vue, comme Confirmer dessus.
  // Avant le routage des lignes : sa zone reste dans la bande de l'en-tete,
  // donc un toucher sur une ligne ne bascule jamais.
  int tapX = 0;
  int tapY = 0;
  if (mappedInput.hasTouchHardware() && hasHeaderCounter() && mappedInput.wasScreenTapped(tapX, tapY)) {
    Rect hit;
    {
      RenderLock lock(*this);
      hit = counterTouchRect;
    }
    if (tapX >= hit.x && tapX < hit.x + hit.width && tapY >= hit.y && tapY < hit.y + hit.height) {
      switchView();
      return true;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Sur le compteur, appui court ou long : la bascule, jamais un menu.
    if (headerFocused) {
      switchView();
      return true;
    }
    const int selected = activeNav().selected;
    if (selected < 0 || selected >= listCount()) return true;
    // Sur « + Ajouter » ou un titre de groupe, un appui long fait l'action
    // normale (ajouter) : le menu ne concerne qu'une tache.
    // En mode selection, un appui long choisit la ligne comme un appui court :
    // le menu n'y a pas cours.
    if (!selecting && mappedInput.getHeldTime() >= kHoldMs &&
        rows[static_cast<size_t>(selected)].kind == TaskRowKind::Task) {
      openTaskMenu(selected);
    } else {
      activateIndex(selected);
    }
    return true;
  }
  return false;
}

void TaskListActivity::onRowLongPress(const int index) {
  headerFocused = false;
  // Hors tache (titre de groupe, ligne d'ajout) l'appui long vaut un toucher,
  // comme l'appui long sur Confirmer (handleButtons()).
  if (selecting || index < 0 || index >= static_cast<int>(rows.size()) ||
      rows[static_cast<size_t>(index)].kind != TaskRowKind::Task) {
    activateIndex(index);
  } else {
    openTaskMenu(index);
  }
}

void TaskListActivity::onRowAction(const fui::ActionEvent& event) {
  // En mode selection, un toucher sur un titre de groupe n'y pose pas le
  // curseur : le mode saute les titres.
  if (selecting && event.value >= 0 && event.value < static_cast<int>(rows.size()) &&
      rows[static_cast<size_t>(event.value)].kind == TaskRowKind::GroupHeader) {
    return;
  }
  headerFocused = false;
  UiListActivity::onRowAction(event);
}

void TaskListActivity::onBackButton() {
  if (selecting) {
    stopSelection();
    return;
  }
  // La vue faites s'ouvre comme une sous-vue : Retour ramene a la liste
  // ouverte, pas hors de Tasks.
  if (view == TaskListView::Done) {
    switchView();
    return;
  }
  finish();
}

bool TaskListActivity::hasHeaderCounter() const { return view == TaskListView::Done || doneCount > 0; }

int TaskListActivity::stepFrom(const int from, const int direction) const {
  const int count = listCount();
  if (!selecting) {
    return direction > 0 ? ButtonNavigator::nextIndex(from, count) : ButtonNavigator::previousIndex(from, count);
  }
  const int next = taskListStepSkippingHeaders(rows, from, direction);
  return next >= 0 ? next : 0;
}

int TaskListActivity::firstCursorRow() const { return selecting ? stepFrom(-1, +1) : 0; }

int TaskListActivity::cursorRowFor(const int index) const {
  if (!selecting || index < 0 || index >= static_cast<int>(rows.size()) ||
      rows[static_cast<size_t>(index)].kind != TaskRowKind::GroupHeader) {
    return index;
  }
  // Un titre est toujours suivi d'au moins une de ses taches : le pas en
  // avant ne boucle pas.
  return stepFrom(index, +1);
}

int TaskListActivity::focusedGroupHeader() {
  if (selecting || headerFocused) return -1;
  const int selected = activeNav().selected;
  if (selected < 0 || selected >= static_cast<int>(rows.size())) return -1;
  return rows[static_cast<size_t>(selected)].kind == TaskRowKind::GroupHeader ? selected : -1;
}

void TaskListActivity::navigateButtons() {
  // Les memes quatre gestes que UiListActivity::navigateButtons(), chacun
  // appele une fois par passe : ButtonNavigator retient d'un appel a l'autre
  // qu'un maintien a deja agi, pour que sa relache ne rejoue pas un pas. Seuls
  // les pas simples connaissent le compteur ; un maintien pagine comme
  // ailleurs et rend le curseur a la liste.
  const int count = listCount();
  auto& n = activeNav();
  buttonNavigator.onNextRelease([this, &n] {
    if (headerFocused) {
      headerFocused = false;
      moveSelectionTo(firstCursorRow());
      return;
    }
    moveSelectionTo(stepFrom(n.selected, +1));
  });
  buttonNavigator.onPreviousRelease([this, count, &n] {
    if (headerFocused) {
      // Comme Haut sur la premiere ligne d'une liste sans compteur : la
      // derniere ligne.
      headerFocused = false;
      moveSelectionTo(selecting ? stepFrom(-1, -1) : (count > 0 ? count - 1 : 0));
      return;
    }
    if (n.selected == firstCursorRow() && hasHeaderCounter()) {
      headerFocused = true;
      requestUpdate();
      return;
    }
    moveSelectionTo(stepFrom(n.selected, -1));
  });
  buttonNavigator.onNextContinuous([this, count, &n] {
    headerFocused = false;
    moveSelectionTo(cursorRowFor(ButtonNavigator::nextPageIndex(n.selected, count, n.pageRows())));
  });
  buttonNavigator.onPreviousContinuous([this, count, &n] {
    headerFocused = false;
    moveSelectionTo(cursorRowFor(ButtonNavigator::previousPageIndex(n.selected, count, n.pageRows())));
  });
}

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
  // « Supprimer les faites » n'apparait que s'il y en a, « Groupe » que s'il
  // existe au moins un groupe : le tableau est donc construit a chaque
  // ouverture, pas constant.
  enum class MenuAction : uint8_t { Tick, View, Edit, Group, Select, DeleteDone, KeepAwake, TextSize, Delete, Sync };
  constexpr size_t kMenuMax = 10;
  std::array<MenuAction, kMenuMax> menu{};
  size_t menuCount = 0;
  const bool hasGroups = !TASK_STORE.groups().empty();
  for (const MenuAction action :
       {MenuAction::Tick, MenuAction::View, MenuAction::Edit, MenuAction::Group, MenuAction::Select,
        MenuAction::DeleteDone, MenuAction::KeepAwake, MenuAction::TextSize, MenuAction::Delete, MenuAction::Sync}) {
    if (action == MenuAction::DeleteDone && doneCount <= 0) continue;
    if (action == MenuAction::Group && !hasGroups) continue;
    menu[menuCount++] = action;
  }
  std::vector<std::string> options;
  options.reserve(menuCount);
  for (size_t i = 0; i < menuCount; ++i) {
    switch (menu[i]) {
      case MenuAction::Tick:
        options.emplace_back(rec.done ? tr(STR_TASK_MENU_REOPEN) : tr(STR_TASK_MENU_TICK));
        break;
      case MenuAction::View:
        options.emplace_back(tr(STR_TASK_MENU_VIEW));
        break;
      case MenuAction::Edit:
        options.emplace_back(tr(STR_TASK_MENU_EDIT));
        break;
      case MenuAction::Group:
        options.emplace_back(tr(STR_TASK_MENU_GROUP));
        break;
      case MenuAction::Select:
        options.emplace_back(tr(STR_TASK_MENU_SELECT));
        break;
      case MenuAction::DeleteDone: {
        char label[64];
        std::snprintf(label, sizeof(label), tr(STR_TASK_MENU_DELETE_DONE), doneCount);
        options.emplace_back(label);
        break;
      }
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
  popup.show(rec.title, options, 0, [this, id, menu, menuCount](const int choice) {
    if (choice < 0 || static_cast<size_t>(choice) >= menuCount) return;
    const int row = rowOfTask(id.c_str());
    switch (menu[static_cast<size_t>(choice)]) {
      case MenuAction::Tick:
        toggleAt(row);
        break;
      case MenuAction::View:
        openDetailAt(row);
        break;
      case MenuAction::Edit:
        editTaskTitle(id);
        break;
      case MenuAction::Group:
        chooseGroup(id);
        break;
      case MenuAction::Select:
        startSelection(id);
        break;
      case MenuAction::DeleteDone:
        promptDeleteDone();
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
  std::vector<std::string> labels{tr(STR_TASK_FONT_10), tr(STR_TASK_FONT_12), I18N.get(kLargeTaskFontLabel)};
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
          uiTarget.setFont(fui::GfxRendererTarget::FONT_LABEL, groupTitleFontId());
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

void TaskListActivity::chooseGroup(const std::string& taskId) {
  const TaskRecord* rec = TASK_STORE.find(taskId.c_str());
  if (rec == nullptr || TASK_STORE.groups().empty()) return;
  TaskGroupChoices choices;
  std::vector<std::string> labels;
  const uint8_t selected = buildTaskGroupChoices(TASK_STORE.groups(), rec->group, choices, labels);
  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "TaskGroup", StrId::STR_TASK_MENU_GROUP,
                                                std::move(labels), selected),
      [this, taskId, choices](const ActivityResult& result) {
        const auto* choice = std::get_if<OptionSelectionResult>(&result.data);
        if (result.isCancelled || choice == nullptr) return;
        const char* group = taskGroupChoiceId(TASK_STORE.groups(), choices, choice->index);
        const TaskRecord* current = TASK_STORE.find(taskId.c_str());
        if (group == nullptr || current == nullptr || std::strcmp(group, current->group) == 0) return;

        TaskRecord next = *current;
        std::snprintf(next.group, sizeof(next.group), "%s", group);
        // Au bout de sa bande dans le nouveau groupe, comme sur le serveur.
        next.order = TASK_ORDER_UNSET;
        TaskOp op{};
        op.kind = TaskOpKind::Group;
        std::snprintf(op.id, sizeof(op.id), "%s", next.id);
        std::snprintf(op.group, sizeof(op.group), "%s", next.group);
        // La file avant l'index, jamais l'inverse (voir toggleAt()).
        if (!TASK_STORE.appendOp(op)) {
          LOG_ERR(TAG, "Could not queue the group change for %s; nothing applied", next.id);
          showNotice(StatusNotice::WriteFailed);
          return;
        }
        pendingOps = true;
        if (!TASK_STORE.upsert(next)) showNotice(StatusNotice::WriteFailed);
        refreshAfterEdit(taskId.c_str());
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

void TaskListActivity::promptDeleteDone() {
  if (doneCount <= 0) return;
  const std::string heading = tr(STR_DELETE) + std::string("? ");
  char body[64];
  std::snprintf(body, sizeof(body), tr(STR_TASK_DONE_TASKS_COUNT), doneCount);
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             // 120 octets de pile : un indice par tache au plus.
                             uint8_t targets[MAX_TASKS];
                             const size_t count = taskDoneTargets(TASK_STORE.all(), targets);
                             applyBatch(BatchAction::Delete, targets, count);
                           }
                           requestUpdate(true);
                         });
}

void TaskListActivity::startSelection(const std::string& id) {
  const int row = rowOfTask(id.c_str());
  selecting = true;
  selection.reset();
  selectionCount = 0;
  // La tache dont on a ouvert le menu est deja choisie : c'est presque
  // toujours la premiere qu'on voulait.
  if (row >= 0) toggleSelectionAt(row);
  // Le curseur est deja sur cette tache (son menu vient d'elle) ; a defaut,
  // jamais sur un titre de groupe, que le mode saute.
  headerFocused = false;
  moveSelectionTo(row >= 0 ? row : cursorRowFor(taskListClampSelection(rows, activeNav().selected)));
}

void TaskListActivity::stopSelection() {
  selecting = false;
  selection.reset();
  selectionCount = 0;
  requestUpdate();
}

void TaskListActivity::toggleSelectionAt(const int index) {
  if (index < 0 || index >= static_cast<int>(rows.size())) return;
  const TaskListRow& row = rows[static_cast<size_t>(index)];
  if (row.kind != TaskRowKind::Task || row.recordIndex < 0) return;
  const size_t bit = static_cast<size_t>(row.recordIndex);
  // std::bitset::flip() hors bornes leve out_of_range, soit abort() sans
  // exceptions. L'index ne depasse jamais MAX_TASKS (fromJson, stageUpsert),
  // mais la garde coute une comparaison.
  if (bit >= MAX_TASKS) return;
  selection.flip(bit);
  selectionCount += selection.test(bit) ? 1 : -1;
  requestUpdate();
}

void TaskListActivity::tickSelection() {
  uint8_t targets[MAX_TASKS];
  const size_t picked = taskSelectionTargets(selection, TASK_STORE.all().size(), targets);
  // Cocher laisse telles quelles les taches deja faites : une op Done(true)
  // de plus ecraserait une reouverture faite entre-temps sur le web.
  const std::vector<TaskRecord>& records = TASK_STORE.all();
  size_t open = 0;
  for (size_t i = 0; i < picked; ++i) {
    if (!records[targets[i]].done) targets[open++] = targets[i];
  }
  applyBatch(BatchAction::Tick, targets, open);
}

void TaskListActivity::promptDeleteSelection() {
  const std::string heading = tr(STR_DELETE) + std::string("? ");
  char body[64];
  std::snprintf(body, sizeof(body), tr(STR_TASK_SELECTED_COUNT), selectionCount);
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) {
                           // Annuler garde le mode et sa selection : on corrige le choix,
                           // on ne le refait pas.
                           if (!result.isCancelled) {
                             uint8_t targets[MAX_TASKS];
                             const size_t count = taskSelectionTargets(selection, TASK_STORE.all().size(), targets);
                             applyBatch(BatchAction::Delete, targets, count);
                           }
                           requestUpdate(true);
                         });
}

void TaskListActivity::applyBatch(const BatchAction action, const uint8_t* targets, const size_t count) {
  // L'action termine le mode, meme sans cible : Droite sur une selection de
  // taches toutes deja faites ne laisse pas l'ecran dans un etat muet.
  selecting = false;
  selection.reset();
  selectionCount = 0;
  if (count == 0) {
    requestUpdate();
    return;
  }

  struct Ctx {
    const std::vector<TaskRecord>* records;
    const uint8_t* targets;
    TaskOpKind kind;
  } ctx{&TASK_STORE.all(), targets, action == BatchAction::Delete ? TaskOpKind::Del : TaskOpKind::Done};
  // La file avant l'index, comme pour une seule tache (deleteTask, toggleAt) :
  // seules les `queued` premieres cibles, dont l'op est sur la carte, sont
  // appliquees ici.
  const size_t queued = TASK_STORE.appendOpBatch(
      count,
      [](void* raw, const size_t i, TaskOp& op) {
        const auto* c = static_cast<const Ctx*>(raw);
        op.kind = c->kind;
        std::snprintf(op.id, sizeof(op.id), "%s", (*c->records)[c->targets[i]].id);
        op.done = true;
        return true;
      },
      &ctx);
  if (queued < count) {
    LOG_ERR(TAG, "Only %u of %u batch ops were queued", static_cast<unsigned>(queued), static_cast<unsigned>(count));
    showNotice(StatusNotice::WriteFailed);
  }
  if (queued == 0) {
    requestUpdate();
    return;
  }
  pendingOps = true;

  {
    // erase() decale les enregistrements suivants et `rows`/`order` indexent
    // ce vecteur : tout le lot et la reconstruction sous un seul verrou, comme
    // deleteTask(). Les cibles vont du plus grand indice au plus petit, donc
    // chaque effacement laisse valides celles qui restent.
    RenderLock lock(*this);
    for (size_t i = 0; i < queued; ++i) {
      // Copie (216 octets de pile) : stageUpsert() ecrit dans le vecteur
      // qu'on lit, et stageRemove() efface l'element dont on lirait l'id.
      TaskRecord rec = TASK_STORE.all()[targets[i]];
      if (action == BatchAction::Delete) {
        TASK_STORE.stageRemove(rec.id);
      } else {
        rec.done = true;
        TASK_STORE.stageUpsert(rec);
      }
    }
    dirty = true;
    rebuildOrder();
  }
  // Une seule ecriture de l'index pour tout le lot, au lieu d'une par tache.
  // Les notes des taches supprimees partent a la sync suivante, comme pour
  // deleteTask().
  if (!TASK_STORE.saveIndex()) showNotice(StatusNotice::WriteFailed);
  const int next = taskListClampSelection(rows, activeNav().selected);
  if (next >= 0) moveSelectionTo(next);
  requestUpdate();
}

void TaskListActivity::switchView() {
  view = view == TaskListView::Open ? TaskListView::Done : TaskListView::Open;
  headerFocused = false;
  // En mode selection, les taches de l'autre vue quittent la selection : une
  // action ne porte jamais sur une ligne qu'on ne voit plus. Vers la vue
  // ouverte partent les faites, et inversement.
  if (selecting) {
    const bool leavingDone = view == TaskListView::Open;
    const std::vector<TaskRecord>& records = TASK_STORE.all();
    for (size_t i = 0; i < records.size() && i < MAX_TASKS; ++i) {
      if (records[i].done == leavingDone && selection.test(i)) {
        selection.reset(i);
        --selectionCount;
      }
    }
  }
  dirty = true;
  rebuildOrder();
  // Une autre liste : le curseur repart de sa premiere ligne, qui existe
  // toujours (la vue faites n'existe pas vide, la vue ouverte a au moins la
  // ligne d'ajout) ; en mode selection, de la premiere qui n'est pas un titre.
  moveSelectionTo(firstCursorRow());
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
  // Une coche ou une decoche fait quitter la vue a la tache : la selection
  // reste a son indice, donc sur la tache qui a pris sa place. On coche ainsi
  // une liste de haut en bas (et on rouvre les faites) sans jamais deplacer le
  // curseur.
  //
  // Sauf quand la tache reste a l'ecran : la derniere faite decochee ramene
  // seule a la liste ouverte (rebuildOrder()), ou la selection la suit.
  const int moved = rowOfTask(id.c_str());
  int next = moved >= 0 ? moved : taskListClampSelection(rows, activeNav().selected);
  // Cocher la derniere tache sans groupe laisse le curseur sur le titre du
  // premier groupe : on passe a sa premiere tache, pour que Confirm continue de
  // cocher plutot que d'ouvrir l'ajout dans le groupe.
  if (moved < 0) next = taskListCursorAfterRemoval(rows, next);
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
                           // (chemin Pop). `order`, `rows`, `openCount` et `view`
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
                           // suit. Si elle a quitte l'ecran (cochee ou rouverte : l'autre vue), le clamp
                           // garde un indice valide.
                           const int moved = rowOfTask(id.c_str());
                           const int next = moved >= 0 ? moved : taskListClampSelection(rows, activeNav().selected);
                           if (next >= 0) moveSelectionTo(next);
                           requestUpdate();
                         });
}

void TaskListActivity::createTask(const char* groupIdArg) {
  // L'id du groupe, copie EN PREMIER : `groupIdArg` pointe dans
  // TASK_STORE.groups(), et reserveFullCapacity() plus bas realloue ce vecteur
  // (liberant le bloc pointe). Une std::string de 9 octets reste dans le tampon
  // court (SSO), sans tas.
  std::string group = groupIdArg != nullptr ? groupIdArg : "";
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

  // Le curseur revient a la liste : au retour, la selection suit la tache creee
  // (ou reste en place sur une annulation), jamais le compteur de l'en-tete.
  headerFocused = false;

  // Un toucher sur "+ Ajouter une tache" quitte l'ecran : sans cela un flash de
  // toucher residuel griserait la ligne 0 au retour (UiListActivity.h:41-43).
  app.clearTapFlash();

  // minLength=1 : c'est le clavier qui refuse un titre vide, comme pour la
  // modification d'un titre existant (TaskDetailActivity::editTitle()).
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TASK_NEW), "",
                                                                 TASK_TITLE_MAX, InputType::Text, /*minLength=*/1),
                         [this, group = std::move(group)](const ActivityResult& result) {
                           const auto* entered = std::get_if<KeyboardResult>(&result.data);
                           // Annulation : rien n'a ete ecrit, ni op ni index, et il n'y a donc
                           // rien a reconstruire. Le repaint est deja programme par le depilage
                           // (ActivityManager.cpp:400-406), comme au retour de l'ecran de detail.
                           if (result.isCancelled || entered == nullptr) return;
                           askPriorityForNewTask(entered->text, group);
                         });
}

void TaskListActivity::askPriorityForNewTask(std::string title, std::string groupId) {
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
      [this, title = std::move(title), groupId = std::move(groupId)](const ActivityResult& result) {
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
        // Dans le groupe du titre ou l'on a fait « ajouter », sinon sans groupe.
        std::snprintf(rec.group, sizeof(rec.group), "%s", groupId.c_str());

        TaskOp op{};
        op.kind = TaskOpKind::Add;
        std::memcpy(op.id, rec.id, sizeof(op.id));
        std::memcpy(op.title, rec.title, sizeof(op.title));
        op.priority = rec.priority;
        std::memcpy(op.group, rec.group, sizeof(op.group));

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
        // Ajoutee depuis la vue faites, la tache neuve (ouverte) n'y serait
        // pas : on revient a la liste ouverte pour la montrer.
        view = TaskListView::Open;
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

void TaskListActivity::formatHeaderRight(char* out, const size_t size) const {
  // U+21BB (une op attend le serveur) suit le compteur dans le meme libelle :
  // les deux occupent le bord droit de la ligne du titre. inter_8_regular le
  // porte (scripts/measure_label.py) ; « 120 a faire ↻ » mesure 100 px, sous
  // les 200 px du pire theme. Rien quand tout est envoye et sans compteur :
  // l'etat normal ne se signale pas.
  const char* pendingMark = pendingOps ? "\u21BB" : "";
  if (!hasHeaderCounter()) {
    std::snprintf(out, size, "%s", pendingMark);
    return;
  }
  // En vue ouverte, combien de faites ; en vue faites, combien a faire : le
  // compteur nomme toujours l'autre vue, celle ou il mene.
  const bool doneView = view == TaskListView::Done;
  std::snprintf(out, size, "%d %s%s%s", doneView ? openCount : doneCount,
                doneView ? tr(STR_TASK_TODO_COUNT) : tr(STR_TASK_DONE_COUNT), pendingOps ? " " : "", pendingMark);
}

void TaskListActivity::drawChrome() {
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  char title[48];
  if (selecting) {
    std::snprintf(title, sizeof(title), "%s (%d)", tr(STR_TASK_SELECTION), selectionCount);
  } else if (view == TaskListView::Done) {
    std::snprintf(title, sizeof(title), "%s (%d)", tr(STR_TASK_DONE_TITLE), doneCount);
  } else {
    std::snprintf(title, sizeof(title), "%s (%d)", tr(STR_TASK_TITLE), openCount);
  }
  // Libelle droit passe comme `subtitle`, que les themes placent au bord droit,
  // cale sur le bas de la ligne du titre, dans la petite police.
  char right[48];
  formatHeaderRight(right, sizeof(right));
  const char* subtitle = right[0] != '\0' ? right : nullptr;
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, title, false, 0, subtitle);
  } else {
    GUI.drawHeader(renderer, header, title, subtitle);
  }
  if (!hasHeaderCounter()) {
    counterTouchRect = Rect{};
    return;
  }
  // La ou le theme vient de poser le libelle (headerRightLabelRect), pour le
  // surligner et le toucher sans redeviner sa position theme par theme.
  const Rect label = GUI.headerRightLabelRect(renderer, header, right);
  // Toute la hauteur de la bande, et un peu de marge a gauche : une cible
  // tactile plus large que le texte, qui ne descend jamais sur la premiere
  // ligne de la liste.
  constexpr int kTouchPadX = 12;
  const int touchX = std::max(header.x, label.x - kTouchPadX);
  counterTouchRect = Rect{touchX, header.y, header.x + header.width - touchX, header.height};
  if (headerFocused) {
    // Video inverse, comme une ligne selectionnee : rectangle plein et texte
    // blanc, en inversant la boite du texte avec une petite marge.
    constexpr int kPadX = 4;
    constexpr int kPadY = 1;
    renderer.invertRect(label.x - kPadX, label.y - kPadY, label.width + 2 * kPadX, label.height + 2 * kPadY);
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
  // Sur une tache deja faite (vue faites), Confirmer la rouvre : le libelle le dit.
  const bool selectedDone = kind == TaskRowKind::Task && selected >= 0 && selected < static_cast<int>(rows.size()) &&
                            TASK_STORE.all()[static_cast<size_t>(rows[static_cast<size_t>(selected)].recordIndex)].done;
  const char* confirmLabel = selectedDone ? tr(STR_TASK_UNTICK) : tr(STR_TASK_TICK);
  const char* rightLabel = tr(STR_TASK_DETAIL);
  const char* leftLabel = tr(STR_TASK_NEW_SHORT);
  if (selecting) {
    // Mode selection : Confirmer choisit, Droite coche, Gauche efface
    // (« supprimer » depasse la case de 8 px en francais, measure_label.py),
    // Retour sort du mode. Rien de choisi : ni cocher ni effacer. Droite et
    // Gauche portent sur la selection, pas sur le curseur : le compteur de
    // l'en-tete ne change que Confirmer.
    const bool picked = kind == TaskRowKind::Task && selected >= 0 && selected < static_cast<int>(rows.size()) &&
                        selection.test(static_cast<size_t>(rows[static_cast<size_t>(selected)].recordIndex));
    // Un titre de groupe n'a rien a choisir (et le mode n'y pose jamais le
    // curseur : cursorRowFor()).
    confirmLabel = headerFocused                      ? tr(STR_TASK_SHOW)
                   : kind == TaskRowKind::GroupHeader ? ""
                   : picked                           ? tr(STR_TASK_UNPICK)
                                                      : tr(STR_TASK_PICK);
    leftLabel = selectionCount > 0 ? tr(STR_TASK_ERASE_SHORT) : "";
    rightLabel = selectionCount > 0 ? tr(STR_TASK_TICK) : "";
  } else if (headerFocused) {
    // Sur le compteur : Confirmer montre l'autre vue, Gauche ajoute toujours,
    // Droite n'a pas de detail a ouvrir.
    confirmLabel = tr(STR_TASK_SHOW);
    rightLabel = "";
  } else if (kind == TaskRowKind::GroupHeader) {
    // Sur un titre : ajouter dans ce groupe, et sauter au titre precedent ou
    // suivant ; case vide au bout, pas de bouclage.
    confirmLabel = tr(STR_TASK_NEW_SHORT);
    leftLabel = taskListGroupJump(rows, selected, -1) >= 0 ? tr(STR_TASK_PREV_GROUP) : "";
    rightLabel = taskListGroupJump(rows, selected, +1) >= 0 ? tr(STR_TASK_NEXT_GROUP) : "";
  } else if (kind == TaskRowKind::AddTask) {
    confirmLabel = tr(STR_TASK_NEW_SHORT);
    leftLabel = "";
    rightLabel = "";
  }
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel, leftLabel, rightLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
