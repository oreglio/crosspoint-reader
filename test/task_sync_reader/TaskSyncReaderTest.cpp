#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "StreamingJsonParser.h"
#include "TaskSyncOutcome.h"
#include "TaskSyncReader.h"

namespace {

struct Capture {
  std::string cursor;
  bool more = false;
  bool reset = false;
  bool header = false;
  std::vector<TaskRecord> tasks;
  std::vector<std::string> deleted;
  std::vector<std::pair<std::string, std::string>> notes;  // id -> corps accumulé
  std::vector<std::pair<std::string, std::string>> rejected;
  size_t noteChunks = 0;
  size_t lastFlags = 0;
};

void onHeader(void* ctx, const char* cursor, bool more, bool reset) {
  auto* c = static_cast<Capture*>(ctx);
  c->cursor = cursor;
  c->more = more;
  c->reset = reset;
  c->header = true;
}
void onTask(void* ctx, const TaskRecord& rec) { static_cast<Capture*>(ctx)->tasks.push_back(rec); }
void onDeleted(void* ctx, const char* id) { static_cast<Capture*>(ctx)->deleted.emplace_back(id); }
void onNoteChunk(void* ctx, const char* id, const char* data, size_t len, bool last) {
  auto* c = static_cast<Capture*>(ctx);
  if (c->notes.empty() || c->notes.back().first != id) c->notes.emplace_back(id, std::string());
  c->notes.back().second.append(data, len);
  c->noteChunks++;
  if (last) c->lastFlags++;
}
void onRejected(void* ctx, const char* id, const char* reason) {
  static_cast<Capture*>(ctx)->rejected.emplace_back(id, reason);
}

TaskSyncCallbacks callbacks(Capture& cap) { return {&cap, onHeader, onTask, onDeleted, onNoteChunk, onRejected}; }

// Alimente le lecteur par tranches de `chunk` octets, pour prouver que le
// cadrage survit à un découpage réseau arbitraire.
void feedInChunks(TaskSyncReader& r, const std::string& body, size_t chunk) {
  for (size_t i = 0; i < body.size(); i += chunk) {
    r.feed(body.data() + i, std::min(chunk, body.size() - i));
  }
}

const std::string kHeader = "{\"schema\":1,\"cursor\":\"MQ==\",\"more\":false,\"reset\":false,\"count\":1}\n";

}  // namespace

TEST(TaskSyncReader, ReadsHeaderAndOneTaskWithoutNote) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      "{\"schema\":1,\"cursor\":\"MTg=\",\"more\":false,\"reset\":false,\"count\":1}\n"
      "{\"id\":\"w17ab93c2\",\"title\":\"Rappeler le notaire\",\"priority\":0,\"done\":false,"
      "\"noteBytes\":0}\n";
  r.feed(body.data(), body.size());

  EXPECT_TRUE(cap.header);
  EXPECT_EQ(cap.cursor, "MTg=");
  EXPECT_FALSE(cap.more);
  ASSERT_EQ(cap.tasks.size(), 1u);
  EXPECT_STREQ(cap.tasks[0].id, "w17ab93c2");
  EXPECT_STREQ(cap.tasks[0].title, "Rappeler le notaire");
  EXPECT_EQ(cap.tasks[0].priority, 0);
  EXPECT_FALSE(cap.tasks[0].done);
  EXPECT_EQ(cap.tasks[0].noteBytes, 0);
  EXPECT_TRUE(cap.notes.empty());
  EXPECT_FALSE(r.hasError());
  EXPECT_TRUE(r.isComplete());
}

TEST(TaskSyncReader, ReadsARawNoteBlockAfterItsMetadataLine) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string note = "Curseur + file d'ops.\nDeuxieme ligne.";
  const std::string body = kHeader +
                           "{\"id\":\"w17ab93c2\",\"title\":\"Spec\",\"priority\":1,\"done\":false,\"noteBytes\":" +
                           std::to_string(note.size()) + "}\n" + note + "\n";
  r.feed(body.data(), body.size());

  ASSERT_EQ(cap.notes.size(), 1u);
  EXPECT_EQ(cap.notes[0].first, "w17ab93c2");
  // Le \n interne à la note ne doit PAS être pris pour une fin de ligne.
  EXPECT_EQ(cap.notes[0].second, note);
  EXPECT_EQ(cap.lastFlags, 1u);
  EXPECT_FALSE(r.hasError());
  EXPECT_TRUE(r.isComplete());
}

