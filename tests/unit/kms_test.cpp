// SPDX-License-Identifier: AGPL-3.0-or-later

#include "kms/EnvVarKms.h"
#include "kms/IKmsProvider.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

  using fmgr::kms::EnvVarKms;
  using fmgr::kms::KmsError;
  using fmgr::kms::WrappedDek;

  // base64 (original variant) of 32 bytes 0x00..0x1F.
  constexpr const char* kKekB64 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";

  std::vector<std::uint8_t> sample_dek() {
    // A 32-byte DEK (the size a crypto_secretbox key would be).
    std::vector<std::uint8_t> dek(32);
    for (std::size_t i = 0; i < dek.size(); ++i) {
      dek[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    return dek;
  }

  TEST(EnvVarKms, WrapUnwrapRoundTrips) {
    const auto kms = EnvVarKms::from_base64(kKekB64);
    const auto dek = sample_dek();

    const WrappedDek wrapped = kms.wrap_dek(dek);
    const std::vector<std::uint8_t> recovered = kms.unwrap_dek(wrapped);

    EXPECT_EQ(recovered, dek);
    // The wrapped ciphertext must not equal the plaintext DEK.
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
    EXPECT_THROW((void)kms.unwrap_dek(wrapped), KmsError);
  }

  TEST(EnvVarKms, WrongKeyCannotUnwrap) {
    const auto producer = EnvVarKms::from_base64(kKekB64);
    const WrappedDek wrapped = producer.wrap_dek(sample_dek());

    // A different 32-byte KEK.
    std::vector<std::uint8_t> other_kek(32, 0x7E);
    const EnvVarKms other(std::move(other_kek));
    EXPECT_THROW((void)other.unwrap_dek(wrapped), KmsError);
  }

  TEST(EnvVarKms, ShortKeyFailsFast) {
    std::vector<std::uint8_t> too_short(16, 0x11);
    EXPECT_THROW(EnvVarKms{std::move(too_short)}, KmsError);
  }

  TEST(EnvVarKms, BadBase64FailsFast) {
    EXPECT_THROW((void)EnvVarKms::from_base64("not*valid*base64"), KmsError);
    // Valid base64 but wrong length (16 bytes).
    EXPECT_THROW((void)EnvVarKms::from_base64("AAECAwQFBgcICQoLDA0ODw=="), KmsError);
  }

  TEST(EnvVarKms, MissingEnvFailsFast) {
    ::unsetenv("FMGR_MASTER_KEK"); // NOLINT(concurrency-mt-unsafe)
    EXPECT_THROW((void)EnvVarKms::from_env(), KmsError);
  }

  TEST(EnvVarKms, ReadsEnvVar) {
    ::setenv("FMGR_MASTER_KEK", kKekB64, 1); // NOLINT(concurrency-mt-unsafe)
    const auto kms = EnvVarKms::from_env();
    const auto dek = sample_dek();
    EXPECT_EQ(kms.unwrap_dek(kms.wrap_dek(dek)), dek);
    ::unsetenv("FMGR_MASTER_KEK"); // NOLINT(concurrency-mt-unsafe)
  }

  TEST(EnvVarKms, KeyIdIsStableAndNonEmpty) {
    const auto a = EnvVarKms::from_base64(kKekB64);
    const auto b = EnvVarKms::from_base64(kKekB64);
    EXPECT_FALSE(a.key_id().empty());
    EXPECT_EQ(a.key_id(), b.key_id());
  }

} // namespace
