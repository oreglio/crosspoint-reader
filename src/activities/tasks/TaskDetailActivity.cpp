#include "TaskDetailActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "TaskStore.h"
#include "activities/tasks/TaskNotePager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "activities/util/PomodoroActivity.h"
#include "activities/util/PomodoroSchedule.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char TAG[] = "TASKDETAIL";

// Deux paliers, jamais trois. EpdFontFamily::Style n'offre que REGULAR / BOLD
// / ITALIC / BOLD_ITALIC : il n'y a pas de Light, et ITALIC ne peut pas tenir
// le palier bas — sur un panneau 1 bit l'italique attire l'oeil au lieu de
// l'apaiser, ce qui inverse l'intention. Seule l'exception se marque ; le tri
// de la liste (taskOrderBefore) place deja la priorite basse en fin de liste.
// C'est exactement ce que fait ListItem::emphasis sur l'ecran de liste.
EpdFontFamily::Style titleStyleFor(const TaskRecord& record) {
  return record.priority == TASK_PRIORITY_HIGH ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
}

StrId priorityLabelId(const uint8_t priority) {
  if (priority == TASK_PRIORITY_HIGH) return StrId::STR_TASK_PRIORITY_HIGH;
  if (priority == TASK_PRIORITY_LOW) return StrId::STR_TASK_PRIORITY_LOW;
  return StrId::STR_TASK_PRIORITY_NORMAL;
}

// Les trois valeurs de TaskRecord::priority, dans l'ordre ou elles sont
// proposees — l'indice rendu par OptionSelectionActivity est un rang dans
// cette table, pas une priorite.
constexpr uint8_t kPriorityChoices[] = {TASK_PRIORITY_HIGH, TASK_PRIORITY_NORMAL, TASK_PRIORITY_LOW};

constexpr int kFramePadding = 10;
constexpr int kBlockGap = 8;
constexpr size_t kTitleMaxLines = 2;
}  // namespace

TaskDetailActivity::TaskDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* taskId)
    : Activity("TaskDetail", renderer, mappedInput), taskId(taskId != nullptr ? taskId : "") {}

// ---------------------------------------------------------------- lifecycle

void TaskDetailActivity::onEnter() {
  // Un seul verrou sur le cycle de vie de base ET la phase de donnees : le
  // onEnter de base programme un rendu, et la tache de rendu ne doit pas lire
  // la geometrie ni le tampon de note avant qu'ils soient en place (meme
  // rituel que TaskListActivity::onEnter).
  RenderLock lock(*this);
  Activity::onEnter();

  // La liste en dessous appelle unload() dans SON onExit, donc le store peut
  // tres bien etre decharge ici apres un aller-retour. ensureLoaded() relit la
  // carte au besoin. Cet ecran n'appelle JAMAIS unload() : la liste tient des
  // indices dans ce vecteur.
  TASK_STORE.ensureLoaded();

  if (!refreshRecord()) {
    LOG_ERR(TAG, "No task with id %s in the index; closing the detail screen", taskId.c_str());
    finish();
    return;
  }

  computeLayout();
  rebuildTitleLines();
  if (allocateNotePool()) {
    paginateNote();
    loadPage(0);
  }
}

void TaskDetailActivity::onExit() {
  // Rien ne reste resident apres l'ecran. Le fichier de note, lui, n'a pas a
  // etre ferme ici : il n'est jamais detenu entre deux images — paginateNote()
  // et loadPage() l'ouvrent et le referment chacun dans leur propre portee.
  notePool.reset();
  notePoolCapacity = 0;
  notePoolLength = 0;
  lineCount = 0;
  pageOffsets.clear();
  pageOffsets.shrink_to_fit();
  titleLines.clear();
  titleLines.shrink_to_fit();
  // Surtout PAS TASK_STORE.unload() : TaskListActivity est encore sur la pile
  // et indexe ce vecteur. C'est son onExit() qui possede le dechargement.
  Activity::onExit();
}