TEST(TaskSyncReader, SurvivesANoteSplitAcrossNetworkChunks) {
  const std::string note(3000, 'n');
  const std::string body = kHeader +
                           "{\"id\":\"d0000abc1\",\"title\":\"Longue\",\"priority\":2,\"done\":false,\"noteBytes\":" +
                           std::to_string(note.size()) + "}\n" + note + "\n";

  for (const size_t chunk : {1u, 7u, 64u, 512u, 1500u}) {
    Capture cap;
    TaskSyncReader r(callbacks(cap));
    feedInChunks(r, body, chunk);
    ASSERT_EQ(cap.notes.size(), 1u) << "chunk=" << chunk;
    EXPECT_EQ(cap.notes[0].second, note) << "chunk=" << chunk;
    // `last` ne doit être levé qu'une seule fois, sur le dernier morceau.
    EXPECT_EQ(cap.lastFlags, 1u) << "chunk=" << chunk;
    EXPECT_FALSE(r.hasError()) << "chunk=" << chunk;
    EXPECT_TRUE(r.isComplete()) << "chunk=" << chunk;
  }
}

// Le découpage réseau peut tomber n'importe où : on le prouve en coupant le
// même corps à chacune de ses frontières, y compris dans la note et sur le \n
// qui la termine.
TEST(TaskSyncReader, SurvivesASplitAtEveryByteBoundary) {
  const std::string note = "abc\ndef";
  const std::string body =
      "{\"schema\":1,\"cursor\":\"Mg==\",\"more\":true,\"reset\":false,\"count\":3}\n"
      "{\"id\":\"w00000001\",\"title\":\"A\",\"priority\":0,\"done\":false,\"noteBytes\":" +
      std::to_string(note.size()) + "}\n" + note +
      "\n"
      "{\"id\":\"w00000002\",\"deleted\":true}\n"
      "{\"id\":\"d0000abc1\",\"title\":\"C\",\"priority\":1,\"done\":true,\"noteBytes\":0}\n";

  for (size_t cut = 0; cut <= body.size(); cut++) {
    Capture cap;
    TaskSyncReader r(callbacks(cap));
    r.feed(body.data(), cut);
    r.feed(body.data() + cut, body.size() - cut);
    ASSERT_FALSE(r.hasError()) << "cut=" << cut;
    ASSERT_TRUE(r.isComplete()) << "cut=" << cut;
    EXPECT_TRUE(cap.more) << "cut=" << cut;
    EXPECT_EQ(cap.cursor, "Mg==") << "cut=" << cut;
    ASSERT_EQ(cap.tasks.size(), 2u) << "cut=" << cut;
    EXPECT_STREQ(cap.tasks[1].id, "d0000abc1") << "cut=" << cut;
    ASSERT_EQ(cap.deleted.size(), 1u) << "cut=" << cut;
    ASSERT_EQ(cap.notes.size(), 1u) << "cut=" << cut;
    EXPECT_EQ(cap.notes[0].second, note) << "cut=" << cut;
    EXPECT_EQ(cap.lastFlags, 1u) << "cut=" << cut;
  }
}

TEST(TaskSyncReader, ReadsSeveralTasksAndTombstonesInOneBody) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      "{\"schema\":1,\"cursor\":\"Mg==\",\"more\":true,\"reset\":false,\"count\":3}\n"
      "{\"id\":\"w00000001\",\"title\":\"A\",\"priority\":0,\"done\":false,\"noteBytes\":2}\n"
      "ab\n"
      "{\"id\":\"w00000002\",\"deleted\":true}\n"
      "{\"id\":\"d0000abc1\",\"title\":\"C\",\"priority\":1,\"done\":true,\"noteBytes\":0}\n";
  feedInChunks(r, body, 13);

  EXPECT_TRUE(cap.more);
  ASSERT_EQ(cap.tasks.size(), 2u);
  EXPECT_STREQ(cap.tasks[1].id, "d0000abc1");
  EXPECT_TRUE(cap.tasks[1].done);
  ASSERT_EQ(cap.deleted.size(), 1u);
  EXPECT_EQ(cap.deleted[0], "w00000002");
  ASSERT_EQ(cap.notes.size(), 1u);
  EXPECT_EQ(cap.notes[0].second, "ab");
}

