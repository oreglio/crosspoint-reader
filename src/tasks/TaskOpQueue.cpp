#include "TaskOpQueue.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "StreamingJsonParser.h"

namespace {

const char* kindName(const TaskOpKind kind) {
  switch (kind) {
    case TaskOpKind::Add:
      return "add";
    case TaskOpKind::Done:
      return "done";
    case TaskOpKind::Prio:
      return "prio";
    case TaskOpKind::Title:
      return "title";
  }
  return "add";
}

bool kindFromName(const char* name, TaskOpKind& out) {
  if (std::strcmp(name, "add") == 0) {
    out = TaskOpKind::Add;
    return true;
  }
  if (std::strcmp(name, "done") == 0) {
    out = TaskOpKind::Done;
    return true;
  }
  if (std::strcmp(name, "prio") == 0) {
    out = TaskOpKind::Prio;
    return true;
  }
  if (std::strcmp(name, "title") == 0) {
    out = TaskOpKind::Title;
    return true;
  }
  return false;
}

// Echappe pour du JSON : guillemets et antislashs sont doubles. Un caractere
// de controle est remplace par une espace plutot que par un \u0000 — le
// StreamingJsonParser partage ne decode pas les echappements \uXXXX, il les
// recopie tels quels (cf lib/JsonParser/StreamingJsonParser.cpp) ; les coder
// ainsi romprait le aller-retour par taskOpFromLine sans jamais rien
// corrompre cote serveur, qui lui les decoderait correctement. Une espace
// reste correcte des deux cotes et rejoint de toute facon ce que le serveur
// fait deja des tabulations et retours a la ligne dans un titre.
bool appendEscaped(const char* src, char* out, size_t outSize, size_t& pos) {
  for (const char* p = src; *p != '\0'; p++) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') {
      if (pos + 2 >= outSize) return false;
      out[pos++] = '\\';
      out[pos++] = static_cast<char>(c);
    } else if (c < 0x20) {
      if (pos + 1 >= outSize) return false;
      out[pos++] = ' ';
    } else {
      if (pos + 1 >= outSize) return false;
      out[pos++] = static_cast<char>(c);
    }
  }
  return true;
}

// Collecteur d'une ligne d'op. Une ligne est toujours un objet plat : pas de
// tableau, pas d'objet imbrique. objDepth sert donc a la fois a ignorer une
// cle/valeur hors de ce niveau et a verifier, a la fin, que l'objet a bien ete
// refermé plutot que coupe en plein milieu.
struct OpSink {
  char key[16] = {};
  char op[12] = {};
  char id[TASK_ID_LEN + 1] = {};
  char title[TASK_TITLE_MAX + 1] = {};
  long priority = TASK_PRIORITY_NORMAL;
  bool done = false;
  bool sawOp = false;
  // Valeur hors gabarit (id ou titre trop long, nombre illisible) : la ligne
  // entiere est refusee plutot que tronquee.
  bool badValue = false;
  bool structureBroken = false;
  int objDepth = 0;
};

void opKey(void* ctx, const char* k, size_t len) {
  auto* s = static_cast<OpSink*>(ctx);
  const size_t n = len < sizeof(s->key) - 1 ? len : sizeof(s->key) - 1;
  std::memcpy(s->key, k, n);
  s->key[n] = '\0';
}

void opString(void* ctx, const char* v, size_t len) {
  auto* s = static_cast<OpSink*>(ctx);
  if (s->objDepth != 1) {
    s->key[0] = '\0';
    return;
  }
  if (std::strcmp(s->key, "op") == 0) {
    const size_t n = len < sizeof(s->op) - 1 ? len : sizeof(s->op) - 1;
    std::memcpy(s->op, v, n);
    s->op[n] = '\0';
    s->sawOp = true;
  } else if (std::strcmp(s->key, "id") == 0) {
    // Tronquer un id trop long le ramenerait parfois a une forme valide, donc
    // a ecrire au mauvais endroit apres une ligne corrompue.
    if (len > TASK_ID_LEN) {
      s->badValue = true;
    } else {
      std::memcpy(s->id, v, len);
      s->id[len] = '\0';
    }
  } else if (std::strcmp(s->key, "title") == 0) {
    if (len > TASK_TITLE_MAX) {
      s->badValue = true;
    } else {
      std::memcpy(s->title, v, len);
      s->title[len] = '\0';
    }
  }
  s->key[0] = '\0';
}