bool TaskDetailActivity::refreshRecord() {
  const TaskRecord* found = TASK_STORE.find(taskId.c_str());
  hasRecord = found != nullptr;
  if (hasRecord) record = *found;
  return hasRecord;
}

// ------------------------------------------------------------------- layout

void TaskDetailActivity::computeLayout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);

  contentX = safe.x + metrics.contentSidePadding;
  contentWidth = std::max(1, safe.width - 2 * metrics.contentSidePadding);
  titleTop = header.y + header.height + metrics.verticalSpacing;

  const int titleHeight = renderer.getLineHeight(LEXENDDECA_16_FONT_ID) * static_cast<int>(kTitleMaxLines);

  // Bloc pomodoro ancre au-dessus des indices de boutons, jamais au fil du
  // texte : c'est une action, pas une ligne de la note.
  pomodoroHeight = renderer.getLineHeight(LEXENDDECA_12_FONT_ID) + 2 * kFramePadding;
  pomodoroX = contentX;
  pomodoroWidth = contentWidth;
  pomodoroY = safe.y + safe.height - metrics.buttonHintsHeight - metrics.verticalSpacing - pomodoroHeight;

  statusTop = titleTop + titleHeight + kBlockGap;

  noteX = contentX;
  noteY = statusTop + renderer.getLineHeight(SMALL_FONT_ID) + kBlockGap;
  noteWidth = contentWidth;
  noteHeight = std::max(0, pomodoroY - kBlockGap - noteY);

  noteAdvanceY = renderer.getLineHeight(LEXENDDECA_10_FONT_ID);
  noteLinesPerPage = std::min(static_cast<int>(kNoteMaxLines), taskNoteLinesPerPage(noteHeight, noteAdvanceY));
}

void TaskDetailActivity::rebuildTitleLines() {
  // Enroule une fois par entree (et apres une modification), pas par image :
  // render() tourne sur la tache de rendu et ne doit pas allouer par frame.
  titleLines = renderer.wrappedText(LEXENDDECA_16_FONT_ID, record.title, contentWidth, static_cast<int>(kTitleMaxLines),
                                    titleStyleFor(record));
}

bool TaskDetailActivity::allocateNotePool() {
  // Dimensionne a partir de la region : elle ne peut pas montrer plus de
  // noteLinesPerPage lignes, et une ligne ne peut pas contenir plus de
  // noteWidth / kNarrowestAdvancePx caracteres. Le produit est ecrete pour que
  // la RAM ne suive pas la largeur de l'ecran ; une page qui deborderait le
  // tampon se termine simplement plus tot et la note gagne une page.
  const size_t widest = static_cast<size_t>(std::max(1, noteWidth / kNarrowestAdvancePx));
  const size_t wanted = widest * static_cast<size_t>(std::max(1, noteLinesPerPage));
  notePoolCapacity = std::min(kNotePoolMaxBytes, std::max(kNotePoolMinBytes, wanted));

  // +1 octet jamais rempli : measureNotePrefix() termine la chaine en place a
  // window[prefixLen], et prefixLen peut valoir toute la longueur lue.
  notePool = makeUniqueNoThrow<char[]>(notePoolCapacity + 1);
  if (!notePool) {
    LOG_ERR(TAG, "Could not allocate the %u-byte note buffer", static_cast<unsigned>(notePoolCapacity + 1));
    notePoolCapacity = 0;
    noteState = NoteState::Unreadable;
    return false;
  }
  return true;
}

// --------------------------------------------------------------------- note

int TaskDetailActivity::measureNotePrefix(void* ctx, const char* window, const size_t prefixLen) {
  auto* self = static_cast<TaskDetailActivity*>(ctx);
  // Terminaison temporaire en place : aucune copie par mesure. `window` pointe
  // toujours dans notePool, que cette activite possede — le const_cast porte
  // sur sa propre memoire (meme procede que KeyboardEntryActivity::measureRange).
  char* mutableWindow = const_cast<char*>(window);
  const char saved = mutableWindow[prefixLen];
  mutableWindow[prefixLen] = '\0';
  const int width = self->renderer.getTextWidth(LEXENDDECA_10_FONT_ID, mutableWindow, EpdFontFamily::REGULAR);
  mutableWindow[prefixLen] = saved;
  return width;
}

