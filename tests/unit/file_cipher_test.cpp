// SPDX-License-Identifier: AGPL-3.0-or-later

#include "crypto/FieldCipher.h" // CipherError
#include "crypto/FileCipher.h"
#include "kms/EnvVarKms.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

  using fmgr::crypto::BackupManifest;
  using fmgr::crypto::CipherError;
  using fmgr::crypto::FileEnvelopeMeta;
  using fmgr::kms::EnvVarKms;

  // base64 of 32 bytes 0x00..0x1F and a distinct second key.
  constexpr const char* kKekB64 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
  constexpr const char* kKek2B64 = "ICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8=";

  class FileCipherTest : public ::testing::Test {
  protected:
    void SetUp() override {
      static std::atomic<int> counter{0};
      dir_ = std::filesystem::temp_directory_path() /
             ("fmgr-filecipher-" + std::to_string(counter.fetch_add(1)));
      std::filesystem::create_directories(dir_);
    }
    void TearDown() override {
      std::error_code error;
      std::filesystem::remove_all(dir_, error);
    }

    [[nodiscard]] std::filesystem::path path(const std::string& name) const {
      return dir_ / name;
    }

    void write_bytes(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) const {
      std::ofstream out(p, std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }

    [[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path& p) const {
      std::ifstream in(p, std::ios::binary);
      return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    std::filesystem::path dir_;
  };

  std::vector<std::uint8_t> pattern(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
      v[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
    }
    return v;
  }

  TEST_F(FileCipherTest, RoundTripsAtVariousSizes) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    for (const std::size_t size : {std::size_t{0}, std::size_t{100}, std::size_t{200000}}) {
      const auto data = pattern(size);
      const auto src = path("src" + std::to_string(size));
      const auto enc = path("enc" + std::to_string(size));
      const auto dec = path("dec" + std::to_string(size));
      write_bytes(src, data);

      const BackupManifest written = fmgr::crypto::encrypt_file(
          src, enc, kms,
          FileEnvelopeMeta{.schema_version = 9, .backend = "sqlite", .created_at_micros = 123});
      const BackupManifest read = fmgr::crypto::decrypt_file(enc, dec, kms);

      EXPECT_EQ(read_bytes(dec), data) << "size=" << size;
      EXPECT_EQ(written.content_sha256, read.content_sha256);
      EXPECT_FALSE(written.content_sha256.empty());
    }
  }

  TEST_F(FileCipherTest, ManifestFieldsRoundTrip) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto src = path("src");
    write_bytes(src, pattern(50));
    const BackupManifest m = fmgr::crypto::encrypt_file(
        src, path("enc"), kms,
        FileEnvelopeMeta{.schema_version = 13, .backend = "sqlite", .created_at_micros = 999});
    EXPECT_EQ(m.version, 1);
    EXPECT_EQ(m.schema_version, 13);
    EXPECT_EQ(m.backend, "sqlite");
    EXPECT_EQ(m.created_at_micros, 999);
    EXPECT_EQ(m.kek_id, kms.key_id());
    const BackupManifest d = fmgr::crypto::decrypt_file(path("enc"), path("dec"), kms);
    EXPECT_EQ(d.schema_version, 13);
    EXPECT_EQ(d.backend, "sqlite");
    EXPECT_EQ(d.created_at_micros, 999);
  }

  TEST_F(FileCipherTest, WrongKeyRejected) {
    const auto producer = EnvVarKms::from_base64(kKekB64);
    const auto other = EnvVarKms::from_base64(kKek2B64);
    const auto src = path("src");
    write_bytes(src, pattern(500));
    fmgr::crypto::encrypt_file(src, path("enc"), producer, FileEnvelopeMeta{.backend = "sqlite"});
    EXPECT_THROW((void)fmgr::crypto::decrypt_file(path("enc"), path("dec"), other), CipherError);
  }

  TEST_F(FileCipherTest, TamperedCiphertextRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto src = path("src");
    write_bytes(src, pattern(500));
    const auto enc = path("enc");
    fmgr::crypto::encrypt_file(src, enc, kms, FileEnvelopeMeta{.backend = "sqlite"});

    // Flip a byte well past the manifest line (in the ciphertext region).
    auto bytes = read_bytes(enc);
    ASSERT_GT(bytes.size(), 100U);
    bytes[bytes.size() - 5] ^= 0x01;
    write_bytes(enc, bytes);

    EXPECT_THROW((void)fmgr::crypto::decrypt_file(enc, path("dec"), kms), CipherError);
  }

  TEST_F(FileCipherTest, ContentHashMismatchRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto src = path("src");
    write_bytes(src, pattern(300));
    const auto enc = path("enc");
    const BackupManifest m =
        fmgr::crypto::encrypt_file(src, enc, kms, FileEnvelopeMeta{.backend = "sqlite"});

    // Corrupt the recorded content hash in the manifest header without touching
    // the ciphertext: decrypt authenticates every chunk, then catches the mismatch.
    auto bytes = read_bytes(enc);
    std::string text(bytes.begin(), bytes.end());
    const auto pos = text.find(m.content_sha256);
    ASSERT_NE(pos, std::string::npos);
    text[pos] = (text[pos] == 'a') ? 'b' : 'a';
    write_bytes(enc, std::vector<std::uint8_t>(text.begin(), text.end()));

    EXPECT_THROW((void)fmgr::crypto::decrypt_file(enc, path("dec"), kms), CipherError);
  }

  TEST_F(FileCipherTest, PlaintextAbsentFromCiphertext) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const std::string marker = "SUPER_SECRET_PHI_MARKER_0123456789";
    const std::vector<std::uint8_t> data(marker.begin(), marker.end());
    const auto src = path("src");
    write_bytes(src, data);
    const auto enc = path("enc");
    fmgr::crypto::encrypt_file(src, enc, kms, FileEnvelopeMeta{.backend = "sqlite"});

    const auto bytes = read_bytes(enc);
    const std::string blob(bytes.begin(), bytes.end());
    EXPECT_EQ(blob.find(marker), std::string::npos);
  }

  TEST_F(FileCipherTest, MalformedManifestRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    write_bytes(path("garbage"), pattern(200));
    EXPECT_THROW((void)fmgr::crypto::decrypt_file(path("garbage"), path("dec"), kms), CipherError);
  }

  // ---- robustness: crafted / truncated backup files ----

  // F-1 (review 2026-06-19): no newline in "manifest" line — the first line is
  // the entire file.  std::getline would try to read a multi-GB "line" and
  // exhaust memory.  This test uses a moderately large buffer to confirm the
  // current implementation does not crash; it should be accompanied by a code
  // fix that caps the manifest line at 4 KiB and throws CipherError.
  TEST_F(FileCipherTest, GiantManifestWithoutNewlineRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto enc = path("no_nl");
    // Write a non-JSON blob with no '\n' — the entire file becomes the
    // "manifest line".  Keep it moderate for a unit test.
    write_bytes(enc, std::vector<std::uint8_t>(128 * 1024, 0x41));
    EXPECT_THROW((void)fmgr::crypto::decrypt_file(enc, path("dec"), kms), CipherError);
  }

  // Ciphertext truncated mid-chunk-length word (only 2 of 4 bytes available).
  TEST_F(FileCipherTest, TruncatedChunkLengthRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto src = path("src");
    write_bytes(src, pattern(2048));
    const auto enc = path("enc");
    fmgr::crypto::encrypt_file(src, enc, kms, FileEnvelopeMeta{.backend = "sqlite"});

    auto bytes = read_bytes(enc);
    ASSERT_GT(bytes.size(), 20U);   // manifest + at least one chunk
    bytes.resize(bytes.size() - 3); // drop last 3 bytes of a 4-byte length field
    write_bytes(enc, bytes);

    EXPECT_THROW((void)fmgr::crypto::decrypt_file(enc, path("dec"), kms), CipherError);
  }

  // Ciphertext truncated mid-chunk body.
  TEST_F(FileCipherTest, TruncatedChunkBodyRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto src = path("src");
    write_bytes(src, pattern(2048));
    const auto enc = path("enc");
    fmgr::crypto::encrypt_file(src, enc, kms, FileEnvelopeMeta{.backend = "sqlite"});

    auto bytes = read_bytes(enc);
    ASSERT_GT(bytes.size(), 100U);
    bytes.resize(bytes.size() - 5); // trim into a chunk body
    write_bytes(enc, bytes);

    EXPECT_THROW((void)fmgr::crypto::decrypt_file(enc, path("dec"), kms), CipherError);
  }

  // Chunk length field too small for AEAD overhead.
  TEST_F(FileCipherTest, ChunkLengthTooSmallRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto src = path("src");
    write_bytes(src, pattern(2048));
    const auto enc = path("enc");
    fmgr::crypto::encrypt_file(src, enc, kms, FileEnvelopeMeta{.backend = "sqlite"});

    auto bytes = read_bytes(enc);
    const auto nl_pos = std::find(bytes.begin(), bytes.end(), '\n');
    ASSERT_NE(nl_pos, bytes.end());
    const std::size_t off = static_cast<std::size_t>(nl_pos - bytes.begin()) + 1;
    ASSERT_LT(off + 3, bytes.size());
    // Patch the first chunk length to 1 (< crypto_secretstream_ABYTES).
    bytes[off] = 1;
    bytes[off + 1] = 0;
    bytes[off + 2] = 0;
    bytes[off + 3] = 0;
    write_bytes(enc, bytes);

    EXPECT_THROW((void)fmgr::crypto::decrypt_file(enc, path("dec"), kms), CipherError);
  }

  // Missing FINAL chunk tag.
  TEST_F(FileCipherTest, MissingFinalChunkRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto src = path("src");
    write_bytes(src, pattern(100));
    const auto enc = path("enc");
    fmgr::crypto::encrypt_file(src, enc, kms, FileEnvelopeMeta{.backend = "sqlite"});

    auto bytes = read_bytes(enc);
    ASSERT_GT(bytes.size(), 150U);
    bytes.resize(bytes.size() - 6); // drop last chunk's length + a few body bytes
    write_bytes(enc, bytes);

    EXPECT_THROW((void)fmgr::crypto::decrypt_file(enc, path("dec"), kms), CipherError);
  }

  // Corrupt manifest ss_header field.
  TEST_F(FileCipherTest, CorruptSecretStreamHeaderRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto src = path("src");
    write_bytes(src, pattern(300));
    const auto enc = path("enc");
    fmgr::crypto::encrypt_file(src, enc, kms, FileEnvelopeMeta{.backend = "sqlite"});

    auto bytes = read_bytes(enc);
    std::string text(bytes.begin(), bytes.end());
    const auto pos = text.find("\"ss_header\":\"");
    ASSERT_NE(pos, std::string::npos);
    // Flip a base64 char inside the ss_header value.
    const auto val_start = pos + 13;
    text[val_start + 2] = (text[val_start + 2] == 'A') ? 'B' : 'A';
    write_bytes(enc, std::vector<std::uint8_t>(text.begin(), text.end()));

    EXPECT_THROW((void)fmgr::crypto::decrypt_file(enc, path("dec"), kms), CipherError);
  }

  // Wrong magic string in manifest.
  TEST_F(FileCipherTest, WrongMagicRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto src = path("src");
    write_bytes(src, pattern(100));
    const auto enc = path("enc");
    fmgr::crypto::encrypt_file(src, enc, kms, FileEnvelopeMeta{.backend = "sqlite"});

    auto bytes = read_bytes(enc);
    std::string text(bytes.begin(), bytes.end());
    const auto pos = text.find("\"FMGRBAK\"");
    ASSERT_NE(pos, std::string::npos);
    text[pos + 1] = 'X';
    text[pos + 2] = 'X';
    text[pos + 3] = 'X';
    write_bytes(enc, std::vector<std::uint8_t>(text.begin(), text.end()));

    EXPECT_THROW((void)fmgr::crypto::decrypt_file(enc, path("dec"), kms), CipherError);
  }

  // Manifest missing wrapped DEK.
  TEST_F(FileCipherTest, ManifestMissingDekRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto src = path("src");
    write_bytes(src, pattern(100));
    const auto enc = path("enc");
    fmgr::crypto::encrypt_file(src, enc, kms, FileEnvelopeMeta{.backend = "sqlite"});

    auto bytes = read_bytes(enc);
    std::string text(bytes.begin(), bytes.end());
    const auto dek_start = text.find("\"dek\":");
    const auto dek_end = text.find('}', dek_start);
    ASSERT_NE(dek_start, std::string::npos);
    ASSERT_NE(dek_end, std::string::npos);
    text.erase(dek_start, dek_end - dek_start);
    write_bytes(enc, std::vector<std::uint8_t>(text.begin(), text.end()));

    EXPECT_THROW((void)fmgr::crypto::decrypt_file(enc, path("dec"), kms), CipherError);
  }

  // Oversized manifest header (no newline) must be rejected by the bounded read
  // rather than read unboundedly into memory (review F-1).
  TEST_F(FileCipherTest, OversizedManifestHeaderRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto enc = path("enc");
    // 5 MiB first "line" with no newline: well over the 4096-byte cap.
    write_bytes(enc, std::vector<std::uint8_t>(std::size_t{5} * 1024 * 1024, 'A'));
    EXPECT_THROW((void)fmgr::crypto::decrypt_file(enc, path("dec"), kms), CipherError);
  }

} // namespace
