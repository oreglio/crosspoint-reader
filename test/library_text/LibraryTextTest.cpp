#include <gtest/gtest.h>

#include <string>

#include "LibraryText.h"

using library::authorKey;
using library::fold;
using library::looksLikeMetadata;
using library::parseFilename;
using library::preferFilenameTitle;

namespace {

// The same text in both Unicode normal forms. Every fold test runs both, because
// a card holding only one form makes a one-sided test pass by accident — which
// is exactly how a fold built on utf8ComposeNfc() survives until the first file
// arrives from the other kind of machine.
struct NormalisationPair {
  const char* nfc;  // precomposed: e-acute is one codepoint
  const char* nfd;  // decomposed: e followed by a combining acute
  const char* expected;
};

constexpr NormalisationPair PAIRS[] = {
    {"pand\xC3\xA9mie", "pande\xCC\x81mie", "pandemie"},
    {"\xC3\x89"
     "clipse totale",
     "E\xCC\x81"
     "clipse totale",
     "eclipse totale"},
    {"M\xC3\xA9moires", "Me\xCC\x81moires", "memoires"},
    {"Ang\xC3\xA9lina", "Ange\xCC\x81lina", "angelina"},
    {"R\xC3\xA9"
     "camier",
     "Re\xCC\x81"
     "camier",
     "recamier"},
    {"Derri\xC3\xA8re les portes", "Derrie\xCC\x80re les portes", "derriere les portes"},
};

}  // namespace

TEST(LibraryFold, BothNormalisationsAgree) {
  for (const auto& p : PAIRS) {
    EXPECT_EQ(fold(p.nfc), p.expected) << "NFC input: " << p.nfc;
    EXPECT_EQ(fold(p.nfd), p.expected) << "NFD input: " << p.nfd;
    EXPECT_EQ(fold(p.nfc), fold(p.nfd)) << "forms disagree for " << p.expected;
  }
}

TEST(LibraryFold, LettersWithoutCanonicalDecomposition) {
  // These have no NFD form at all, so a decompose-only fold silently deletes
  // them. "Nesbø" losing its last letter is the case that motivated the map.
  EXPECT_EQ(fold("Nesb\xC3\xB8"), "nesbo");
  EXPECT_EQ(fold("\xC3\x98stergaard"), "ostergaard");
  EXPECT_EQ(fold("\xC3\x86"
                 "sop"),
            "aesop");
  EXPECT_EQ(fold("Stra\xC3\x9F"
                 "e"),
            "strasse");
  EXPECT_EQ(fold("\xC5\x81odz"), "lodz");
  EXPECT_EQ(fold("s\xC5\x93ur"), "soeur");
}

TEST(LibraryFold, ApostrophesSurviveSoArchiveCreditsStayMatchable) {
  // U+2019 is what exporters actually emit; folding it to a space would split
  // "Anna's" into two tokens and defeat looksLikeMetadata().
  EXPECT_EQ(fold("Anna\xE2\x80\x99s Archive"), "anna's archive");
  EXPECT_EQ(fold("Anna's Archive"), "anna's archive");
  EXPECT_EQ(fold("L\xE2\x80\x99inconsol\xC3\xA9"), "l'inconsole");
}

TEST(LibraryFold, PunctuationSeparatesAndSpaceRunsCollapse) {
  EXPECT_EQ(fold("Le juge Ti.T2.Le po\xC3\xA8te"), "le juge ti t2 le poete");
  EXPECT_EQ(fold("  spaced   out  "), "spaced out");
  EXPECT_EQ(fold("a---b"), "a b");
  EXPECT_EQ(fold("2084 _ Artificial"), "2084 artificial");
  EXPECT_EQ(fold(""), "");
  EXPECT_EQ(fold("!!!"), "");
}