size_t TaskDetailActivity::wrapLoadedWindow(const bool windowIsFinal, const bool keepLines) {
  if (keepLines) lineCount = 0;

  size_t consumed = 0;
  for (int line = 0; line < noteLinesPerPage && consumed < notePoolLength; line++) {
    const TaskNoteLineBreak brk = taskNoteNextLine(notePool.get() + consumed, notePoolLength - consumed, windowIsFinal,
                                                   noteWidth, &TaskDetailActivity::measureNotePrefix, this);
    if (brk.skipBytes == 0) break;  // le pager garantit la progression; ceinture et bretelles
    if (keepLines && lineCount < kNoteMaxLines) {
      lineStart[lineCount] = static_cast<uint16_t>(consumed);
      lineLength[lineCount] = brk.drawBytes;
      lineCount++;
    }
    consumed += brk.skipBytes;
  }
  return consumed;
}

void TaskDetailActivity::paginateNote() {
  pageOffsets.clear();
  noteState = NoteState::Empty;
  if (!notePool) return;

  char path[64];
  if (!TaskStore::notePath(record.id, path, sizeof(path))) {
    noteState = NoteState::Unreadable;
    return;
  }
  if (record.noteBytes == 0 || !Storage.exists(path)) return;  // pas de note : ce n'est pas une erreur

  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    noteState = NoteState::Unreadable;
    return;
  }

  // A partir d'ici et jusqu'au close(), aucun `return` : c'est ce qui rend la
  // fermeture du handle verifiable a la lecture. SdFat n'autorise qu'un
  // handle par chemin sur le materiel, et aucun test de cette branche ne peut
  // attraper une fuite.
  // Ecrete a TASK_NOTE_MAX : c'est ce que la sync ecrit au plus, mais le
  // fichier vit sur une carte editable a la main. L'ecretage garde aussi les
  // offsets dans un uint16_t (4096 < 65535).
  const size_t total = std::min<size_t>(file.size(), TASK_NOTE_MAX);
  bool ok = true;
  size_t offset = 0;
  while (offset < total && pageOffsets.size() < kMaxNotePages) {
    pageOffsets.push_back(static_cast<uint16_t>(offset));
    if (!file.seekSet(offset)) {
      ok = false;
      break;
    }
    const int got = file.read(notePool.get(), std::min(notePoolCapacity, total - offset));
    if (got <= 0) {
      // Rien a lire la ou le fichier annoncait des octets : pas de page vide
      // en queue, et une lecture en erreur (< 0) reste une erreur.
      pageOffsets.pop_back();
      ok = got == 0;
      break;
    }
    const bool windowIsFinal = offset + static_cast<size_t>(got) >= total;
    notePoolLength =
        windowIsFinal ? static_cast<size_t>(got) : taskNoteCompleteUtf8Prefix(notePool.get(), static_cast<size_t>(got));
    const size_t consumed = wrapLoadedWindow(windowIsFinal, /*keepLines=*/false);
    if (consumed == 0) {
      pageOffsets.pop_back();
      break;
    }
    offset += consumed;
  }
  file.close();

  if (!ok) {
    LOG_ERR(TAG, "Could not read the note of %s while paginating", record.id);
    pageOffsets.clear();
    noteState = NoteState::Unreadable;
    return;
  }
  if (pageOffsets.size() >= kMaxNotePages && offset < total) {
    LOG_ERR(TAG, "Note of %s exceeds %u pages; the tail is not shown", record.id, static_cast<unsigned>(kMaxNotePages));
  }
  noteState = pageOffsets.empty() ? NoteState::Empty : NoteState::Ready;
}

