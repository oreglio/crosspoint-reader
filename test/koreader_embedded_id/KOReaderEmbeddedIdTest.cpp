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