TEST(LibraryFold, ArticleStrippingOnlyWhenAsked) {
  EXPECT_EQ(fold("The Fury"), "the fury");
  EXPECT_EQ(fold("The Fury", true), "fury");
  EXPECT_EQ(fold("Les refuges", true), "refuges");
  EXPECT_EQ(fold("L\xE2\x80\x99inconsol\xC3\xA9", true), "inconsole");
  // A title that IS an article-like word must not vanish.
  EXPECT_EQ(fold("The", true), "the");
}

TEST(LibraryParse, TitleAndAuthorFromTheExportPattern) {
  const auto p = parseFilename(
      "Encres de Chine -- Xiaolong, Qiu -- 2012 -- Liana Levi -- d8fc -- Anna's Archive");
  EXPECT_EQ(p.title, "Encres de Chine");
  EXPECT_EQ(p.author, "Xiaolong, Qiu");
}

TEST(LibraryParse, NoSeparatorKeepsTheWholeStemAsTitle) {
  const auto p = parseFilename("Kazuo Ishiguro - Linconsole");
  EXPECT_EQ(p.title, "Kazuo Ishiguro - Linconsole");
  EXPECT_TRUE(p.author.empty());
}

TEST(LibraryParse, MetadataInTheAuthorSlotIsRejected) {
  // Exporters that have no author still emit the field, so segment 1 may hold a
  // publisher, a year or a hash. Accepting those would print them as authors.
  for (const char* stem : {
           "Some Title -- 2019 -- Publisher",
           "Some Title -- d8fc9e08df5718a087a9b2fbbb07e96b -- x",
           "Some Title -- isbn13 9780310109563 -- x",
           "Some Title -- 9782226463982 -- x",
           "Some Title -- Paris, 2019 -- x",
           "Some Title -- Une enquete de l'inspecteur Chen, Paris, DL 2009 -- x",
           "Some Title -- Anna's Archive",
           "Some Title -- Paris, 1985 -",  // trailing dash tolerance
       }) {
    EXPECT_TRUE(parseFilename(stem).author.empty()) << stem;
  }
}

TEST(LibraryParse, RealAuthorsAreNotMistakenForMetadata) {
  // "Michaelides, Alex" is structurally identical to "Paris, France"; only the
  // year requirement separates them. Any looser rule eats real authors.
  for (const char* author : {"Michaelides, Alex", "Pink, Daniel H.", "Jia Jiang [Jiang, Jia]",
                             "John C Lennox", "Qiu Xiaolong_", "Giebel, Karine",
                             "Lloyd  Evans; Paul   Grundy", "B_A_ Paris"}) {
    const std::string stem = std::string("T -- ") + author + " -- 2019";
    EXPECT_EQ(parseFilename(stem).author, author) << author;
  }
}

TEST(LibraryAuthorKey, OrderAndPunctuationDoNotMatter) {
  const std::string expected = authorKey("Qiu Xiaolong");
  EXPECT_FALSE(expected.empty());
  for (const char* spelling : {"Qiu, Xiaolong", "Xiaolong, Qiu", "Qiu Xiaolong_",
                               "Qiu Xiaolong [Xiaolong, Qiu]", "  qiu   xiaolong  "}) {
    EXPECT_EQ(authorKey(spelling), expected) << spelling;
  }
}

TEST(LibraryAuthorKey, InitialsAreIgnored) {
  EXPECT_EQ(authorKey("John C Lennox"), authorKey("John Lennox"));
  EXPECT_EQ(authorKey("Pink, Daniel H."), authorKey("Daniel Pink"));
}

TEST(LibraryAuthorKey, SecondaryAuthorsAndBracketsDropped) {
  EXPECT_EQ(authorKey("Lloyd Evans; Paul Grundy"), authorKey("Lloyd Evans"));
  EXPECT_EQ(authorKey("Karine Giebel [Giebel, Karine]"), authorKey("Karine Giebel"));
}