TEST(TaskSyncReader, ReportsTheResetFlag) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body = "{\"schema\":1,\"cursor\":\"MQ==\",\"more\":false,\"reset\":true,\"count\":0}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(cap.reset);
}

TEST(TaskSyncReader, ReportsEveryRejectionInTheHeader) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      "{\"schema\":1,\"cursor\":\"MTg=\",\"rejected\":[{\"id\":\"d0000abc1\",\"reason\":\"full\"},"
      "{\"id\":\"w17ab93c2\",\"reason\":\"unknown\"}],\"more\":true,\"reset\":false,\"count\":0}\n";
  r.feed(body.data(), body.size());

  ASSERT_EQ(cap.rejected.size(), 2u);
  EXPECT_EQ(cap.rejected[0].first, "d0000abc1");
  EXPECT_EQ(cap.rejected[0].second, "full");
  EXPECT_EQ(cap.rejected[1].first, "w17ab93c2");
  EXPECT_EQ(cap.rejected[1].second, "unknown");
  // Les champs propres de l'en-tête restent lisibles sur la même ligne.
  EXPECT_TRUE(cap.header);
  EXPECT_EQ(cap.cursor, "MTg=");
  EXPECT_TRUE(cap.more);
  EXPECT_FALSE(cap.reset);
  EXPECT_FALSE(r.hasError());
}

// Un collecteur plat laisserait les clés imbriquées écraser les champs de
// l'en-tête : on le prouve avec une entrée qui porte exprès les mêmes noms.
TEST(TaskSyncReader, NestedRejectionFieldsDoNotMasqueradeAsHeaderFields) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      "{\"schema\":1,\"cursor\":\"MTg=\",\"more\":false,\"reset\":false,"
      "\"rejected\":[{\"id\":\"d0000abc1\",\"reason\":\"full\",\"more\":true,\"reset\":true,"
      "\"cursor\":\"PIRATE\"}],\"count\":0}\n";
  r.feed(body.data(), body.size());

  ASSERT_EQ(cap.rejected.size(), 1u);
  EXPECT_EQ(cap.rejected[0].first, "d0000abc1");
  EXPECT_EQ(cap.cursor, "MTg=");
  EXPECT_FALSE(cap.more);
  EXPECT_FALSE(cap.reset);
}

TEST(TaskSyncReader, RejectsAMalformedIdWithoutEmittingATask) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      kHeader + "{\"id\":\"../etc/passwd\",\"title\":\"X\",\"priority\":1,\"done\":false,\"noteBytes\":0}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(cap.tasks.empty());
}

// Un id trop long ne doit pas être tronqué jusqu'à redevenir valide : ce serait
// accepter une tâche sous une identité qui n'est pas la sienne.
TEST(TaskSyncReader, RejectsAnOverlongIdInsteadOfTruncatingItToAValidOne) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      kHeader + "{\"id\":\"w17ab93c2ffff\",\"title\":\"X\",\"priority\":1,\"done\":false,\"noteBytes\":0}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(cap.tasks.empty());
}

// La tâche est refusée, mais son bloc de note doit quand même être consommé :
// sinon ses octets bruts seraient relus comme des lignes de métadonnées.
TEST(TaskSyncReader, SkipsTheNoteBlockOfARejectedTaskAndKeepsTheFraming) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body = kHeader +
                           "{\"id\":\"BAD\",\"title\":\"X\",\"priority\":1,\"done\":false,\"noteBytes\":22}\n"
                           "note perdue de 22 o.!!\n"
                           "{\"id\":\"w00000001\",\"title\":\"Apres\",\"priority\":1,\"done\":false,\"noteBytes\":0}\n";
  r.feed(body.data(), body.size());

  EXPECT_TRUE(cap.notes.empty());
  ASSERT_EQ(cap.tasks.size(), 1u);
  EXPECT_STREQ(cap.tasks[0].id, "w00000001");
  EXPECT_FALSE(r.hasError());
  EXPECT_TRUE(r.isComplete());
}

