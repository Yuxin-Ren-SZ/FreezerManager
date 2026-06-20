// SPDX-License-Identifier: AGPL-3.0-or-later

#include "kms/EnvVarKms.h"
#include "kms/IKmsProvider.h"
#include "kms/KeyringKms.h"
#include "kms/KmsFactory.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

  using fmgr::kms::EnvVarKms;
  using fmgr::kms::KeyringKms;
  using fmgr::kms::KmsError;
  using fmgr::kms::WrappedDek;

  // base64 (original variant) of 32 bytes 0x00..0x1F.
  constexpr const char* kKekB64 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
  // base64 of 32 bytes 0x20..0x3F (a distinct second KEK).
  constexpr const char* kKek2B64 = "ICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8=";

  std::vector<std::uint8_t> sample_dek() {
    std::vector<std::uint8_t> dek(32);
    for (std::size_t i = 0; i < dek.size(); ++i) {
      dek[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    return dek;
  }

  std::vector<std::uint8_t> kek_a() {
    std::vector<std::uint8_t> kek(32);
    for (std::size_t i = 0; i < kek.size(); ++i) {
      kek[i] = static_cast<std::uint8_t>(i);
    }
    return kek;
  }

  TEST(EnvVarKms, WrapUnwrapRoundTrips) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto dek = sample_dek();

    const WrappedDek wrapped = kms.wrap_dek(dek);
    const std::vector<std::uint8_t> recovered = kms.unwrap_dek(wrapped, kms.key_id());

    EXPECT_EQ(recovered, dek);
    EXPECT_NE(wrapped.ciphertext, dek);
  }

  TEST(EnvVarKms, FreshNoncePerWrap) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto dek = sample_dek();

    const WrappedDek a = kms.wrap_dek(dek);
    const WrappedDek b = kms.wrap_dek(dek);

    EXPECT_NE(a.nonce, b.nonce);
    EXPECT_NE(a.ciphertext, b.ciphertext);
  }

  TEST(EnvVarKms, TamperedCiphertextRejected) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    WrappedDek wrapped = kms.wrap_dek(sample_dek());

    wrapped.ciphertext[0] ^= 0x01;
    EXPECT_THROW((void)kms.unwrap_dek(wrapped, kms.key_id()), KmsError);
  }

  TEST(EnvVarKms, WrongKeyCannotUnwrap) {
    const auto producer = EnvVarKms::from_base64(kKekB64);
    const WrappedDek wrapped = producer.wrap_dek(sample_dek());

    // A keyring holding a different KEK does not know the producer's kek_id.
    const EnvVarKms other(std::vector<std::uint8_t>(32, 0x7E));
    EXPECT_THROW((void)other.unwrap_dek(wrapped, producer.key_id()), KmsError);
  }

  TEST(EnvVarKms, ShortKeyFailsFast) {
    std::vector<std::uint8_t> too_short(16, 0x11);
    EXPECT_THROW(EnvVarKms{std::move(too_short)}, KmsError);
  }

  TEST(EnvVarKms, BadBase64FailsFast) {
    EXPECT_THROW((void)EnvVarKms::from_base64("not*valid*base64"), KmsError);
    EXPECT_THROW((void)EnvVarKms::from_base64("AAECAwQFBgcICQoLDA0ODw=="), KmsError);
  }

  TEST(EnvVarKms, MissingEnvFailsFast) {
    ::unsetenv("FMGR_MASTER_KEK"); // NOLINT(concurrency-mt-unsafe)
    EXPECT_THROW((void)EnvVarKms::from_env(), KmsError);
  }

  TEST(EnvVarKms, ReadsEnvVar) {
    ::setenv("FMGR_MASTER_KEK", kKekB64, 1); // NOLINT(concurrency-mt-unsafe)
    ::unsetenv("FMGR_MASTER_KEK_PREVIOUS");  // NOLINT(concurrency-mt-unsafe)
    const auto kms = EnvVarKms::from_env();
    const auto dek = sample_dek();
    EXPECT_EQ(kms.unwrap_dek(kms.wrap_dek(dek), kms.key_id()), dek);
    ::unsetenv("FMGR_MASTER_KEK"); // NOLINT(concurrency-mt-unsafe)
  }

  TEST(EnvVarKms, KeyIdIsStableAndNonEmpty) {
    const auto a = EnvVarKms::from_base64(kKekB64);
    const auto b = EnvVarKms::from_base64(kKekB64);
    EXPECT_FALSE(a.key_id().empty());
    EXPECT_EQ(a.key_id(), b.key_id());
  }

  // ---- Keyring (active + retired) ----

  TEST(KeyringKms, RetiredKeyStillUnwraps) {
    // Wrap under KEK-A, then build a keyring whose ACTIVE key is KEK-2 but which
    // retains KEK-A as a retired key. The old record must still decrypt.
    const auto producer = EnvVarKms::from_base64(kKekB64); // active = A (= kek_a())
    const WrappedDek wrapped = producer.wrap_dek(sample_dek());
    const std::string a_id = producer.key_id();

    const EnvVarKms keyring(/*active=*/std::vector<std::uint8_t>(32, 0x55), /*retired=*/{kek_a()});

    EXPECT_NE(keyring.key_id(), a_id);
    EXPECT_EQ(keyring.unwrap_dek(wrapped, a_id), sample_dek());
  }

  TEST(KeyringKms, UnknownKekIdThrows) {
    const auto producer = EnvVarKms::from_base64(kKekB64);
    const WrappedDek wrapped = producer.wrap_dek(sample_dek());
    const EnvVarKms keyring(std::vector<std::uint8_t>(32, 0x55));
    EXPECT_THROW((void)keyring.unwrap_dek(wrapped, "00000000deadbeef"), KmsError);
  }

  TEST(KeyringKms, FingerprintMatchesRegisteredId) {
    const EnvVarKms keyring(kek_a());
    EXPECT_EQ(keyring.key_id(), KeyringKms::fingerprint(kek_a()));
  }

  // ---- Backup key (independent of the master KEK, PRD §8/§14) ----

  TEST(EnvVarKms, FromNamedEnvVarsLoadIndependentKey) {
    ::setenv("FMGR_BACKUP_KEK", kKek2B64, 1); // NOLINT(concurrency-mt-unsafe)
    ::unsetenv("FMGR_BACKUP_KEK_PREVIOUS");   // NOLINT(concurrency-mt-unsafe)
    const auto kms = EnvVarKms::from_env("FMGR_BACKUP_KEK", "FMGR_BACKUP_KEK_PREVIOUS");
    const auto dek = sample_dek();
    EXPECT_EQ(kms.unwrap_dek(kms.wrap_dek(dek), kms.key_id()), dek);
    ::unsetenv("FMGR_BACKUP_KEK"); // NOLINT(concurrency-mt-unsafe)
  }

  TEST(KmsFactory, BackupKmsIsDistinctFromMasterAndCannotCrossUnwrap) {
    ::unsetenv("CREDENTIALS_DIRECTORY");      // NOLINT(concurrency-mt-unsafe)
    ::setenv("FMGR_MASTER_KEK", kKekB64, 1);  // NOLINT(concurrency-mt-unsafe)
    ::setenv("FMGR_BACKUP_KEK", kKek2B64, 1); // NOLINT(concurrency-mt-unsafe)
    ::unsetenv("FMGR_MASTER_KEK_PREVIOUS");   // NOLINT(concurrency-mt-unsafe)
    ::unsetenv("FMGR_BACKUP_KEK_PREVIOUS");   // NOLINT(concurrency-mt-unsafe)

    auto master = fmgr::kms::make_default_kms();
    auto backup = fmgr::kms::make_backup_kms();
    ASSERT_NE(master, nullptr);
    ASSERT_NE(backup, nullptr);
    EXPECT_NE(master->key_id(), backup->key_id());

    // A DEK wrapped by the backup key does not unwrap under the master key.
    const WrappedDek wrapped = backup->wrap_dek(sample_dek());
    EXPECT_THROW((void)master->unwrap_dek(wrapped, backup->key_id()), KmsError);

    ::unsetenv("FMGR_MASTER_KEK"); // NOLINT(concurrency-mt-unsafe)
    ::unsetenv("FMGR_BACKUP_KEK"); // NOLINT(concurrency-mt-unsafe)
  }

  TEST(KmsFactory, BackupKmsNullWhenUnconfigured) {
    ::unsetenv("CREDENTIALS_DIRECTORY"); // NOLINT(concurrency-mt-unsafe)
    ::unsetenv("FMGR_BACKUP_KEK");       // NOLINT(concurrency-mt-unsafe)
    EXPECT_EQ(fmgr::kms::make_backup_kms(), nullptr);
  }

  TEST(EnvVarKms, EnvPreviousKeysAreRetainedForUnwrap) {
    // Active = KEK-2, previous (retired) = KEK-A. A record wrapped under A decrypts.
    const auto producer = EnvVarKms::from_base64(kKekB64); // A
    const WrappedDek wrapped = producer.wrap_dek(sample_dek());
    const std::string a_id = producer.key_id();

    ::setenv("FMGR_MASTER_KEK", kKek2B64, 1);         // NOLINT(concurrency-mt-unsafe)
    ::setenv("FMGR_MASTER_KEK_PREVIOUS", kKekB64, 1); // NOLINT(concurrency-mt-unsafe)
    const auto kms = EnvVarKms::from_env();
    EXPECT_NE(kms.key_id(), a_id);
    EXPECT_EQ(kms.unwrap_dek(wrapped, a_id), sample_dek());
    ::unsetenv("FMGR_MASTER_KEK");          // NOLINT(concurrency-mt-unsafe)
    ::unsetenv("FMGR_MASTER_KEK_PREVIOUS"); // NOLINT(concurrency-mt-unsafe)
  }

} // namespace
