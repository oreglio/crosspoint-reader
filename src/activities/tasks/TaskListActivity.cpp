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
#include "fontIds.h"
#include "tasks/TaskOpQueue.h"

namespace {

constexpr char TAG[] = "TASKLIST";
// Air au-dessus et en dessous du bloc de texte d'une ligne, dans la hauteur
// de ligne totale. Pas de filet entre les lignes : juste assez d'air pour
// que le trait barre d'une tache faite ne colle pas a la ligne suivante.
constexpr int kRowVPad = 4;

// La police EpdFontFamily n'a que REGULAR/BOLD/ITALIC/BOLD_ITALIC : pas de
// graisse "light". L'italique est le seul style existant qui s'allege
// visuellement du normal sans changer la taille du corps (contrainte "un
// seul corps de police, jamais un badge") ; c'est ce qui tient lieu de light
// pour la priorite basse ici, faute d'une vraie graisse legere dans la fonte
// embarquee. A remplacer si LexendDeca Light est un jour ajoutee au build.
EpdFontFamily::Style styleForPriority(uint8_t priority) {
  switch (priority) {
    case TASK_PRIORITY_HIGH:
      return EpdFontFamily::BOLD;
    case TASK_PRIORITY_LOW:
      return EpdFontFamily::ITALIC;
    case TASK_PRIORITY_NORMAL:
    default:
      return EpdFontFamily::REGULAR;
  }
}

}  // namespace

void TaskListActivity::onEnter() {
  Activity::onEnter();
  // Le store ne se charge pas tout seul au premier acces (getInstance() ne
  // fait rien lire) : c'est a l'ecran de le demander ici, jamais depuis le
  // rendu. Voir PersistableStore::ensureLoaded().
  TASK_STORE.ensureLoaded();

  records = TASK_STORE.all();
  tickedHere.clear();
  order.clear();
  rows.clear();
  rowHeights.clear();
  selected = 0;
  scrollTop = 0;
  showDone = false;
  dirty = true;
  rebuildOrder();
  requestUpdate();
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
  rowHeights.clear();
  rowHeights.shrink_to_fit();
  Activity::onExit();
}

void TaskListActivity::rebuildOrder() {
  if (!dirty) return;
  buildTaskOrder(records, tickedHere, order, openCount);
  buildTaskListRows(order, openCount, showDone, rows);
  doneCount = static_cast<int>(records.size()) - openCount;
  selected = taskListNormalizeSelection(rows, selected);
  // Les hauteurs dependent du renderer (mesure de texte) : on se contente de
  // les invalider ici, render() les recalcule au prochain passage.
  rowHeights.clear();
  dirty = false;
}

void TaskListActivity::remeasureRowHeights(const int lineHeight, const int smallLineHeight, const int maxTextWidth) {
  rowHeights.clear();
  rowHeights.reserve(rows.size());
  for (const auto& row : rows) {
    if (row.kind == TaskRowKind::DoneHeader) {
      rowHeights.push_back(smallLineHeight + 2 * kRowVPad);
      continue;
    }
    const TaskRecord& rec = records[row.recordIndex];
    const auto style = styleForPriority(rec.priority);
    const auto lines = renderer.wrappedText(LEXENDDECA_14_FONT_ID, rec.title, maxTextWidth, /*maxLines=*/2, style);
    const int lineCount = std::max<int>(1, static_cast<int>(lines.size()));
    rowHeights.push_back(lineCount * lineHeight + 2 * kRowVPad);
  }
}

void TaskListActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleSelected();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    openDetail();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    // Bouton avant restant une fois Retour/cocher/detail assignes (voir le
    // rapport de la Tache 5) : bascule la section "terminees".
    showDone = !showDone;
    dirty = true;
    rebuildOrder();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    selected = taskListStepSelection(rows, selected, -1);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    selected = taskListStepSelection(rows, selected, +1);
    requestUpdate();
    return;
  }
}