TEST(TaskSyncReader, RejectsANoteLongerThanTheCap) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      kHeader + "{\"id\":\"w00000001\",\"title\":\"X\",\"priority\":1,\"done\":false,\"noteBytes\":99999}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(r.hasError());
}

TEST(TaskSyncReader, TruncatesAnOverlongTitleInsteadOfOverflowing) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string title(400, 't');
  const std::string body =
      kHeader + "{\"id\":\"w00000001\",\"title\":\"" + title + "\",\"priority\":1,\"done\":false,\"noteBytes\":0}\n";
  r.feed(body.data(), body.size());
  ASSERT_EQ(cap.tasks.size(), 1u);
  EXPECT_LE(std::strlen(cap.tasks[0].title), TASK_TITLE_MAX);
}

// Tronquer au milieu d'un caractère multi-octet produirait un titre illisible
// à l'écran : la coupe doit retomber sur une frontière UTF-8.
TEST(TaskSyncReader, TruncatesTheTitleOnAUtf8Boundary) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  // 67 x '€' = 201 octets : la coupe à 200 tombe au milieu du dernier caractère.
  std::string title;
  for (int i = 0; i < 67; i++) title += "\xE2\x82\xAC";
  const std::string body =
      kHeader + "{\"id\":\"w00000001\",\"title\":\"" + title + "\",\"priority\":1,\"done\":false,\"noteBytes\":0}\n";
  r.feed(body.data(), body.size());

  ASSERT_EQ(cap.tasks.size(), 1u);
  EXPECT_EQ(std::strlen(cap.tasks[0].title), 198u);
  EXPECT_EQ(std::string(cap.tasks[0].title), title.substr(0, 198));
}

// Une ligne illisible fait perdre le cadrage : on ne sait plus si des octets
// bruts suivent, donc on échoue fermé au lieu de relire la note en JSON.
TEST(TaskSyncReader, FailsClosedOnAMalformedMetadataLine) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body = kHeader + "{\"id\":\"w00000001\",\"noteBytes\":4\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(r.hasError());
}

// Le remplissage place la coupure APRÈS l'accolade fermante : le préfixe gardé
// est donc du JSON complet, et seule la garde sur `dropped` empêche de le
// prendre pour la ligne entière — et de désaligner le bloc de note qui suit.
TEST(TaskSyncReader, FailsClosedOnALineLongerThanTheBuffer) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string padding(TaskSyncReader::LINE_BUF_SIZE + 64, ' ');
  const std::string body = kHeader +
                           "{\"id\":\"w00000001\",\"title\":\"X\",\"priority\":1,\"done\":false,\"noteBytes\":2}" +
                           padding + "\nab\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(r.hasError());
  EXPECT_TRUE(cap.tasks.empty());
  EXPECT_TRUE(cap.notes.empty());
}

// Une pierre tombale ne porte pas de note, mais si le serveur en annonçait une
// il faudrait quand même consommer ses octets pour rester aligné.
TEST(TaskSyncReader, ConsumesTheNoteBlockOfATombstone) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body = kHeader + "{\"id\":\"w00000002\",\"deleted\":true,\"noteBytes\":3}\nxyz\n" +
                           "{\"id\":\"w00000001\",\"title\":\"Apres\",\"priority\":1,\"done\":false,\"noteBytes\":0}\n";
  r.feed(body.data(), body.size());

  ASSERT_EQ(cap.deleted.size(), 1u);
  EXPECT_TRUE(cap.notes.empty());
  ASSERT_EQ(cap.tasks.size(), 1u);
  EXPECT_STREQ(cap.tasks[0].id, "w00000001");
  EXPECT_FALSE(r.hasError());
  EXPECT_TRUE(r.isComplete());
}

TEST(TaskSyncReader, FailsClosedWhenTheNoteIsNotTerminatedByANewline) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body = kHeader +
                           "{\"id\":\"w00000001\",\"title\":\"X\",\"priority\":1,\"done\":false,\"noteBytes\":2}\n"
                           "abX{\"id\":\"w00000002\",\"deleted\":true}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(r.hasError());
  EXPECT_TRUE(cap.deleted.empty());
}

