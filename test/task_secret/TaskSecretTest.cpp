#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "tasks/TaskSecret.h"

namespace {

std::string encode(const uint8_t (&bytes)[TASK_SECRET_BYTES]) {
  char out[TASK_SECRET_LEN + 1];
  std::memset(out, '#', sizeof(out));
  taskSecretEncode(bytes, out);
  return std::string(out);
}

// Recopie caractere pour caractere de la classe que le serveur garde apres
// normalisation (crossdrop src/taskApi.ts:172, /[^0-9A-HJKMNP-TV-Z]/g). Un
// caractere hors de cette classe serait retire du code tape sur le web mais
// garde dans l'en-tete Bearer : les deux hashes divergeraient.
bool serverKeeps(const char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'H') || c == 'J' || c == 'K' || c == 'M' || c == 'N' ||
         (c >= 'P' && c <= 'T') || (c >= 'V' && c <= 'Z');
}

// Proprietes de la forme canonique, verifiees sur chaque sortie de
// l'encodeur : c'est la forme stockee puis envoyee telle quelle en Bearer.
void expectCanonical(const std::string& s) {
  ASSERT_EQ(s.size(), TASK_SECRET_LEN) << s;
  for (const char c : s) {
    EXPECT_TRUE(serverKeeps(c)) << "'" << c << "' in " << s;
    EXPECT_NE(c, ' ') << s;
    EXPECT_NE(c, '-') << s;
    EXPECT_FALSE(c >= 'a' && c <= 'z') << s;
    EXPECT_EQ(std::strchr("ILOU", c), nullptr) << s;
  }
  EXPECT_TRUE(taskSecretIsCanonical(s.c_str())) << s;
}

}  // namespace

TEST(TaskSecretTest, AlphabetIsExactlyWhatTheServerKeeps) {
  ASSERT_EQ(std::strlen(TASK_SECRET_ALPHABET), 32u);
  for (size_t i = 0; i < 32; ++i) EXPECT_TRUE(serverKeeps(TASK_SECRET_ALPHABET[i])) << TASK_SECRET_ALPHABET[i];
  // Et reciproquement : la classe du serveur compte 32 caracteres, tous
  // atteignables par l'encodeur.
  int kept = 0;
  for (int c = 0; c < 128; ++c) {
    if (!serverKeeps(static_cast<char>(c))) continue;
    ++kept;
    EXPECT_NE(std::strchr(TASK_SECRET_ALPHABET, c), nullptr) << static_cast<char>(c);
  }
  EXPECT_EQ(kept, 32);
}

TEST(TaskSecretTest, AllZeroBytesEncodeToZeros) {
  const uint8_t bytes[TASK_SECRET_BYTES] = {};
  const std::string s = encode(bytes);
  EXPECT_EQ(s, "00000000000000000000000000");
  expectCanonical(s);
}

TEST(TaskSecretTest, AllOnesEndWithTheThreeTrailingBitsPaddedByZeros) {
  uint8_t bytes[TASK_SECRET_BYTES];
  std::memset(bytes, 0xFF, sizeof(bytes));
  const std::string s = encode(bytes);
  // 25 caracteres pleins (31 = 'Z'), puis 0b111 complete en 0b11100 = 28 = 'W'.
  EXPECT_EQ(s, "ZZZZZZZZZZZZZZZZZZZZZZZZZW");
  expectCanonical(s);
}

// Vecteurs de reference produits hors du depot par base64.b32encode (RFC 4648,
// meme ordre de bits) puis transcrits dans l'alphabet Crockford, et passes au
// normaliseur du serveur, qui les rend inchanges.
TEST(TaskSecretTest, KnownVectorsMatchAnIndependentEncoder) {
  const uint8_t ramp[TASK_SECRET_BYTES] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                           0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
  EXPECT_EQ(encode(ramp), "000G40R40M30E209185GR38E1W");

  const uint8_t mixed[TASK_SECRET_BYTES] = {0x8f, 0x3a, 0x1c, 0x5e, 0x9b, 0x02, 0xd7, 0x46,
                                            0x6a, 0xe1, 0xf0, 0xc3, 0x4b, 0x9d, 0x28, 0x75};
  const std::string s = encode(mixed);
  EXPECT_EQ(s, "HWX1RQMV0BBMCTQ1Y31MQ798EM");
  expectCanonical(s);
}