void TaskListActivity::toggleSelected() {
  if (selected < 0 || selected >= static_cast<int>(rows.size())) return;
  const TaskListRow row = rows[selected];
  if (row.kind != TaskRowKind::Task) return;

  TaskRecord& rec = records[row.recordIndex];
  rec.done = !rec.done;

  const std::string id(rec.id);
  const auto already = std::find(tickedHere.begin(), tickedHere.end(), id);
  if (rec.done) {
    // Reste en place, barree, jusqu'a onExit() : voir le commentaire sur
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
  requestUpdate();
}

void TaskListActivity::openDetail() {
  if (selected < 0 || selected >= static_cast<int>(rows.size()) || rows[selected].kind != TaskRowKind::Task) return;
  // TaskDetailActivity arrive avec la Tache 6 ; ce bouton n'a pas encore
  // d'ecran a ouvrir.
  LOG_INF(TAG, "Detail demande pour %s (TaskDetailActivity : Tache 6)", records[rows[selected].recordIndex].id);
}

void TaskListActivity::startSync() {
  // TaskSyncActivity arrive avec la Tache 9, qui devra aussi choisir quel
  // bouton la declenche : les quatre boutons avant sont deja pris par
  // Retour / cocher / detail / bascule "terminees" (voir le rapport de la
  // Tache 5 sur la penurie de boutons face aux quatre actions demandees).
  LOG_INF(TAG, "Sync demandee (TaskSyncActivity : Tache 9)");
}

void TaskListActivity::render(RenderLock&&) {
  rebuildOrder();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const int lineHeight = renderer.getLineHeight(LEXENDDECA_14_FONT_ID);
  const int smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int bulletDiameter = std::max(6, lineHeight * 3 / 5);
  const int textX = metrics.contentSidePadding + bulletDiameter + metrics.contentSidePadding / 2;
  const int maxTextWidth = std::max(1, pageWidth - textX - metrics.contentSidePadding);

  if (rowHeights.empty() && !rows.empty()) {
    remeasureRowHeights(lineHeight, smallLineHeight, maxTextWidth);
  }

  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  char title[48];
  std::snprintf(title, sizeof(title), "%s (%d)", tr(STR_TASK_TITLE), openCount);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, title, false);
  } else {
    GUI.drawHeader(renderer, header, title);
  }

  // Fraicheur de sync : seule l'information qu'on a vraiment (appairee ou
  // non) est affichee ici. Un horodatage relatif ("il y a 2h") demanderait un
  // champ que rien n'ecrit encore -- voir le rapport de la Tache 5.
  const int subHeaderTop = header.y + header.height;
  const char* freshness = TASK_STORE.hasSecret() ? tr(STR_TASK_UP_TO_DATE) : tr(STR_TASK_PAIRING_REQUIRED);
  GUI.drawSubHeader(renderer, Rect{0, subHeaderTop, pageWidth, metrics.tabBarHeight}, "", freshness);

  const int listTop = subHeaderTop + metrics.tabBarHeight;
  const int listBottom = pageHeight - metrics.buttonHintsHeight;
  const int contentHeight = std::max(0, listBottom - listTop);

  scrollTop = taskListClampScrollTop(rowHeights, selected, scrollTop, contentHeight);
  const int visible = taskListVisibleRows(rowHeights, scrollTop, contentHeight);

  int y = listTop;
  const int rowsTotal = static_cast<int>(rows.size());
  for (int i = scrollTop; i < scrollTop + visible && i < rowsTotal; ++i) {
    const TaskListRow& row = rows[i];
    const int rowH = rowHeights[i];

    if (row.kind == TaskRowKind::DoneHeader) {
      char doneHeader[32];
      std::snprintf(doneHeader, sizeof(doneHeader), "%d %s", doneCount, tr(STR_TASK_DONE_COUNT));
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + kRowVPad, doneHeader);
      y += rowH;
      continue;
    }

    const bool isSelected = (i == selected);
    const TaskRecord& rec = records[row.recordIndex];
    const auto style = styleForPriority(rec.priority);

    // Ligne selectionnee : fond noir plein puis texte blanc, jamais un
    // simple filet -- c'est ce qui reste lisible apres le fantomage e-ink.
    if (isSelected) renderer.fillRect(0, y, pageWidth, rowH, true);

    // Puce circulaire : pleine si faite, creuse sinon. GfxRenderer n'a pas
    // de drawCircle/fillCircle ; un rectangle arrondi dont le rayon vaut la
    // moitie du cote en tient lieu.
    const int bulletY = y + rowH / 2 - bulletDiameter / 2;
    const int bulletX = metrics.contentSidePadding;
    if (rec.done) {
      renderer.fillRoundedRect(bulletX, bulletY, bulletDiameter, bulletDiameter, bulletDiameter / 2,
                               isSelected ? Color::White : Color::Black);
    } else {
      renderer.drawRoundedRect(bulletX, bulletY, bulletDiameter, bulletDiameter, 1, bulletDiameter / 2, !isSelected);
    }

    const auto lines = renderer.wrappedText(LEXENDDECA_14_FONT_ID, rec.title, maxTextWidth, /*maxLines=*/2, style);
    int lineY = y + kRowVPad;
    for (const auto& line : lines) {
      renderer.drawText(LEXENDDECA_14_FONT_ID, textX, lineY, line.c_str(), !isSelected, style);
      if (rec.done) {
        // Cochee pendant cette visite ou deja faite avant : dans les deux
        // cas rec.done est vrai et la ligne reste barree en place.
        const int textWidth = renderer.getTextWidth(LEXENDDECA_14_FONT_ID, line.c_str(), style);
        const int strikeY = lineY + lineHeight / 2;
        renderer.drawLine(textX, strikeY, textX + textWidth, strikeY, !isSelected);
      }
      lineY += lineHeight;
    }

    y += rowH;
  }

  char doneLabel[16];
  std::snprintf(doneLabel, sizeof(doneLabel), "\xE2\x98\x91 %d", doneCount);  // "[checkbox] N"
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_TASK_TICK), doneLabel, tr(STR_TASK_DETAIL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
