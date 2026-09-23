#pragma once

#include <cstddef>
#include <cstdint>

#include "TaskRecord.h"

// Del : suppression faite sur l'appareil. Le serveur la pose en tombstone,
// comme une suppression web, et la rend donc aux autres clients.
enum class TaskOpKind : uint8_t { Add, Done, Prio, Title, Del };

struct TaskOp {
  TaskOpKind kind;
  char id[TASK_ID_LEN + 1];
  char title[TASK_TITLE_MAX + 1];
  uint8_t priority;
  bool done;
};

// Limite de lot : le serveur refuse au-dela de 50 ops, et le corps doit tenir
// dans la limite de 16 Ko de Fastify.
inline constexpr size_t TASK_MAX_OPS_PER_SYNC = 50;
inline constexpr size_t TASK_REQUEST_BUF_SIZE = 12288;

// Plus longue ligne que la file relit. Pire cas mesure sur taskOpToLine() pour
// un op "add" (le seul qui cumule titre echappe et priorite) : entete+id (~30)
// + titre echappe 2*200 o (411) + priorite (15) + accolade finale (2) = 458 o.
inline constexpr size_t TASK_OP_LINE_MAX = 512;
// Un enregistrement de la file : '\n' + ligne + '\n' (voir taskOpToRecord).
inline constexpr size_t TASK_OP_RECORD_MAX = TASK_OP_LINE_MAX + 2;

// Serialise une op en une ligne JSON (sans \n final). Rend 0 si `out` est trop
// petit — l'appelant ne doit alors rien ecrire.
size_t taskOpToLine(const TaskOp& op, char* out, size_t outSize);

// L'enregistrement que la file ajoute pour une op, a ecrire en UN seul
// write() : "\n" + ligne + "\n", sans terminateur. Le '\n' de TETE est ce qui
// isole un fragment laisse par une coupure (ou par un write refuse) : sans
// lui, l'ajout suivant se collerait au fragment et la ligne fusionnee serait
// rejetee — perdant l'op suivante, celle d'un geste que l'utilisateur a vu
// reussir. Les lignes vides qu'il produit sont ignorees a la lecture. Rend 0
// si `out` est trop petit.
size_t taskOpToRecord(const TaskOp& op, char* out, size_t outSize);

// Decoupe le contenu de la file, octet par octet, en ops. Sans E/S : c'est la
// regle de lecture de la file, que TaskStore applique au fichier et que la
// suite hote epingle. Une ligne n'est remise que terminee par '\n' : un
// fragment final (coupure en plein write) n'est jamais rejoue comme une op aux
// champs par defaut. Une ligne vide, trop longue ou illisible est sautee.
class TaskOpLineSplitter {
 public:
  // Rend true quand `byte` termine une ligne qui se relit en op valide, alors
  // copiee dans `out`.
  bool feed(char byte, TaskOp& out);
  // Vrai si la derniere ligne terminee depassait TASK_OP_LINE_MAX.
  bool lastLineTooLong() const { return lastTooLong_; }

 private:
  char line_[TASK_OP_LINE_MAX];
  size_t len_ = 0;
  bool overflow_ = false;
  bool lastTooLong_ = false;
};

// Relit une ligne ecrite par taskOpToLine. Rend false pour tout ce qui n'est
// pas un objet JSON plat et complet — notamment une ligne tronquee par une
// coupure de courant en plein write(), qui ne doit jamais repasser pour une
// op valide aux champs par defaut.
bool taskOpFromLine(const char* line, size_t len, TaskOp& out);

// Assemble {"schema":1,"cursor":"…","ops":[…]}. Rend 0 si tout ne tient pas,
// y compris quand `count` depasse TASK_MAX_OPS_PER_SYNC : cette fonction ne
// peut pas dire a l'appelant combien d'ops elle a retenues, donc elle ne doit
// jamais en ecarter en silence — c'est a l'appelant de decouper le lot avant
// d'appeler.
size_t taskOpsToRequestBody(const TaskOp* ops, size_t count, const char* cursor, char* out, size_t outSize);