TEST(TaskSyncReader, ReportsAnIncompleteBodyEndingMidNote) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body = kHeader +
                           "{\"id\":\"w00000001\",\"title\":\"X\",\"priority\":1,\"done\":false,\"noteBytes\":10}\n"
                           "abc";
  r.feed(body.data(), body.size());
  EXPECT_FALSE(r.isComplete());
  EXPECT_EQ(cap.lastFlags, 0u);
}

TEST(TaskSyncReader, ReportsAnIncompleteBodyCutMidLine) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body = kHeader + "{\"id\":\"w00000001\",\"title\":\"X\"";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(r.sawHeader());
  EXPECT_FALSE(r.isComplete());
  EXPECT_TRUE(cap.tasks.empty());
}

// Le curseur repart tel quel dans le corps de la requête suivante : une valeur
// hors alphabet base64url y injecterait de la syntaxe JSON.
TEST(TaskSyncReader, RejectsACursorOutsideTheBase64urlAlphabet) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body = "{\"schema\":1,\"cursor\":\"a\\\",\\\"ops\\\":[\",\"more\":false,\"reset\":false}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(r.hasError());
  EXPECT_FALSE(cap.header);
}

// Un curseur tronqué serait une perte de données silencieuse à la prochaine
// synchro : on refuse plutôt que de raccourcir.
TEST(TaskSyncReader, RejectsAnOverlongCursor) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string cursor(TASK_SYNC_CURSOR_MAX + 1, 'A');
  const std::string body = "{\"schema\":1,\"cursor\":\"" + cursor + "\",\"more\":false,\"reset\":false}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(r.hasError());
  EXPECT_FALSE(cap.header);
}

// Un schéma inconnu peut cadrer autrement : le lire comme du schéma 1
// corromprait les notes en silence.
TEST(TaskSyncReader, RejectsAnUnknownSchema) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body = "{\"schema\":2,\"cursor\":\"MQ==\",\"more\":false,\"reset\":false,\"count\":0}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(r.hasError());
  EXPECT_FALSE(cap.header);
}

TEST(TaskSyncReader, ToleratesMissingCallbacks) {
  TaskSyncCallbacks empty{};
  TaskSyncReader r(empty);
  const std::string body = kHeader +
                           "{\"id\":\"w00000001\",\"title\":\"X\",\"priority\":1,\"done\":false,\"noteBytes\":2}\n"
                           "ab\n";
  r.feed(body.data(), body.size());
  EXPECT_FALSE(r.hasError());
  EXPECT_TRUE(r.isComplete());
}

// `count` n'est qu'indicatif : refuser l'en-tête parce qu'il serait écrit
// autrement casserait la synchro sans rien protéger.
TEST(TaskSyncReader, IgnoresTheShapeOfFieldsItDoesNotUse) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      "{\"schema\":1,\"cursor\":\"MQ==\",\"more\":false,\"reset\":false,\"count\":3.0,"
      "\"serverTime\":1758542400000}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(cap.header);
  EXPECT_FALSE(r.hasError());
}

// Un curseur vide écrit dans cursor.txt se lit « absent » côté serveur : chaque
// synchro repartirait de zéro, en silence et pour toujours. Le serveur n'en
// émet jamais, donc vide veut dire que quelque chose s'est perdu.
TEST(TaskSyncReader, RejectsAHeaderWithoutACursorKey) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body = "{\"schema\":1,\"more\":false,\"reset\":false,\"count\":0}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(r.hasError());
  EXPECT_FALSE(cap.header);
}

// Au-delà de TOKEN_BUF_SIZE, StreamingJsonParser n'émet pas le jeton du tout :
// la garde d'alphabet ne voit rien passer et le curseur resterait vide.
TEST(TaskSyncReader, RejectsACursorLongerThanTheParserTokenBuffer) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string cursor(StreamingJsonParser::TOKEN_BUF_SIZE + 1, 'A');
  const std::string body = "{\"schema\":1,\"cursor\":\"" + cursor + "\",\"more\":false,\"reset\":false}\n";
  ASSERT_LT(body.size(), TaskSyncReader::LINE_BUF_SIZE);
  r.feed(body.data(), body.size());
  EXPECT_TRUE(r.hasError());
  EXPECT_FALSE(cap.header);
}