void opNumber(void* ctx, const char* v, size_t len) {
  auto* s = static_cast<OpSink*>(ctx);
  if (s->objDepth != 1 || std::strcmp(s->key, "priority") != 0) {
    s->key[0] = '\0';
    return;
  }
  char buf[16];
  if (len >= sizeof(buf)) {
    s->badValue = true;
    s->key[0] = '\0';
    return;
  }
  std::memcpy(buf, v, len);
  buf[len] = '\0';
  char* end = nullptr;
  const long value = std::strtol(buf, &end, 10);
  if (end == buf || *end != '\0') {
    s->badValue = true;
  } else {
    s->priority = value;
  }
  s->key[0] = '\0';
}

void opBool(void* ctx, bool value) {
  auto* s = static_cast<OpSink*>(ctx);
  if (s->objDepth == 1 && std::strcmp(s->key, "done") == 0) s->done = value;
  s->key[0] = '\0';
}

void opNull(void* ctx) { static_cast<OpSink*>(ctx)->key[0] = '\0'; }

void opObjectStart(void* ctx) {
  auto* s = static_cast<OpSink*>(ctx);
  s->objDepth++;
  if (s->objDepth > 1) s->structureBroken = true;  // une ligne d'op est toujours plate
  s->key[0] = '\0';
}

void opObjectEnd(void* ctx) {
  auto* s = static_cast<OpSink*>(ctx);
  s->objDepth--;
  if (s->objDepth < 0) s->structureBroken = true;
  s->key[0] = '\0';
}

void opArrayStart(void* ctx) {
  auto* s = static_cast<OpSink*>(ctx);
  s->structureBroken = true;  // aucun tableau n'est attendu dans une ligne d'op
  s->key[0] = '\0';
}

void opArrayEnd(void* ctx) { static_cast<OpSink*>(ctx)->key[0] = '\0'; }

}  // namespace

size_t taskOpToLine(const TaskOp& op, char* out, size_t outSize) {
  const int head = std::snprintf(out, outSize, "{\"op\":\"%s\",\"id\":\"%s\"", kindName(op.kind), op.id);
  if (head < 0 || static_cast<size_t>(head) >= outSize) return 0;
  size_t pos = static_cast<size_t>(head);

  if (op.kind == TaskOpKind::Add || op.kind == TaskOpKind::Title) {
    const int lead = std::snprintf(out + pos, outSize - pos, ",\"title\":\"");
    if (lead < 0 || pos + static_cast<size_t>(lead) >= outSize) return 0;
    pos += static_cast<size_t>(lead);
    if (!appendEscaped(op.title, out, outSize, pos)) return 0;
    if (pos + 1 >= outSize) return 0;
    out[pos++] = '"';
  }
  if (op.kind == TaskOpKind::Add || op.kind == TaskOpKind::Prio) {
    const int p = std::snprintf(out + pos, outSize - pos, ",\"priority\":%u", op.priority);
    if (p < 0 || pos + static_cast<size_t>(p) >= outSize) return 0;
    pos += static_cast<size_t>(p);
  }
  if (op.kind == TaskOpKind::Done) {
    const int d = std::snprintf(out + pos, outSize - pos, ",\"done\":%s", op.done ? "true" : "false");
    if (d < 0 || pos + static_cast<size_t>(d) >= outSize) return 0;
    pos += static_cast<size_t>(d);
  }
  if (pos + 2 > outSize) return 0;
  out[pos++] = '}';
  out[pos] = '\0';
  return pos;
}

size_t taskOpToRecord(const TaskOp& op, char* out, size_t outSize) {
  if (outSize < 3) return 0;
  out[0] = '\n';
  // taskOpToLine() pose un terminateur apres la ligne : il est ecrase par le
  // '\n' final, d'ou la place d'un octet gardee en fin de tampon.
  const size_t len = taskOpToLine(op, out + 1, outSize - 2);
  if (len == 0) return 0;
  out[1 + len] = '\n';
  return len + 2;
}

