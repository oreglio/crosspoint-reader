#include "TaskSyncReader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "StreamingJsonParser.h"

// Logging.h n'existe que dans la compilation firmware ; le test hôte compile le
// même .cpp sans Arduino, donc le log s'y réduit à rien.
#ifdef ARDUINO
#include <Logging.h>
#define TASK_LOG_ERR(...) LOG_ERR("TASK", __VA_ARGS__)
#else
#define TASK_LOG_ERR(...) ((void)0)
#endif

namespace {

bool isValidTaskId(const char* id) {
  if (std::strlen(id) != TASK_ID_LEN) return false;
  if (id[0] != 'w' && id[0] != 'd') return false;
  for (size_t i = 1; i < TASK_ID_LEN; i++) {
    const char c = id[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}

// Le curseur repart tel quel dans le corps de la requête suivante, assemblé par
// snprintf : hors de cet alphabet il y injecterait de la syntaxe JSON. Le `=`
// est accepté parce que le serveur padde son base64url.
bool isSafeCursor(const char* cursor) {
  for (const char* p = cursor; *p != '\0'; ++p) {
    const char c = *p;
    const bool ok =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '=';
    if (!ok) return false;
  }
  return true;
}

// Recule la coupe sur une frontière UTF-8 : tronquer au milieu d'un caractère
// laisserait un octet orphelin que le rendu afficherait comme un déchet.
size_t utf8TruncateLen(const char* v, size_t len, size_t cap) {
  if (len <= cap) return len;
  size_t n = cap;
  while (n > 0 && (static_cast<unsigned char>(v[n]) & 0xC0) == 0x80) n--;
  return n;
}

// Collecteur de champs de la ligne courante.
//
// Il suit la profondeur : l'en-tête porte un tableau `rejected` dont les
// entrées ont leurs propres clés `id` et `reason`, et un collecteur qui ne
// retiendrait que la dernière clé vue les laisserait se faire passer pour des
// champs d'en-tête. Les champs de la ligne ne sont donc lus qu'à la profondeur
// 1, et les rejets qu'à la profondeur 2 à l'intérieur de ce tableau.
struct FieldSink {
  // Non nul pour la ligne d'en-tête seulement : chaque rejet est émis dès que
  // son entrée se ferme, ce qui évite de les stocker.
  const TaskSyncCallbacks* cb = nullptr;

  char key[24] = {};
  char id[TASK_ID_LEN + 1] = {};
  char title[TASK_TITLE_MAX + 1] = {};
  char cursor[TASK_SYNC_CURSOR_MAX + 1] = {};
  long schema = -1;
  long priority = TASK_PRIORITY_NORMAL;
  long noteBytes = 0;
  bool done = false;
  bool deleted = false;
  bool more = false;
  bool reset = false;

  // Valeur hors contrat (curseur trop long ou hors alphabet, nombre aberrant,
  // octet nul dans une chaîne) : la ligne entière est refusée.
  bool badValue = false;

  int objDepth = 0;
  int arrayDepth = 0;
  int rejectedArrayDepth = 0;
  bool sawTopObject = false;
  bool structureBroken = false;
  bool inRejected = false;
  char rejId[TASK_ID_LEN + 1] = {};
  char rejReason[16] = {};
};

bool atTopLevel(const FieldSink* f) { return f->objDepth == 1 && f->arrayDepth == 0; }
bool atRejectedEntry(const FieldSink* f) { return f->inRejected && f->objDepth == 2; }
bool keyIs(const FieldSink* f, const char* name) { return std::strcmp(f->key, name) == 0; }

// Une chaîne du réseau ne doit jamais dépasser sa destination ni y glisser un
// octet nul, qui ferait mentir toute mesure ultérieure par strlen.
bool copyBounded(char* dst, size_t dstSize, const char* v, size_t len) {
  if (len >= dstSize) return false;
  if (std::memchr(v, '\0', len) != nullptr) return false;
  std::memcpy(dst, v, len);
  dst[len] = '\0';
  return true;
}

void sinkKey(void* ctx, const char* k, size_t len) {
  auto* f = static_cast<FieldSink*>(ctx);
  const size_t n = len < sizeof(f->key) - 1 ? len : sizeof(f->key) - 1;
  std::memcpy(f->key, k, n);
  f->key[n] = '\0';
}

void sinkString(void* ctx, const char* v, size_t len) {
  auto* f = static_cast<FieldSink*>(ctx);
  if (atRejectedEntry(f)) {
    if (keyIs(f, "id")) {
      if (!copyBounded(f->rejId, sizeof(f->rejId), v, len)) f->rejId[0] = '\0';
    } else if (keyIs(f, "reason")) {
      if (!copyBounded(f->rejReason, sizeof(f->rejReason), v, len)) f->rejReason[0] = '\0';
    }
    f->key[0] = '\0';
    return;
  }
  if (!atTopLevel(f)) {
    f->key[0] = '\0';
    return;
  }
  if (keyIs(f, "id")) {
    // Tronquer un id trop long le ramènerait parfois à une forme valide, donc
    // à accepter une tâche sous une identité qui n'est pas la sienne.
    if (!copyBounded(f->id, sizeof(f->id), v, len)) f->id[0] = '\0';
  } else if (keyIs(f, "title")) {
    if (std::memchr(v, '\0', len) != nullptr) {
      f->badValue = true;
    } else {
      const size_t n = utf8TruncateLen(v, len, TASK_TITLE_MAX);
      std::memcpy(f->title, v, n);
      f->title[n] = '\0';
    }
  } else if (keyIs(f, "cursor")) {
    // Raccourcir un curseur serait une perte de données silencieuse à la
    // synchro suivante : on refuse la réponse plutôt que de le tronquer.
    if (!copyBounded(f->cursor, sizeof(f->cursor), v, len) || !isSafeCursor(f->cursor)) {
      f->badValue = true;
    }
  }
  f->key[0] = '\0';
}

void sinkNumber(void* ctx, const char* v, size_t len) {
  auto* f = static_cast<FieldSink*>(ctx);
  long* slot = nullptr;
  if (atTopLevel(f)) {
    if (keyIs(f, "priority"))
      slot = &f->priority;
    else if (keyIs(f, "noteBytes"))
      slot = &f->noteBytes;
    else if (keyIs(f, "schema"))
      slot = &f->schema;
  }
  f->key[0] = '\0';
  // Les autres nombres ne nous servent pas : `count` est informatif, et refuser
  // la réponse parce qu'il serait écrit autrement casserait la synchro pour
  // rien. Seuls les trois champs dont on se sert doivent être des entiers.
  if (slot == nullptr) return;
  char buf[16];
  if (!copyBounded(buf, sizeof(buf), v, len)) {
    f->badValue = true;
    return;
  }
  char* end = nullptr;
  const long value = std::strtol(buf, &end, 10);
  if (end == buf || *end != '\0') {
    f->badValue = true;
    return;
  }
  *slot = value;
}

void sinkBool(void* ctx, bool value) {
  auto* f = static_cast<FieldSink*>(ctx);
  if (atTopLevel(f)) {
    if (keyIs(f, "done"))
      f->done = value;
    else if (keyIs(f, "deleted"))
      f->deleted = value;
    else if (keyIs(f, "more"))
      f->more = value;
    else if (keyIs(f, "reset"))
      f->reset = value;
  }
  f->key[0] = '\0';
}

void sinkNull(void* ctx) { static_cast<FieldSink*>(ctx)->key[0] = '\0'; }

void sinkObjectStart(void* ctx) {
  auto* f = static_cast<FieldSink*>(ctx);
  f->objDepth++;
  if (f->objDepth == 1) f->sawTopObject = true;
  f->key[0] = '\0';
}

void sinkObjectEnd(void* ctx) {
  auto* f = static_cast<FieldSink*>(ctx);
  if (atRejectedEntry(f)) {
    if (isValidTaskId(f->rejId)) {
      if (f->cb != nullptr && f->cb->onRejected != nullptr) {
        f->cb->onRejected(f->cb->ctx, f->rejId, f->rejReason);
      }
    } else {
      // Sans id lisible, Task 9 ne pourrait de toute façon nommer aucune tâche.
      TASK_LOG_ERR("sync: rejection with a malformed id ignored");
    }
    f->rejId[0] = '\0';
    f->rejReason[0] = '\0';
  }
  f->objDepth--;
  if (f->objDepth < 0) f->structureBroken = true;
  f->key[0] = '\0';
}

void sinkArrayStart(void* ctx) {
  auto* f = static_cast<FieldSink*>(ctx);
  const bool isRejectedArray = atTopLevel(f) && keyIs(f, "rejected");
  f->arrayDepth++;
  if (isRejectedArray) {
    f->inRejected = true;
    f->rejectedArrayDepth = f->arrayDepth;
  }
  f->key[0] = '\0';
}

void sinkArrayEnd(void* ctx) {
  auto* f = static_cast<FieldSink*>(ctx);
  if (f->inRejected && f->arrayDepth == f->rejectedArrayDepth) f->inRejected = false;
  f->arrayDepth--;
  if (f->arrayDepth < 0) f->structureBroken = true;
  f->key[0] = '\0';
}

JsonCallbacks fieldCallbacks(FieldSink& sink) {
  return JsonCallbacks{&sink,    sinkKey,         sinkString,    sinkNumber,     sinkBool,
                       sinkNull, sinkObjectStart, sinkObjectEnd, sinkArrayStart, sinkArrayEnd};
}

// StreamingJsonParser ne signale pas une entrée tronquée : il s'arrête au
// dernier octet reçu sans erreur. On exige donc un objet ouvert puis refermé,
// sinon une ligne coupée passerait pour une ligne complète aux champs absents.
bool parseFlatObject(const char* line, size_t len, FieldSink& sink) {
  StreamingJsonParser parser(fieldCallbacks(sink));
  parser.feed(line, len);
  return !parser.hasError() && sink.sawTopObject && !sink.structureBroken && !sink.badValue && sink.objDepth == 0 &&
         sink.arrayDepth == 0;
}

}  // namespace

void TaskSyncReader::feed(const char* data, size_t len) {
  size_t i = 0;
  while (i < len && !error_) {
    if (mode_ == Mode::NoteBytes) {
      if (noteSwallowNewline_) {
        // Le \n qui clôt le bloc de note n'est pas compté dans noteBytes. S'il
        // manque, le serveur ne cadre pas comme nous : tout ce qui suit serait
        // décalé d'un octet, donc on s'arrête là.
        if (data[i] != '\n') {
          TASK_LOG_ERR("sync: note block not terminated by a newline");
          error_ = true;
          return;
        }
        noteSwallowNewline_ = false;
        mode_ = Mode::Line;
        lineLen_ = 0;
        overflow_ = false;
        i++;
        continue;
      }
      const size_t take = (len - i) < noteRemaining_ ? (len - i) : noteRemaining_;
      noteRemaining_ -= take;
      if (!noteDiscard_ && cb_.onNoteChunk != nullptr && take > 0) {
        cb_.onNoteChunk(cb_.ctx, noteId_, data + i, take, noteRemaining_ == 0);
      }
      i += take;
      if (noteRemaining_ == 0) noteSwallowNewline_ = true;
      continue;
    }

    const char c = data[i++];
    if (c == '\n') {
      const size_t lineLen = lineLen_;
      const bool dropped = overflow_;
      lineLen_ = 0;
      overflow_ = false;
      consumeLine(lineLen, dropped);
      continue;
    }
    if (lineLen_ < LINE_BUF_SIZE - 1) {
      line_[lineLen_++] = c;
    } else {
      overflow_ = true;
    }
  }
}

void TaskSyncReader::consumeLine(size_t len, bool dropped) {
  if (dropped) {
    // On a jeté le début de la ligne, donc on ne peut plus savoir si un bloc de
    // note la suit : le cadrage est perdu.
    TASK_LOG_ERR("sync: overlong line dropped");
    error_ = true;
    return;
  }
  if (len == 0) return;
  if (!sawHeader_) {
    parseHeaderLine(len);
    return;
  }
  parseTaskLine(len);
}

void TaskSyncReader::parseHeaderLine(size_t len) {
  FieldSink sink;
  sink.cb = &cb_;
  if (!parseFlatObject(line_, len, sink)) {
    TASK_LOG_ERR("sync: unreadable header line");
    error_ = true;
    return;
  }
  if (sink.schema != TASK_SYNC_SCHEMA) {
    TASK_LOG_ERR("sync: unsupported schema %ld", sink.schema);
    error_ = true;
    return;
  }
  sawHeader_ = true;
  if (cb_.onHeader != nullptr) cb_.onHeader(cb_.ctx, sink.cursor, sink.more, sink.reset);
}

void TaskSyncReader::parseTaskLine(size_t len) {
  FieldSink sink;
  if (!parseFlatObject(line_, len, sink)) {
    // Ligne illisible : noteBytes n'est pas fiable, donc on ne sait plus si des
    // octets bruts suivent. Continuer, ce serait relire une note comme du JSON.
    TASK_LOG_ERR("sync: unreadable task line");
    error_ = true;
    return;
  }
  if (sink.noteBytes < 0 || static_cast<size_t>(sink.noteBytes) > TASK_NOTE_MAX) {
    TASK_LOG_ERR("sync: noteBytes out of range (%ld)", sink.noteBytes);
    error_ = true;
    return;
  }
  const size_t noteBytes = static_cast<size_t>(sink.noteBytes);

  if (!isValidTaskId(sink.id)) {
    TASK_LOG_ERR("sync: task line with a malformed id skipped");
    beginNoteBlock(nullptr, noteBytes);
    return;
  }
  if (sink.deleted) {
    if (cb_.onDeleted != nullptr) cb_.onDeleted(cb_.ctx, sink.id);
    beginNoteBlock(nullptr, noteBytes);
    return;
  }

  TaskRecord rec{};
  std::snprintf(rec.id, sizeof(rec.id), "%s", sink.id);
  std::snprintf(rec.title, sizeof(rec.title), "%s", sink.title);
  rec.priority = (sink.priority >= TASK_PRIORITY_HIGH && sink.priority <= TASK_PRIORITY_LOW)
                     ? static_cast<uint8_t>(sink.priority)
                     : TASK_PRIORITY_NORMAL;
  rec.done = sink.done;
  rec.noteBytes = static_cast<uint16_t>(noteBytes);
  if (cb_.onTask != nullptr) cb_.onTask(cb_.ctx, rec);

  beginNoteBlock(rec.id, noteBytes);
}

void TaskSyncReader::beginNoteBlock(const char* id, size_t bytes) {
  if (bytes == 0) return;
  // Même quand la tâche est refusée, le bloc doit être consommé : sinon ses
  // octets bruts repartiraient dans le découpage en lignes.
  noteDiscard_ = (id == nullptr);
  if (id != nullptr) {
    std::snprintf(noteId_, sizeof(noteId_), "%s", id);
  } else {
    noteId_[0] = '\0';
  }
  noteRemaining_ = bytes;
  noteSwallowNewline_ = false;
  mode_ = Mode::NoteBytes;
}