// Le tableau `rejected` doit se refermer : un autre tableau d'objets, plus loin
// dans le même en-tête, ne doit pas voir ses entrées prises pour des rejets.
TEST(TaskSyncReader, DoesNotReadRejectionsOutOfAnUnrelatedArray) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      "{\"schema\":1,\"cursor\":\"MQ\",\"rejected\":[{\"id\":\"d0000abc1\",\"reason\":\"full\"}],"
      "\"audit\":[{\"id\":\"w00000001\",\"reason\":\"PIRATE\"}],\"more\":false,\"reset\":false}\n";
  r.feed(body.data(), body.size());

  ASSERT_EQ(cap.rejected.size(), 1u);
  EXPECT_EQ(cap.rejected[0].first, "d0000abc1");
  EXPECT_EQ(cap.rejected[0].second, "full");
  EXPECT_EQ(cap.cursor, "MQ");
  EXPECT_FALSE(r.hasError());
}

// Le même nom de clé, un cran plus bas, ne doit pas écraser le champ d'en-tête.
TEST(TaskSyncReader, DoesNotReadHeaderFieldsOutOfANestedObject) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      "{\"schema\":1,\"cursor\":\"MQ\",\"meta\":{\"cursor\":\"PIRATE\"},\"more\":false,"
      "\"reset\":false}\n";
  r.feed(body.data(), body.size());

  EXPECT_TRUE(cap.header);
  EXPECT_EQ(cap.cursor, "MQ");
  EXPECT_FALSE(r.hasError());
}

// Rien dans le protocole ne fixe l'ordre des clés d'un objet JSON : `rejected`
// doit se lire aussi bien avant qu'après les champs propres de l'en-tête.
TEST(TaskSyncReader, ReadsRejectionsDeclaredBeforeTheHeaderFields) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      "{\"rejected\":[{\"id\":\"d0000abc1\",\"reason\":\"full\"}],\"schema\":1,\"cursor\":\"MQ\","
      "\"more\":true,\"reset\":false,\"count\":0}\n";
  r.feed(body.data(), body.size());

  ASSERT_EQ(cap.rejected.size(), 1u);
  EXPECT_EQ(cap.rejected[0].first, "d0000abc1");
  EXPECT_TRUE(cap.header);
  EXPECT_EQ(cap.cursor, "MQ");
  EXPECT_TRUE(cap.more);
  EXPECT_FALSE(r.hasError());
}

// Octets exacts rendus par crossdrop (feat/crosstasks) a une sync appairee,
// captures avec curl : c'est le contrat reel, pas une reconstitution.
TEST(TaskSyncReader, ReadsTheBytesTheRealServerSends) {
  const std::string body =
      "{\"schema\":1,\"cursor\":\"Mg\",\"more\":false,\"reset\":false,\"count\":2,\"rejected\":[]}\n"
      "{\"id\":\"w209d9ea6\",\"title\":\"Tache creee sur le web\",\"priority\":0,\"done\":false,\"noteBytes\":0}\n"
      "{\"id\":\"d0000abcd\",\"title\":\"Tache creee sur la liseuse\",\"priority\":1,\"done\":false,\"noteBytes\":0}\n";
  // 17 octets : la tranche du chemin simulateur de TaskSyncActivity.
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  feedInChunks(r, body, 17);

  EXPECT_TRUE(r.isComplete());
  EXPECT_EQ(cap.cursor, "Mg");
  ASSERT_EQ(cap.tasks.size(), 2u);
  EXPECT_STREQ(cap.tasks[0].id, "w209d9ea6");
  EXPECT_EQ(cap.tasks[0].priority, 0);
  EXPECT_STREQ(cap.tasks[1].id, "d0000abcd");
  EXPECT_TRUE(cap.rejected.empty());
}