// Chaque valeur de 5 bits a chaque position : aucune ne doit sortir de
// l'alphabet, ni devenir une minuscule ou un separateur.
TEST(TaskSecretTest, EveryOutputIsCanonical) {
  uint8_t bytes[TASK_SECRET_BYTES];
  for (int seed = 0; seed < 256; ++seed) {
    for (size_t i = 0; i < TASK_SECRET_BYTES; ++i) bytes[i] = static_cast<uint8_t>(seed * 37 + i * 101);
    expectCanonical(encode(bytes));
  }
}

TEST(TaskSecretTest, IsCanonicalRejectsEveryFormTheServerWouldHashDifferently) {
  EXPECT_TRUE(taskSecretIsCanonical("HWX1RQMV0BBMCTQ1Y31MQ798EM"));
  EXPECT_FALSE(taskSecretIsCanonical(nullptr));
  EXPECT_FALSE(taskSecretIsCanonical(""));
  EXPECT_FALSE(taskSecretIsCanonical("HWX1RQMV0BBMCTQ1Y31MQ798E"));    // 25
  EXPECT_FALSE(taskSecretIsCanonical("HWX1RQMV0BBMCTQ1Y31MQ798EMA"));  // 27
  EXPECT_FALSE(taskSecretIsCanonical("hwx1rqmv0bbmctq1y31mq798em"));   // minuscules
  EXPECT_FALSE(taskSecretIsCanonical("HWX1 RQMV 0BBM CTQ1 Y31M Q798 EM"));
  EXPECT_FALSE(taskSecretIsCanonical("HWX1RQMV0BBMCTQ1Y31MQ798E "));
  EXPECT_FALSE(taskSecretIsCanonical("HWX1RQMV0BBMCTQ1Y31MQ798EI"));  // I hors alphabet
  EXPECT_FALSE(taskSecretIsCanonical("OWX1RQMV0BBMCTQ1Y31MQ798EM"));  // O hors alphabet
}

TEST(TaskSecretTest, GroupingIsDisplayOnlyAndLeavesTheCanonicalFormIntact) {
  const char canonical[] = "HWX1RQMV0BBMCTQ1Y31MQ798EM";
  char grouped[TASK_SECRET_GROUPED_LEN + 1];
  ASSERT_TRUE(taskSecretGroup(canonical, grouped));
  EXPECT_STREQ(grouped, "HWX1 RQMV 0BBM CTQ1 Y31M Q798 EM");
  EXPECT_EQ(std::strlen(grouped), TASK_SECRET_GROUPED_LEN);
  // La forme groupee n'est PAS canonique : elle ne doit jamais atteindre
  // writeSecret().
  EXPECT_FALSE(taskSecretIsCanonical(grouped));
  EXPECT_STREQ(canonical, "HWX1RQMV0BBMCTQ1Y31MQ798EM");
}

TEST(TaskSecretTest, GroupingRefusesANonCanonicalInput) {
  char grouped[TASK_SECRET_GROUPED_LEN + 1];
  std::memset(grouped, '#', sizeof(grouped));
  EXPECT_FALSE(taskSecretGroup("hwx1rqmv0bbmctq1y31mq798em", grouped));
  EXPECT_STREQ(grouped, "");
}

// Le tampon de relecture de l'ecran d'appairage est remis a zero puis rempli
// par readSecret() : un secret tronque y laisse des NUL jusqu'au bout. C'est
// la garde « s[i] == '\0' » qui le rejette — pas le hasard d'un octet non nul
// lu au-dela d'un litteral.
TEST(TaskSecretTest, IsCanonicalRejectsATruncatedValueInAZeroedBuffer) {
  char stored[TASK_SECRET_LEN + 2] = {};
  std::memcpy(stored, "HWX1RQMV0BBMCTQ1Y31MQ798E", TASK_SECRET_LEN - 1);
  EXPECT_FALSE(taskSecretIsCanonical(stored));

  char empty[TASK_SECRET_LEN + 2] = {};
  EXPECT_FALSE(taskSecretIsCanonical(empty));
}
