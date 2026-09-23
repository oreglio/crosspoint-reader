#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "StreamingJsonParser.h"
#include "TaskOpQueue.h"

namespace {
TaskOp addOp(const char* id, const char* title, uint8_t priority) {
  TaskOp op{};
  op.kind = TaskOpKind::Add;
  std::snprintf(op.id, sizeof(op.id), "%s", id);
  std::snprintf(op.title, sizeof(op.title), "%s", title);
  op.priority = priority;
  return op;
}
}  // namespace

TEST(TaskOpQueue, AddRoundTrips) {
  char line[512];
  const TaskOp op = addOp("d0000abc1", "Racheter du cafe", 0);
  const size_t n = taskOpToLine(op, line, sizeof(line));
  ASSERT_GT(n, 0u);

  TaskOp back{};
  ASSERT_TRUE(taskOpFromLine(line, n, back));
  EXPECT_EQ(back.kind, TaskOpKind::Add);
  EXPECT_STREQ(back.id, "d0000abc1");
  EXPECT_STREQ(back.title, "Racheter du cafe");
  EXPECT_EQ(back.priority, 0);
}

TEST(TaskOpQueue, DoneAndPrioRoundTrip) {
  char line[512];
  TaskOp done{};
  done.kind = TaskOpKind::Done;
  std::snprintf(done.id, sizeof(done.id), "w17ab93c2");
  done.done = true;
  const size_t n = taskOpToLine(done, line, sizeof(line));
  TaskOp back{};
  ASSERT_TRUE(taskOpFromLine(line, n, back));
  EXPECT_EQ(back.kind, TaskOpKind::Done);
  EXPECT_TRUE(back.done);
}

TEST(TaskOpQueue, EscapesQuotesAndBackslashesInTitles) {
  char line[512];
  const TaskOp op = addOp("d0000abc1", "Lire \"Dune\" et C:\\temp", 1);
  const size_t n = taskOpToLine(op, line, sizeof(line));
  ASSERT_GT(n, 0u);
  TaskOp back{};
  ASSERT_TRUE(taskOpFromLine(line, n, back));
  EXPECT_STREQ(back.title, "Lire \"Dune\" et C:\\temp");
}

// Le round-trip est le vrai test : accents, CJK et emoji sont de l'UTF-8
// multi-octet dont chaque octet de poids fort vaut 0x80 ou plus, donc jamais
// pris pour un guillemet, un antislash ou un caractere de controle par
// appendEscaped — ils doivent traverser telles quelles.
TEST(TaskOpQueue, RoundTripsAccentsCjkAndEmoji) {
  char line[512];
  const TaskOp op = addOp("d0000abc1", "Reviser 日本語 et l'ete \xC3\xA9\xC3\xA9 \xF0\x9F\x98\x80", 2);
  const size_t n = taskOpToLine(op, line, sizeof(line));
  ASSERT_GT(n, 0u);
  TaskOp back{};
  ASSERT_TRUE(taskOpFromLine(line, n, back));
  EXPECT_STREQ(back.title, op.title);
}

// Deviation vs le brief : un caractere de controle devient une espace plutot
// qu'un \u00XX, parce que StreamingJsonParser ne decode pas \uXXXX (il le
// recopie tel quel — cf sa propre StreamingJsonParser.cpp) et casserait donc
// ce round-trip. Rien d'autre dans la suite n'exerce un octet de controle :
// sans ce test, une regression vers l'echappement \u passerait inapercue.
TEST(TaskOpQueue, ReplacesControlBytesInTitlesWithASpace) {
  char line[512];
  const TaskOp op = addOp("d0000abc1",
                          "abc"
                          "\x07"
                          "def"
                          "\x09"
                          "ghi",
                          1);
  const size_t n = taskOpToLine(op, line, sizeof(line));
  ASSERT_GT(n, 0u);
  const std::string s(line, n);
  EXPECT_NE(s.find("\"title\":\"abc def ghi\""), std::string::npos);

  TaskOp back{};
  ASSERT_TRUE(taskOpFromLine(line, n, back));
  EXPECT_STREQ(back.title, "abc def ghi");
}