bool TaskOpLineSplitter::feed(const char byte, TaskOp& out) {
  if (byte == '\r') return false;  // jamais ecrit par la file, tolere en lecture
  if (byte != '\n') {
    if (len_ < sizeof(line_)) {
      line_[len_++] = byte;
    } else {
      overflow_ = true;
    }
    return false;
  }
  const bool usable = !overflow_ && len_ > 0 && taskOpFromLine(line_, len_, out);
  lastTooLong_ = overflow_;
  len_ = 0;
  overflow_ = false;
  return usable;
}

bool taskOpFromLine(const char* line, const size_t len, TaskOp& out) {
  OpSink sink;
  const JsonCallbacks cb{&sink,  opKey,         opString,    opNumber,     opBool,
                         opNull, opObjectStart, opObjectEnd, opArrayStart, opArrayEnd};
  StreamingJsonParser parser(cb);
  parser.feed(line, len);
  // hasError() ne couvre pas une ligne tronquee : une chaine ou un objet
  // jamais refermes s'arretent juste au dernier octet recu, sans lever
  // d'erreur. C'est exactement ce que laisserait une coupure de courant en
  // plein write() du fichier de file d'attente — objDepth doit donc etre
  // revenu a 0 pour que la ligne soit acceptee.
  //
  // tokenTruncated() couvre un trou distinct : un champ de plus de
  // TOKEN_BUF_SIZE octets ne remonte jamais jusqu'a opString/opNumber (voir
  // le commentaire de tokenTruncated()), donc badValue ne peut pas le voir
  // non plus — un champ qu'on ne peut pas lire est un champ qu'on ne peut
  // pas valider, la ligne est refusee plutot qu'acceptee avec un champ vide.
  if (parser.hasError() || parser.tokenTruncated() || sink.badValue || sink.structureBroken || !sink.sawOp ||
      sink.objDepth != 0) {
    return false;
  }

  TaskOpKind kind{};
  if (!kindFromName(sink.op, kind)) return false;
  out = TaskOp{};
  out.kind = kind;
  std::snprintf(out.id, sizeof(out.id), "%s", sink.id);
  std::snprintf(out.title, sizeof(out.title), "%s", sink.title);
  out.priority = (sink.priority == TASK_PRIORITY_HIGH || sink.priority == TASK_PRIORITY_LOW)
                     ? static_cast<uint8_t>(sink.priority)
                     : TASK_PRIORITY_NORMAL;
  out.done = sink.done;
  return true;
}

size_t taskOpsToRequestBody(const TaskOp* ops, const size_t count, const char* cursor, char* out,
                            const size_t outSize) {
  // Cette fonction ne peut pas dire a l'appelant combien d'ops elle a
  // retenues : si elle se contentait de plafonner a 50 en silence, un
  // appelant qui purge sa file en croyant que `count` ops sont parties
  // perdrait les suivantes pour de bon. Mieux vaut refuser que de laisser
  // croire que tout est parti.
  if (count > TASK_MAX_OPS_PER_SYNC) return 0;

  const int head = std::snprintf(out, outSize, "{\"schema\":1,\"cursor\":\"");
  if (head < 0 || static_cast<size_t>(head) >= outSize) return 0;
  size_t pos = static_cast<size_t>(head);

  // Le curseur est opaque — on ne l'interprete jamais — mais il doit rester a
  // l'interieur d'une chaine JSON valide. L'echapper ne suppose rien de son
  // contenu ; ca n'est pas le parser, juste eviter qu'un octet corrompu par
  // la carte SD ne casse la structure de tout le corps.
  if (!appendEscaped(cursor, out, outSize, pos)) return 0;
  const int mid = std::snprintf(out + pos, outSize - pos, "\",\"ops\":[");
  if (mid < 0 || pos + static_cast<size_t>(mid) >= outSize) return 0;
  pos += static_cast<size_t>(mid);

  for (size_t i = 0; i < count; i++) {
    if (i > 0) {
      if (pos + 1 >= outSize) return 0;
      out[pos++] = ',';
    }
    const size_t written = taskOpToLine(ops[i], out + pos, outSize - pos);
    if (written == 0) return 0;
    pos += written;
  }
  if (pos + 3 > outSize) return 0;
  out[pos++] = ']';
  out[pos++] = '}';
  out[pos] = '\0';
  return pos;
}
