// SPDX-License-Identifier: AGPL-3.0-or-later

#include "kms/EnvVarKms.h"
#include "kms/KeyringKms.h"
#include "kms/OsKeyringKms.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

  using fmgr::kms::EnvVarKms;
  using fmgr::kms::KeyringKms;
  using fmgr::kms::KmsError;
  using fmgr::kms::OsKeyringKms;
  using fmgr::kms::WrappedDek;

  constexpr const char* kKekB64 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";

  std::vector<std::uint8_t> kek_a_bytes() {
    std::vector<std::uint8_t> kek(32);
    for (std::size_t i = 0; i < kek.size(); ++i) {
      kek[i] = static_cast<std::uint8_t>(i); // == decoded kKekB64
    }
    return kek;
  }

  std::vector<std::uint8_t> kek_b_bytes() {
    std::vector<std::uint8_t> kek(32);
    for (std::size_t i = 0; i < kek.size(); ++i) {
      kek[i] = static_cast<std::uint8_t>(0x40 + i);
    }
    return kek;
  }

  std::vector<std::uint8_t> sample_dek() {
    std::vector<std::uint8_t> dek(32, 0xCD);
    return dek;
  }

  class CredentialsDir : public ::testing::Test {
  protected:
    void SetUp() override {
      static std::atomic<int> counter{0};
      dir_ = std::filesystem::temp_directory_path() /
             ("fmgr-creds-" + std::to_string(counter.fetch_add(1)));
      std::filesystem::create_directories(dir_);
    }
    void TearDown() override {
      std::error_code error;
      std::filesystem::remove_all(dir_, error);
    }

    void write_file(const std::string& name, const std::string& text) const {
      std::ofstream(dir_ / name, std::ios::binary) << text;
    }
    void write_raw(const std::string& name, const std::vector<std::uint8_t>& bytes) const {
      std::ofstream out(dir_ / name, std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }

    std::filesystem::path dir_;
  };

  TEST_F(CredentialsDir, LoadsActiveBase64Key) {
    write_file("master_kek", std::string(kKekB64) + "\n"); // trailing newline tolerated
    const auto kms = OsKeyringKms::from_credentials_dir(dir_);
    const auto dek = sample_dek();
    EXPECT_EQ(kms.unwrap_dek(kms.wrap_dek(dek), kms.key_id()), dek);
  }

  TEST_F(CredentialsDir, LoadsRawBinaryKey) {
    write_raw("master_kek", kek_a_bytes());
    const auto kms = OsKeyringKms::from_credentials_dir(dir_);
    EXPECT_EQ(kms.key_id(), KeyringKms::fingerprint(kek_a_bytes()));
  }

  TEST_F(CredentialsDir, RetiredKeysStillUnwrap) {
    // A record wrapped under KEK-A; keyring is active KEK-B with KEK-A retired.
    const EnvVarKms producer(kek_a_bytes());
    const WrappedDek wrapped = producer.wrap_dek(sample_dek());

    write_raw("master_kek", kek_b_bytes());
    write_raw("master_kek.prev.001", kek_a_bytes());
    const auto kms = OsKeyringKms::from_credentials_dir(dir_);

    EXPECT_EQ(kms.key_id(), KeyringKms::fingerprint(kek_b_bytes()));
    EXPECT_EQ(kms.unwrap_dek(wrapped, producer.key_id()), sample_dek());
  }

  TEST_F(CredentialsDir, MissingActiveFileFailsFast) {
    EXPECT_THROW((void)OsKeyringKms::from_credentials_dir(dir_), KmsError);
  }

  TEST_F(CredentialsDir, BadKeyFileFailsFast) {
    write_file("master_kek", "not-a-valid-key");
    EXPECT_THROW((void)OsKeyringKms::from_credentials_dir(dir_), KmsError);
  }

  TEST_F(CredentialsDir, FromSystemdCredentialsReadsEnv) {
    write_file("master_kek", kKekB64);
    ::setenv("CREDENTIALS_DIRECTORY", dir_.c_str(), 1); // NOLINT(concurrency-mt-unsafe)
    const auto kms = OsKeyringKms::from_systemd_credentials();
    const auto dek = sample_dek();
    EXPECT_EQ(kms.unwrap_dek(kms.wrap_dek(dek), kms.key_id()), dek);
    ::unsetenv("CREDENTIALS_DIRECTORY"); // NOLINT(concurrency-mt-unsafe)
  }

  TEST(OsKeyringKms, MissingDirFailsFast) {
    EXPECT_THROW((void)OsKeyringKms::from_credentials_dir("/nonexistent/fmgr/creds/dir/xyz"),
                 KmsError);
  }

  TEST(OsKeyringKms, MissingEnvFailsFast) {
    ::unsetenv("CREDENTIALS_DIRECTORY"); // NOLINT(concurrency-mt-unsafe)
    EXPECT_THROW((void)OsKeyringKms::from_systemd_credentials(), KmsError);
  }

} // namespace