TEST(TaskOpQueue, RefusesABufferTooSmall) {
  char tiny[8];
  EXPECT_EQ(taskOpToLine(addOp("d0000abc1", "x", 1), tiny, sizeof(tiny)), 0u);
}

TEST(TaskOpQueue, RejectsAGarbageLine) {
  TaskOp back{};
  const char* junk = "pas du json";
  EXPECT_FALSE(taskOpFromLine(junk, std::strlen(junk), back));
}

// Une coupure de courant en plein write() du fichier de file d'attente coupe
// la derniere ligne n'importe ou, jamais proprement sur l'accolade finale.
// Une ligne ainsi tronquee ne doit jamais repasser pour une op valide aux
// champs par defaut (ici un "done" reinterprete comme done=false).
TEST(TaskOpQueue, RejectsALineTruncatedMidWrite) {
  char line[512];
  TaskOp done{};
  done.kind = TaskOpKind::Done;
  std::snprintf(done.id, sizeof(done.id), "w17ab93c2");
  done.done = true;
  const size_t n = taskOpToLine(done, line, sizeof(line));
  ASSERT_GT(n, 0u);

  // Coupe avant le champ "done" et l'accolade fermante.
  const char* truncated = "{\"op\":\"done\",\"id\":\"w17ab93c2\"";
  TaskOp back{};
  EXPECT_FALSE(taskOpFromLine(truncated, std::strlen(truncated), back));
}

// Une ligne forgee (jamais produite par taskOpToLine, dont le titre tient
// dans TASK_TITLE_MAX) avec un champ plus long que le tampon de jeton du
// parseur partage : au-dela de TOKEN_BUF_SIZE, StreamingJsonParser tronque le
// jeton et n'appelle jamais onString pour lui — sans tokenTruncated(), opString
// ne verrait rien passer et la ligne serait acceptee avec un titre vide.
TEST(TaskOpQueue, RejectsATitleLongerThanTheParserTokenBuffer) {
  std::string line = "{\"op\":\"add\",\"id\":\"d0000abc1\",\"title\":\"";
  line.append(StreamingJsonParser::TOKEN_BUF_SIZE + 1, 'c');
  line += "\",\"priority\":0}";

  TaskOp back{};
  EXPECT_FALSE(taskOpFromLine(line.c_str(), line.size(), back));
}

TEST(TaskOpQueue, BuildsARequestBodyWithCursorAndOps) {
  const TaskOp ops[] = {addOp("d0000abc1", "Cafe", 0)};
  char body[1024];
  const size_t n = taskOpsToRequestBody(ops, 1, "MTg=", body, sizeof(body));
  ASSERT_GT(n, 0u);
  const std::string s(body, n);
  EXPECT_NE(s.find("\"cursor\":\"MTg=\""), std::string::npos);
  EXPECT_NE(s.find("\"op\":\"add\""), std::string::npos);
  EXPECT_NE(s.find("\"id\":\"d0000abc1\""), std::string::npos);
  EXPECT_EQ(s.front(), '{');
  EXPECT_EQ(s.back(), '}');
}

TEST(TaskOpQueue, BuildsAnEmptyOpsBodyWhenNothingIsPending) {
  char body[256];
  const size_t n = taskOpsToRequestBody(nullptr, 0, "", body, sizeof(body));
  const std::string s(body, n);
  EXPECT_NE(s.find("\"ops\":[]"), std::string::npos);
}

TEST(TaskOpQueue, StopsBeforeOverflowingTheRequestBuffer) {
  TaskOp many[60];
  for (auto& op : many) op = addOp("d0000abc1", "titre assez long pour peser", 1);
  char body[256];
  EXPECT_EQ(taskOpsToRequestBody(many, 60, "", body, sizeof(body)), 0u);
}

// Distinct du test precedent : ici le tampon est largement assez grand pour
// 51 ops courtes. Si la fonction se contentait de plafonner a 50 en
// silence, elle rendrait un corps non nul en ayant discretement perdu une
// op — l'appelant croirait alors avoir tout envoye et purgerait sa file en
// consequence. Elle doit refuser au lieu de tronquer.
TEST(TaskOpQueue, RefusesMoreThanFiftyOpsEvenWithRoomToSpare) {
  TaskOp many[TASK_MAX_OPS_PER_SYNC + 1];
  for (auto& op : many) op = addOp("d0000abc1", "cafe", 1);
  char body[TASK_REQUEST_BUF_SIZE];
  EXPECT_EQ(taskOpsToRequestBody(many, TASK_MAX_OPS_PER_SYNC + 1, "", body, sizeof(body)), 0u);

  // Mais exactement 50 doit passer, preuve que la limite est bien 50 et non
  // un effet de bord du test precedent.
  EXPECT_GT(taskOpsToRequestBody(many, TASK_MAX_OPS_PER_SYNC, "", body, sizeof(body)), 0u);
}