void TaskDetailActivity::loadPage(const size_t page) {
  lineCount = 0;
  notePoolLength = 0;
  if (!notePool || noteState != NoteState::Ready || page >= pageOffsets.size()) return;
  currentPage = page;

  char path[64];
  if (!TaskStore::notePath(record.id, path, sizeof(path))) {
    noteState = NoteState::Unreadable;
    return;
  }

  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) {
    noteState = NoteState::Unreadable;
    return;
  }

  // Meme discipline que paginateNote() : aucun `return` entre l'ouverture et
  // le close().
  const size_t total = std::min<size_t>(file.size(), TASK_NOTE_MAX);
  const size_t offset = pageOffsets[page];
  bool ok = offset < total && file.seekSet(offset);
  if (ok) {
    const int got = file.read(notePool.get(), std::min(notePoolCapacity, total - offset));
    if (got < 0) {
      ok = false;
    } else {
      const bool windowIsFinal = offset + static_cast<size_t>(got) >= total;
      notePoolLength = windowIsFinal ? static_cast<size_t>(got)
                                     : taskNoteCompleteUtf8Prefix(notePool.get(), static_cast<size_t>(got));
    }
  }
  file.close();

  if (!ok) {
    LOG_ERR(TAG, "Could not read page %u of the note of %s", static_cast<unsigned>(page), record.id);
    notePoolLength = 0;
    noteState = NoteState::Unreadable;
    return;
  }
  wrapLoadedWindow(offset + notePoolLength >= total, /*keepLines=*/true);
}

void TaskDetailActivity::showPage(const size_t page) {
  if (noteState != NoteState::Ready || pageOffsets.empty()) return;
  const size_t clamped = std::min(page, pageOffsets.size() - 1);
  if (clamped == currentPage) return;
  // loadPage() ecrit dans notePool et dans la table de lignes que render() lit
  // depuis la tache de rendu : la mutation prend le verrou de rendu.
  {
    RenderLock lock(*this);
    loadPage(clamped);
  }
  requestUpdate();
}

// -------------------------------------------------------------------- input

void TaskDetailActivity::loop() {
  if (!hasRecord) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
#if CROSSINK_APP_CAP_TOUCH
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finish();
    return;
  }
#endif
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleDone();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    editTitle();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    startPomodoro();
    return;
  }
  // Haut/Bas ne sont pas dans la barre de boutons (mapLabels n'en porte pas),
  // ils sont donc libres pour la pagination de la note.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (currentPage > 0) showPage(currentPage - 1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    showPage(currentPage + 1);
    return;
  }
}

// Ni celle-ci ni rebuildTitleLines() ne prennent le verrou de rendu : elles
// sont appelees aussi bien depuis onEnter(), qui le detient deja, que depuis
// un gestionnaire de resultat, qui ne le detient pas. renderingMutex est un
// xSemaphoreCreateMutex() NON recursif — un verrou imbrique serait un
// interblocage, pas une precaution. Ce sont donc les appelants qui verrouillent.
bool TaskDetailActivity::appendAndApply(const TaskOp& op, const TaskRecord& next) {
  // La file d'ops AVANT l'index, jamais l'inverse : ce sont deux ecritures SD
  // distinctes, et une coupure entre les deux doit laisser un etat reparable.
  // Une op en file sans changement local l'est (la prochaine sync l'applique) ;
  // un index modifie avec une file vide ne l'est pas. Meme raisonnement que
  // TaskListActivity::toggleAt().
  if (!TASK_STORE.appendOp(op)) {
    LOG_ERR(TAG, "Could not queue the edit for %s; nothing applied", next.id);
    writeFailed = true;
    requestUpdate();
    return false;
  }
  writeFailed = false;
  TASK_STORE.upsert(next);
  record = next;
  return true;
}

