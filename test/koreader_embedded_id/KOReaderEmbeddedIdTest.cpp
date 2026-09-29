#include <gtest/gtest.h>

#include <string>

#include "KOReaderEmbeddedId.h"
#include "ZipFile.h"

namespace {
constexpr const char* VALID_ID = "0123456789abcdef0123456789abcdef";
constexpr const char* VALID_JSON = "{\"version\":1,\"koreaderPartialMd5\":\"0123456789abcdef0123456789abcdef\"}";
}  // namespace

TEST(EmbeddedIdParse, AcceptsCanonicalPayload) { EXPECT_EQ(KOReaderEmbeddedId::parse(VALID_JSON), VALID_ID); }

TEST(EmbeddedIdParse, AcceptsWhitespaceVariants) {
  EXPECT_EQ(KOReaderEmbeddedId::parse(
                "{ \"version\" : 1 ,\n  \"koreaderPartialMd5\" : \"0123456789abcdef0123456789abcdef\" }"),
            VALID_ID);
}

TEST(EmbeddedIdParse, RejectsMissingVersion) {
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"koreaderPartialMd5\":\"0123456789abcdef0123456789abcdef\"}"), "");
}

TEST(EmbeddedIdParse, RejectsUnknownVersion) {
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"version\":2,\"koreaderPartialMd5\":\"0123456789abcdef0123456789abcdef\"}"),
            "");
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"version\":12,\"koreaderPartialMd5\":\"0123456789abcdef0123456789abcdef\"}"),
            "");
}

TEST(EmbeddedIdParse, RejectsMalformedIds) {
  // Uppercase hex, short, long, non-string, empty input.
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"version\":1,\"koreaderPartialMd5\":\"0123456789ABCDEF0123456789ABCDEF\"}"),
            "");
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"version\":1,\"koreaderPartialMd5\":\"0123\"}"), "");
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"version\":1,\"koreaderPartialMd5\":\"0123456789abcdef0123456789abcdef0\"}"),
            "");
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"version\":1,\"koreaderPartialMd5\":42}"), "");
  EXPECT_EQ(KOReaderEmbeddedId::parse(""), "");
}

TEST(EmbeddedIdRead, ReadsCannedEntry) {
  ZipFile::openable = true;
  ZipFile::entryName = KOReaderEmbeddedId::SYNC_ID_PATH;
  ZipFile::entryContent = VALID_JSON;
  EXPECT_EQ(KOReaderEmbeddedId::read("/books/x.epub"), VALID_ID);
}

TEST(EmbeddedIdRead, EmptyWhenEntryMissing) {
  ZipFile::openable = true;
  ZipFile::entryName = "META-INF/container.xml";
  ZipFile::entryContent = VALID_JSON;
  EXPECT_EQ(KOReaderEmbeddedId::read("/books/x.epub"), "");
}

TEST(EmbeddedIdRead, EmptyWhenOversized) {
  ZipFile::openable = true;
  ZipFile::entryName = KOReaderEmbeddedId::SYNC_ID_PATH;
  ZipFile::entryContent = std::string(4096, 'x');
  EXPECT_EQ(KOReaderEmbeddedId::read("/books/x.epub"), "");
}

TEST(EmbeddedIdRead, EmptyWhenZipUnopenable) {
  ZipFile::openable = false;
  ZipFile::entryName = KOReaderEmbeddedId::SYNC_ID_PATH;
  ZipFile::entryContent = VALID_JSON;
  EXPECT_EQ(KOReaderEmbeddedId::read("/books/x.epub"), "");
}

TEST(EmbeddedIdRead, EmptyWhenSizeZero) {
  ZipFile::openable = true;
  ZipFile::entryName = KOReaderEmbeddedId::SYNC_ID_PATH;
  ZipFile::entryContent = "";
  EXPECT_EQ(KOReaderEmbeddedId::read("/books/x.epub"), "");
}

TEST(EmbeddedIdRead, EmptyWhenPayloadMalformed) {
  ZipFile::openable = true;
  ZipFile::entryName = KOReaderEmbeddedId::SYNC_ID_PATH;
  ZipFile::entryContent = "{\"version\":1,\"koreaderPartialMd5\":\"0123456789ABCDEF0123456789ABCDEF\"}";
  EXPECT_EQ(KOReaderEmbeddedId::read("/books/x.epub"), "");
}

// --- Quelles identités une sync lit et écrit ---------------------------------
// Readest et KOReader identifient un livre par le contenu du fichier QU'ILS ont.
// Un livre optimisé ici porte deux identités : l'original (embarquée) et cette
// copie. Écrire sous les deux est ce qui permet à un appareil qui a l'original
// comme à un appareil qui a la copie de voir la progression.

#include "KOReaderSyncIdentities.h"

namespace {
constexpr const char* ORIGINAL = "15002bce62c030f58f1dede893e5d66d";
constexpr const char* COPY = "f0772a3c7848df88067073a828695479";
constexpr const char* BY_NAME = "538c7c4e9d910b01851bca3f1eb844a4";
}  // namespace

TEST(SyncIdentities, UploadUnderTheOriginalAlsoWritesThisCopy) {
  EXPECT_EQ(koreaderCompanionUploadHash(ORIGINAL, ORIGINAL, COPY), COPY);
}

TEST(SyncIdentities, UploadUnderThisCopyAlsoWritesTheOriginal) {
  EXPECT_EQ(koreaderCompanionUploadHash(COPY, ORIGINAL, COPY), ORIGINAL);
}

TEST(SyncIdentities, NoEmbeddedIdMeansASingleUpload) { EXPECT_EQ(koreaderCompanionUploadHash(COPY, "", COPY), ""); }

TEST(SyncIdentities, AnUnoptimizedBookCarryingItsOwnIdIsWrittenOnce) {
  // Livre dont l'empreinte embarquée est déjà celle de son contenu.
  EXPECT_EQ(koreaderCompanionUploadHash(ORIGINAL, ORIGINAL, ORIGINAL), "");
}

TEST(SyncIdentities, AnUnreadableCopyAddsNothing) {
  EXPECT_EQ(koreaderCompanionUploadHash(ORIGINAL, ORIGINAL, ""), "");
}

TEST(SyncIdentities, AFilenameUploadGetsNoCompanion) {
  // Ni l'original ni la copie : rien à apparier.
  EXPECT_EQ(koreaderCompanionUploadHash(BY_NAME, ORIGINAL, COPY), "");
}

TEST(SyncIdentities, AnEmbeddedIdProbesTheOtherIdentitiesInEveryMode) {
  EXPECT_TRUE(koreaderProbeAlternateIdentities(/*smartSync=*/false, /*hasEmbeddedId=*/true));
  EXPECT_TRUE(koreaderProbeAlternateIdentities(true, true));
  EXPECT_TRUE(koreaderProbeAlternateIdentities(true, false));
  EXPECT_FALSE(koreaderProbeAlternateIdentities(false, false));
}
