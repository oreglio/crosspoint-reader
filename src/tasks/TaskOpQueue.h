#pragma once

#include <cstddef>
#include <cstdint>

#include "TaskRecord.h"

enum class TaskOpKind : uint8_t { Add, Done, Prio, Title };

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

// Serialise une op en une ligne JSON (sans \n final). Rend 0 si `out` est trop
// petit — l'appelant ne doit alors rien ecrire.
size_t taskOpToLine(const TaskOp& op, char* out, size_t outSize);

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
