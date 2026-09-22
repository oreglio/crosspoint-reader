#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "activities/tasks/TaskNotePager.h"

namespace {

// Mesure synthetique : 10 px par octet. Deterministe et independante de toute
// police, ce qui est le seul moyen d'epingler l'arithmetique de coupure sans
// GfxRenderer — que la suite hote ne peut pas atteindre.
constexpr int kPxPerByte = 10;

int measureByBytes(void*, const char*, const size_t prefixLen) { return static_cast<int>(prefixLen) * kPxPerByte; }

TaskNoteLineBreak nextLine(const std::string& window, const bool windowIsFinal, const int maxWidth) {
  return taskNoteNextLine(window.data(), window.size(), windowIsFinal, maxWidth, &measureByBytes, nullptr);
}

TEST(TaskNotePagerTest, LinesPerPageFloorsAndNeverReturnsZero) {
  EXPECT_EQ(taskNoteLinesPerPage(100, 26), 3);
  EXPECT_EQ(taskNoteLinesPerPage(78, 26), 3);
  // Une region plus courte qu'une ligne doit quand meme en montrer une :
  // sinon une page ne consomme aucun octet et la pagination ne progresse pas.
  EXPECT_EQ(taskNoteLinesPerPage(10, 26), 1);
  EXPECT_EQ(taskNoteLinesPerPage(100, 0), 1);
  EXPECT_EQ(taskNoteLinesPerPage(0, 26), 1);
}

TEST(TaskNotePagerTest, BreaksOnTheLastWordThatFits) {
  // 10 octets tiennent. "hello world" coupe apres "hello", et l'espace est
  // consomme sans etre dessine.
  const TaskNoteLineBreak brk = nextLine("hello world", /*windowIsFinal=*/true, 100);
  EXPECT_EQ(brk.drawBytes, 5);
  EXPECT_EQ(brk.skipBytes, 6);
}

TEST(TaskNotePagerTest, ConsumesTheWholeFinalWindowWhenItFits) {
  const TaskNoteLineBreak brk = nextLine("hello", /*windowIsFinal=*/true, 100);
  EXPECT_EQ(brk.drawBytes, 5);
  EXPECT_EQ(brk.skipBytes, 5);
}

TEST(TaskNotePagerTest, NewlineClosesTheLineAndIsConsumed) {
  const TaskNoteLineBreak brk = nextLine("ab\ncd", /*windowIsFinal=*/true, 1000);
  EXPECT_EQ(brk.drawBytes, 2);
  EXPECT_EQ(brk.skipBytes, 3);
}

TEST(TaskNotePagerTest, CarriageReturnIsNotDrawn) {
  // Une note ecrite depuis un poste Windows ne doit pas afficher un glyphe
  // parasite en fin de ligne.
  const TaskNoteLineBreak brk = nextLine("ab\r\ncd", /*windowIsFinal=*/true, 1000);
  EXPECT_EQ(brk.drawBytes, 2);
  EXPECT_EQ(brk.skipBytes, 4);
}

TEST(TaskNotePagerTest, EmptyParagraphProducesAnEmptyLine) {
  const TaskNoteLineBreak brk = nextLine("\nabc", /*windowIsFinal=*/true, 1000);
  EXPECT_EQ(brk.drawBytes, 0);
  EXPECT_EQ(brk.skipBytes, 1);
}

TEST(TaskNotePagerTest, OverlongWordIsHardSplitRatherThanDropped) {
  const TaskNoteLineBreak brk = nextLine("abcdefghijklmno", /*windowIsFinal=*/true, 100);
  EXPECT_EQ(brk.drawBytes, 10);
  EXPECT_EQ(brk.skipBytes, 10);
}

TEST(TaskNotePagerTest, HardSplitLandsOnAUtf8Boundary) {
  // Sept "e accent aigu", deux octets chacun. A 100 px, dix octets tiennent :
  // la coupure doit tomber sur 10 (cinq caracteres), jamais sur un demi.
  const std::string accented = "ééééééé";
  ASSERT_EQ(accented.size(), 14u);
  EXPECT_EQ(nextLine(accented, /*windowIsFinal=*/true, 100).drawBytes, 10);
  // A 90 px, neuf octets tiendraient mais couperaient un caractere en deux :
  // la coupure recule a huit.
  EXPECT_EQ(nextLine(accented, /*windowIsFinal=*/true, 90).drawBytes, 8);
}

TEST(TaskNotePagerTest, AWordTouchingANonFinalWindowIsNotCommitted) {
  // "wor" peut continuer au-dela du tampon de lecture : la ligne s'arrete au
  // mot precedent, et l'appelant relira depuis skipBytes.
  const TaskNoteLineBreak brk = nextLine("hello wor", /*windowIsFinal=*/false, 1000);
  EXPECT_EQ(brk.drawBytes, 5);
  EXPECT_EQ(brk.skipBytes, 6);
}

TEST(TaskNotePagerTest, TheSameWordIsCommittedWhenTheWindowIsFinal) {
  const TaskNoteLineBreak brk = nextLine("hello wor", /*windowIsFinal=*/true, 1000);
  EXPECT_EQ(brk.drawBytes, 9);
  EXPECT_EQ(brk.skipBytes, 9);
}

TEST(TaskNotePagerTest, AlwaysProgressesEvenWhenNothingFits) {
  // Un seul mot, plus long que la fenetre, et une largeur qui ne laisse pas
  // passer un caractere : sans progression garantie, la pagination bouclerait.
  const TaskNoteLineBreak brk = nextLine("abcdefghij", /*windowIsFinal=*/false, 1);
  EXPECT_GT(brk.skipBytes, 0);
  EXPECT_LE(brk.drawBytes, brk.skipBytes);
}

TEST(TaskNotePagerTest, AWindowOfOneOverlongWordWithNoSpaceTerminates) {
  // Le cas qui fige l'appareil plutot que de mal dessiner : une fenetre faite
  // d'un seul mot multi-octets, sans une seule espace ni fin de ligne, donc
  // rien pour couper sauf la coupure dure. Si celle-ci rend zero octet une
  // seule fois, la pagination boucle indefiniment. On deroule jusqu'au bout.
  std::string word;
  for (int i = 0; i < 40; i++) word += "é";  // 80 octets, aucun separateur
  ASSERT_EQ(word.size(), 80u);

  size_t offset = 0;
  size_t iterations = 0;
  while (offset < word.size()) {
    ASSERT_LT(++iterations, 200u) << "la coupure dure doit progresser a chaque tour";
    const TaskNoteLineBreak brk = taskNoteNextLine(word.data() + offset, word.size() - offset, /*windowIsFinal=*/false,
                                                   100, &measureByBytes, nullptr);
    ASSERT_GT(brk.skipBytes, 0u);
    // Et chaque coupure tombe sur une frontiere UTF-8 : l'octet suivant ne
    // doit jamais etre une continuation (10xxxxxx).
    const size_t cut = offset + brk.drawBytes;
    if (cut < word.size()) {
      EXPECT_EQ(static_cast<unsigned char>(word[cut]) & 0xC0, 0xC0u)
          << "coupure au milieu d'un caractere a l'octet " << cut;
    }
    offset += brk.skipBytes;
  }
  EXPECT_EQ(offset, word.size());
}

TEST(TaskNotePagerTest, EmptyWindowConsumesNothing) {
  const TaskNoteLineBreak brk = taskNoteNextLine("", 0, true, 100, &measureByBytes, nullptr);
  EXPECT_EQ(brk.drawBytes, 0);
  EXPECT_EQ(brk.skipBytes, 0);
}

TEST(TaskNotePagerTest, LeadingBlanksStayPartOfTheDrawnLine) {
  // L'indentation d'une note se conserve : les blancs en tete appartiennent au
  // texte dessine, pas au separateur.
  const TaskNoteLineBreak brk = nextLine("  ab\ncd", /*windowIsFinal=*/true, 1000);
  EXPECT_EQ(brk.drawBytes, 4);
  EXPECT_EQ(brk.skipBytes, 5);
}

TEST(TaskNotePagerTest, CompleteUtf8PrefixStopsBeforeATruncatedSequence) {
  const char twoByte[] = {'a', '\xc3', '\xa9'};
  EXPECT_EQ(taskNoteCompleteUtf8Prefix(twoByte, sizeof(twoByte)), 3u);
  // La meme sequence coupee par le bord du tampon : le prefixe s'arrete avant.
  EXPECT_EQ(taskNoteCompleteUtf8Prefix(twoByte, 2u), 1u);

  const char fourByte[] = {'\xf0', '\x9f', '\x93', '\x9d'};  // U+1F4DD
  EXPECT_EQ(taskNoteCompleteUtf8Prefix(fourByte, sizeof(fourByte)), 4u);
  EXPECT_EQ(taskNoteCompleteUtf8Prefix(fourByte, 3u), 0u);
  EXPECT_EQ(taskNoteCompleteUtf8Prefix(nullptr, 4u), 0u);
}

// Deroule une note entiere fenetre par fenetre, exactement comme
// TaskDetailActivity::paginateNote() : c'est la propriete qui compte pour
// l'ecran, puisque rien d'autre ne peut la verifier ici.
TEST(TaskNotePagerTest, PaginationCoversEveryByteExactlyOnce) {
  const std::string note =
      "Premiere ligne de la note.\nUne deuxieme, plus longue, qui doit s'enrouler sur plusieurs lignes.\n\nEt une "
      "derniere apres un paragraphe vide.";
  constexpr size_t kWindow = 32;  // tampon volontairement etroit
  constexpr int kMaxWidth = 120;  // 12 octets par ligne
  constexpr int kLinesPerPage = 3;

  std::string drawn;
  size_t offset = 0;
  size_t guard = 0;
  while (offset < note.size() && guard++ < 1000) {
    const size_t rawLen = std::min(kWindow, note.size() - offset);
    const bool windowIsFinal = offset + rawLen >= note.size();
    const size_t windowLen = windowIsFinal ? rawLen : taskNoteCompleteUtf8Prefix(note.data() + offset, rawLen);
    size_t consumed = 0;
    for (int line = 0; line < kLinesPerPage && consumed < windowLen; line++) {
      const TaskNoteLineBreak brk = taskNoteNextLine(note.data() + offset + consumed, windowLen - consumed,
                                                     windowIsFinal, kMaxWidth, &measureByBytes, nullptr);
      ASSERT_GT(brk.skipBytes, 0u) << "la pagination doit progresser";
      ASSERT_LE(brk.drawBytes, brk.skipBytes);
      drawn.append(note, offset + consumed, brk.drawBytes);
      consumed += brk.skipBytes;
    }
    ASSERT_GT(consumed, 0u);
    offset += consumed;
  }
  ASSERT_EQ(offset, note.size());

  // Tout le texte est passe a l'ecran : seuls les separateurs ont disparu.
  std::string expected;
  for (const char c : note) {
    if (c != ' ' && c != '\n' && c != '\r') expected.push_back(c);
  }
  std::string actual;
  for (const char c : drawn) {
    if (c != ' ' && c != '\n' && c != '\r') actual.push_back(c);
  }
  EXPECT_EQ(actual, expected);
}

}  // namespace