void TaskDetailActivity::toggleDone() {
  TaskRecord next = record;
  next.done = !next.done;

  TaskOp op{};
  op.kind = TaskOpKind::Done;
  std::snprintf(op.id, sizeof(op.id), "%s", next.id);
  op.done = next.done;

  // `record` et `writeFailed` sont lus par render() sur la tache de rendu.
  RenderLock lock(*this);
  if (appendAndApply(op, next)) requestUpdate();
}

void TaskDetailActivity::editTitle() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TASK_EDIT_TITLE), record.title,
                                              TASK_TITLE_MAX, InputType::Text, /*minLength=*/1),
      [this](const ActivityResult& result) {
        // L'enfant a repeint tout le panneau : le retour se fait en passe
        // complete, sans quoi le residu du clavier reste visible.
        pendingFullRefresh = true;
        const auto* entered = std::get_if<KeyboardResult>(&result.data);
        // Annuler le titre annule toute la modification : la priorite n'est
        // pas une seconde chance apres un retour en arriere.
        if (result.isCancelled || entered == nullptr) return;

        if (std::strcmp(entered->text.c_str(), record.title) != 0) {
          TaskRecord next = record;
          std::snprintf(next.title, sizeof(next.title), "%s", entered->text.c_str());
          TaskOp op{};
          op.kind = TaskOpKind::Title;
          std::snprintf(op.id, sizeof(op.id), "%s", next.id);
          std::snprintf(op.title, sizeof(op.title), "%s", next.title);
          // Portee explicite : le verrou tombe avant editPriority(), qui
          // empile un ecran et ne doit pas le faire verrou tenu.
          {
            RenderLock lock(*this);
            if (!appendAndApply(op, next)) return;
            rebuildTitleLines();
          }
        }
        editPriority();
      });
}

void TaskDetailActivity::editPriority() {
  std::vector<std::string> rows;
  rows.reserve(3);
  uint8_t selected = 1;
  for (size_t i = 0; i < 3; i++) {
    rows.emplace_back(I18N.get(priorityLabelId(kPriorityChoices[i])));
    if (kPriorityChoices[i] == record.priority) selected = static_cast<uint8_t>(i);
  }

  startActivityForResult(std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "TaskPriority",
                                                                   StrId::STR_TASK_PRIORITY, std::move(rows), selected),
                         [this](const ActivityResult& result) {
                           pendingFullRefresh = true;
                           const auto* choice = std::get_if<OptionSelectionResult>(&result.data);
                           if (result.isCancelled || choice == nullptr || choice->index >= 3) return;

                           const uint8_t priority = kPriorityChoices[choice->index];
                           if (priority == record.priority) return;

                           TaskRecord next = record;
                           next.priority = priority;
                           TaskOp op{};
                           op.kind = TaskOpKind::Prio;
                           std::snprintf(op.id, sizeof(op.id), "%s", next.id);
                           op.priority = priority;

                           RenderLock lock(*this);
                           if (!appendAndApply(op, next)) return;
                           // La graisse du titre porte la priorite haute : la
                           // changer rend l'enroulement precedent caduc.
                           rebuildTitleLines();
                         });
}

void TaskDetailActivity::startPomodoro() {
  // Le titre est passe en contexte, rien de plus : le pomodoro ne compte ni ne
  // synchronise quoi que ce soit pour la tache.
  startActivityForResult(std::make_unique<PomodoroActivity>(renderer, mappedInput, record.title),
                         [this](const ActivityResult&) { pendingFullRefresh = true; });
}

// ------------------------------------------------------------------- render