// Meme forme que ci-dessus (ordre des cles de JSON.stringify cote serveur),
// avec ce que la capture n'avait pas : un rejet, une note multi-ligne en UTF-8
// et une suppression.
TEST(TaskSyncReader, ReadsServerShapedRejectionsNotesAndDeletions) {
  const std::string note = "Premi\xc3\xa8re ligne\nseconde ligne";
  const std::string body =
      "{\"schema\":1,\"cursor\":\"czQ\",\"more\":true,\"reset\":true,\"count\":2,"
      "\"rejected\":[{\"id\":\"d0000abc9\",\"reason\":\"full\"},{\"id\":\"w0000ffff\",\"reason\":\"unknown\"}]}\n"
      "{\"id\":\"w209d9ea6\",\"title\":\"Avec note\",\"priority\":2,\"done\":true,\"noteBytes\":" +
      std::to_string(note.size()) + "}\n" + note +
      "\n"
      "{\"id\":\"w0000aaaa\",\"deleted\":true}\n";
  for (const size_t chunk : {1u, 17u, 4096u}) {
    Capture cap;
    TaskSyncReader r(callbacks(cap));
    feedInChunks(r, body, chunk);

    EXPECT_TRUE(r.isComplete()) << "chunk " << chunk;
    EXPECT_EQ(cap.cursor, "czQ");
    EXPECT_TRUE(cap.more);
    EXPECT_TRUE(cap.reset);
    ASSERT_EQ(cap.rejected.size(), 2u);
    EXPECT_EQ(cap.rejected[0], (std::pair<std::string, std::string>{"d0000abc9", "full"}));
    EXPECT_EQ(cap.rejected[1], (std::pair<std::string, std::string>{"w0000ffff", "unknown"}));
    ASSERT_EQ(cap.tasks.size(), 1u);
    EXPECT_TRUE(cap.tasks[0].done);
    ASSERT_EQ(cap.notes.size(), 1u);
    EXPECT_EQ(cap.notes[0].second, note);
    ASSERT_EQ(cap.deleted.size(), 1u);
    EXPECT_EQ(cap.deleted[0], "w0000aaaa");
  }
}

// Une reponse coupee net apres une ligne complete n'est PAS une erreur de
// cadrage, et le transport seul peut la voir : d'ou le bodyComplete separe.
TEST(TaskSyncOutcome, OnlyAFullyAcceptedSuccessMayCommit) {
  EXPECT_EQ(classifyTaskSyncResponse(200, true, true), TaskSyncOutcome::Commit);
  EXPECT_EQ(classifyTaskSyncResponse(204, true, true), TaskSyncOutcome::Commit);

  EXPECT_EQ(classifyTaskSyncResponse(200, false, true), TaskSyncOutcome::BadResponse);
  EXPECT_EQ(classifyTaskSyncResponse(200, true, false), TaskSyncOutcome::BadResponse);
  EXPECT_EQ(classifyTaskSyncResponse(200, false, false), TaskSyncOutcome::BadResponse);

  EXPECT_EQ(classifyTaskSyncResponse(401, true, true), TaskSyncOutcome::PairingRequired);
  EXPECT_EQ(classifyTaskSyncResponse(-1, false, false), TaskSyncOutcome::TransportFailed);
  EXPECT_EQ(classifyTaskSyncResponse(0, false, false), TaskSyncOutcome::ServerError);
  for (const int code : {199, 301, 400, 403, 413, 500, 502, 503}) {
    EXPECT_EQ(classifyTaskSyncResponse(code, true, true), TaskSyncOutcome::ServerError) << code;
  }
}

// Le lecteur, lui, ne voit que des octets : un corps arrete en plein milieu
// d'une note ou d'une ligne ne doit jamais passer pour complet.
TEST(TaskSyncOutcome, ATruncatedBodyIsNeverCommittable) {
  const std::string note(100, 'x');
  const std::string body = kHeader +
                           "{\"id\":\"w17ab93c2\",\"title\":\"T\",\"priority\":1,\"done\":false,\"noteBytes\":" +
                           std::to_string(note.size()) + "}\n" + note + "\n";
  for (size_t cut = 1; cut < body.size(); cut++) {
    Capture cap;
    TaskSyncReader r(callbacks(cap));
    r.feed(body.data(), cut);
    const bool atFrameBoundary = r.isComplete();
    // Les seules coupes « propres » sont juste apres l'en-tete et juste apres
    // le \n qui ferme la note ; ailleurs, le lecteur doit refuser de lui-meme.
    if (atFrameBoundary) {
      EXPECT_TRUE(cut == kHeader.size() || cut == body.size()) << "cut " << cut;
    }
  }
}