TEST(LibraryAuthorKey, DistinctPeopleDoNotCollide) {
  EXPECT_NE(authorKey("Alex Michaelides"), authorKey("Ashley Elston"));
  EXPECT_NE(authorKey("Ian Manook"), authorKey("Min Jin Lee"));
}

TEST(LibraryAuthorKey, FitsTheRecordFieldWithoutCollapsingToAForename) {
  const std::string key = authorKey("Bartholomew Fitzgerald Wellington");
  EXPECT_LE(key.size(), library::AUTHOR_KEY_MAX_BYTES);
  EXPECT_NE(key.back(), ' ');

  // Sorting puts a short forename first, so cutting on a token boundary would
  // reduce this to "alex" and merge every Alex in the library. The byte cut must
  // keep enough of the surname to discriminate.
  const std::string alex = authorKey("Michaelides, Alex");
  EXPECT_GT(alex.size(), 5u);
  EXPECT_NE(alex, "alex");
  EXPECT_NE(alex, authorKey("Alex Trevelyan"));

  // A truncated key stays a prefix of the untruncated one, so grouping is stable
  // however long the name is.
  EXPECT_EQ(authorKey("Michaelides, Alexander").rfind("alex", 0), 0u);

  EXPECT_FALSE(authorKey("Nebuchadnezzarson").empty());
  EXPECT_TRUE(authorKey("").empty());
  EXPECT_TRUE(authorKey("J. R. R.").empty());  // initials only: no identity
}

TEST(LibraryTitleMerge, PrefersTheRicherFilenameTitle) {
  EXPECT_TRUE(preferFilenameTitle("2084", "2084 _ Artificial Intelligence and the Future of Humanity"));
  EXPECT_TRUE(preferFilenameTitle("Cosmic Chemistry", "Cosmic Chemistry: Do God and Science Mix?"));
}

TEST(LibraryTitleMerge, BlocksExporterMidPhraseTruncation) {
  // The filename stops on a stop word, so it is a cut phrase, not a fuller title.
  EXPECT_FALSE(preferFilenameTitle("Galileo's Error", "Galileo's error _ foundations for a new science of"));
  EXPECT_FALSE(preferFilenameTitle("Mere Christianity",
                                   "Mere Christianity_ a revised and amplified edition, with a"));
  // Too little added to be worth the swap.
  EXPECT_FALSE(preferFilenameTitle("Dark Matter", "Dark Matter : A Novel"));
  EXPECT_FALSE(preferFilenameTitle("The Fury", "The Fury"));
  // Not a prefix at all.
  EXPECT_FALSE(preferFilenameTitle("Pachinko", "Min Lee Jin-Pachonko"));
  // Prefix but not on a word boundary.
  EXPECT_FALSE(preferFilenameTitle("Cosmic", "Cosmical Chemistry Do God Mix"));
  EXPECT_FALSE(preferFilenameTitle("", "anything at all here"));
}

TEST(LibraryMetadataGuard, ClassifiesYearsAndLeavesNamesAlone) {
  EXPECT_TRUE(looksLikeMetadata("2019"));
  EXPECT_TRUE(looksLikeMetadata("1985"));
  EXPECT_TRUE(looksLikeMetadata("2084"));  // in range; see the title test below
  EXPECT_FALSE(looksLikeMetadata("42"));
  EXPECT_FALSE(looksLikeMetadata("blake crouch"));
}

TEST(LibraryMetadataGuard, NeverAppliedToTheTitleSegment) {
  // The guard would classify "2084" as a year, so a title that is a bare year
  // survives only because segment 0 is never classified. This is the invariant
  // that makes the unconditional segment-2+ drop safe, so it gets its own test.
  const auto p = parseFilename("2084 -- John C Lennox -- 2020 -- HarperCollins");
  EXPECT_EQ(p.title, "2084");
  EXPECT_EQ(p.author, "John C Lennox");

  EXPECT_EQ(parseFilename("1984").title, "1984");
  EXPECT_EQ(parseFilename("1984 -- Orwell, George").title, "1984");
}