void TaskDetailActivity::drawNote() {
  if (noteState == NoteState::Ready && lineCount > 0) {
    int y = noteY;
    for (size_t i = 0; i < lineCount; i++) {
      if (lineLength[i] > 0) {
        // Terminaison temporaire en place, comme pour la mesure : pas une
        // std::string par ligne et par image.
        char* line = notePool.get() + lineStart[i];
        const char saved = line[lineLength[i]];
        line[lineLength[i]] = '\0';
        renderer.drawText(LEXENDDECA_10_FONT_ID, noteX, y, line, true, EpdFontFamily::REGULAR);
        line[lineLength[i]] = saved;
      }
      y += noteAdvanceY;
    }
    return;
  }

  const char* message = noteState == NoteState::Unreadable ? tr(STR_TASK_NOTE_UNREADABLE) : tr(STR_TASK_NOTE_EMPTY);
  renderer.drawText(SMALL_FONT_ID, noteX, noteY, message, true);
}

void TaskDetailActivity::drawPomodoroBlock() const {
  renderer.drawRect(pomodoroX, pomodoroY, pomodoroWidth, pomodoroHeight, true);

  char duration[16];
  std::snprintf(duration, sizeof(duration), I18N.get(StrId::STR_SLEEP_TIMER_VALUE_FORMAT),
                static_cast<unsigned>(PomodoroSchedule::clamp(APP_STATE.pomodoroWorkMinutes)));
  char series[32];
  // Position dans la serie. Elle vaut toujours 1 depuis cet ecran : une
  // session demarre au premier pas et PomodoroSchedule ne persiste rien entre
  // deux visites. La montrer dit ce qui va reellement se passer plutot que de
  // laisser croire a un compteur qui n'existe pas.
  std::snprintf(series, sizeof(series), I18N.get(StrId::STR_TASK_POMODORO_SERIES), PomodoroSchedule::pomodoroNumber(0),
                PomodoroSchedule::kPomodorosPerLongBreak);

  const int textY = pomodoroY + kFramePadding;
  renderer.drawText(LEXENDDECA_12_FONT_ID, pomodoroX + kFramePadding, textY, duration, true);
  const int seriesWidth = renderer.getTextWidth(SMALL_FONT_ID, series);
  renderer.drawText(SMALL_FONT_ID, pomodoroX + pomodoroWidth - kFramePadding - seriesWidth, textY, series, true);
}

void TaskDetailActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  char pageLabel[16];
  const size_t pageCount = std::max<size_t>(1, pageOffsets.size());
  std::snprintf(pageLabel, sizeof(pageLabel), "%u / %u", static_cast<unsigned>(currentPage + 1),
                static_cast<unsigned>(pageCount));
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_TASK_TITLE), false, 0, pageLabel);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_TASK_TITLE), pageLabel);
  }

  int y = titleTop;
  const EpdFontFamily::Style titleStyle = titleStyleFor(record);
  for (const std::string& line : titleLines) {
    renderer.drawText(LEXENDDECA_16_FONT_ID, contentX, y, line.c_str(), true, titleStyle);
    y += renderer.getLineHeight(LEXENDDECA_16_FONT_ID);
  }

  // Une seule ligne d'etat : l'echec d'ecriture prime, sinon l'etat "faite".
  // Rien a dire quand la tache est ouverte et que tout s'est bien passe.
  const char* status = writeFailed ? tr(STR_TASK_TICK_FAILED) : (record.done ? tr(STR_TASK_STATE_DONE) : nullptr);
  if (status != nullptr) renderer.drawText(SMALL_FONT_ID, contentX, statusTop, status, true);

  drawNote();
  drawPomodoroBlock();

  char editLabel[24];
  std::snprintf(editLabel, sizeof(editLabel), "\xE2\x89\xA1 %s", tr(STR_TASK_EDIT));  // "[identical to] modifier"
  char pomodoroLabel[24];
  std::snprintf(pomodoroLabel, sizeof(pomodoroLabel), "\xE2\x96\xB8 %s", tr(STR_TASK_POMODORO));  // "[play] pomodoro"
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_TASK_TICK), editLabel, pomodoroLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (++repaintsSinceFullRefresh >= kRepaintsPerFullRefresh) {
    repaintsSinceFullRefresh = 0;
    pendingFullRefresh = true;
  }
  renderer.displayBuffer(pendingFullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  pendingFullRefresh = false;
}