namespace {
TaskOp doneOp(const char* id, bool done) {
  TaskOp op{};
  op.kind = TaskOpKind::Done;
  std::snprintf(op.id, sizeof(op.id), "%s", id);
  op.done = done;
  return op;
}

std::string recordOf(const TaskOp& op) {
  char buf[TASK_OP_RECORD_MAX];
  const size_t n = taskOpToRecord(op, buf, sizeof(buf));
  return std::string(buf, n);
}

// Ce que la file rend d'un contenu donne, octet par octet comme TaskStore.
std::vector<TaskOp> splitQueue(const std::string& content) {
  std::vector<TaskOp> ops;
  TaskOpLineSplitter splitter;
  TaskOp op{};
  for (const char c : content) {
    if (splitter.feed(c, op)) ops.push_back(op);
  }
  return ops;
}
}  // namespace

// Un enregistrement coupe par une coupure de courant (ou dont un write a ete
// refuse) laisse un fragment sans '\n' final. L'op ajoutee ensuite doit
// survivre : c'est celle d'un geste que l'utilisateur a vu reussir.
TEST(TaskOpQueue, TornRecordDoesNotSwallowTheNextOp) {
  const std::string torn = recordOf(doneOp("w17ab93c2", true));
  const std::string next = recordOf(doneOp("d0000abc1", false));
  const std::string content = torn.substr(0, torn.size() / 2) + next;

  const std::vector<TaskOp> ops = splitQueue(content);
  ASSERT_EQ(ops.size(), 1u);
  EXPECT_STREQ(ops[0].id, "d0000abc1");
  EXPECT_FALSE(ops[0].done);
}

// Toutes les coupures possibles : l'op suivante n'est jamais perdue. L'op
// coupee n'est rendue que si sa ligne entiere a atteint la carte (la coupure
// n'a emporte que son '\n' final, que le '\n' de tete suivant remplace).
TEST(TaskOpQueue, NextOpSurvivesACutAtEveryByte) {
  const std::string torn = recordOf(addOp("w17ab93c2", "Racheter du cafe", 0));
  const std::string next = recordOf(doneOp("d0000abc1", true));
  for (size_t cut = 0; cut <= torn.size(); cut++) {
    const std::vector<TaskOp> ops = splitQueue(torn.substr(0, cut) + next);
    const bool lineComplete = cut >= torn.size() - 1;
    ASSERT_EQ(ops.size(), lineComplete ? 2u : 1u) << "cut at " << cut;
    EXPECT_STREQ(ops.back().id, "d0000abc1") << "cut at " << cut;
    if (lineComplete) EXPECT_STREQ(ops.front().id, "w17ab93c2") << "cut at " << cut;
  }
}

TEST(TaskOpQueue, TrailingFragmentIsNeverReplayed) {
  const std::string full = recordOf(doneOp("w17ab93c2", true));
  EXPECT_TRUE(splitQueue(full.substr(0, full.size() - 1)).empty());
  EXPECT_EQ(splitQueue(full).size(), 1u);
}

// Une priorite hors des trois valeurs connues (carte editee a la main, ou
// format a venir) revient a normale plutot que d'atteindre le serveur telle
// quelle.
TEST(TaskOpQueue, OutOfRangePriorityFallsBackToNormal) {
  const char line[] = "{\"op\":\"prio\",\"id\":\"d0000abc1\",\"priority\":7}";
  TaskOp back{};
  ASSERT_TRUE(taskOpFromLine(line, sizeof(line) - 1, back));
  EXPECT_EQ(back.kind, TaskOpKind::Prio);
  EXPECT_EQ(back.priority, TASK_PRIORITY_NORMAL);
}
