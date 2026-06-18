// SPDX-License-Identifier: AGPL-3.0-or-later

#include "crypto/FieldCipher.h"
#include "kms/EnvVarKms.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>

namespace {

  using fmgr::crypto::CipherError;
  using fmgr::crypto::decrypt;
  using fmgr::crypto::encrypt;
  using fmgr::crypto::PhiFields;
  using fmgr::kms::EnvVarKms;

  constexpr const char* kKekB64 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";

  EnvVarKms make_kms() {
    return EnvVarKms::from_base64(kKekB64);
  }

  TEST(FieldCipher, RoundTripsSingleField) {
    const auto kms = make_kms();
    PhiFields in{{"patient_id", "MRN-12345"}};

    const std::string envelope = encrypt(in, kms);
    const PhiFields out = decrypt(envelope, kms);

    EXPECT_EQ(out, in);
  }

  TEST(FieldCipher, RoundTripsMultipleMixedTypes) {
    const auto kms = make_kms();
    PhiFields in{
        {"patient_id", "MRN-12345"},
        {"age", 42},
        {"consented", true},
        {"diagnosis", nlohmann::json{{"code", "C50"}, {"text", "neoplasm"}}},
    };

    EXPECT_EQ(decrypt(encrypt(in, kms), kms), in);
  }

  TEST(FieldCipher, EmptyMapSerializesToEmptyObject) {
    const auto kms = make_kms();
    EXPECT_EQ(encrypt(PhiFields{}, kms), "{}");
  }

  TEST(FieldCipher, EmptyAndDefaultEnvelopesDecryptToEmpty) {
    const auto kms = make_kms();
    EXPECT_TRUE(decrypt("{}", kms).empty());
    EXPECT_TRUE(decrypt("", kms).empty());
  }

  TEST(FieldCipher, PlaintextNeverAppearsInEnvelope) {
    const auto kms = make_kms();
    const std::string secret = "SUPER-SECRET-MRN-99999";
    PhiFields in{{"patient_id", secret}};

    const std::string envelope = encrypt(in, kms);
    EXPECT_EQ(envelope.find(secret), std::string::npos);
    // The field key name is not secret and may appear; the value must not.
  }

  TEST(FieldCipher, FreshDekPerEncryptYieldsDifferentCiphertext) {
    const auto kms = make_kms();
    PhiFields in{{"patient_id", "MRN-12345"}};

    EXPECT_NE(encrypt(in, kms), encrypt(in, kms));
  }

  TEST(FieldCipher, TamperedFieldCiphertextRejected) {
    const auto kms = make_kms();
    PhiFields in{{"patient_id", "MRN-12345"}};
    nlohmann::json envelope = nlohmann::json::parse(encrypt(in, kms));

    // Flip a base64 char in the field ciphertext.
    std::string ct = envelope["fields"]["patient_id"]["c"].get<std::string>();
    ct[0] = (ct[0] == 'A') ? 'B' : 'A';
    envelope["fields"]["patient_id"]["c"] = ct;

    EXPECT_THROW((void)decrypt(envelope.dump(), kms), CipherError);
  }

  TEST(FieldCipher, WrongKekCannotDecrypt) {
    const auto producer = make_kms();
    PhiFields in{{"patient_id", "MRN-12345"}};
    const std::string envelope = encrypt(in, producer);

    const EnvVarKms other(std::vector<std::uint8_t>(32, 0x5A));
    EXPECT_THROW((void)decrypt(envelope, other), fmgr::kms::KmsError);
  }

  TEST(FieldCipher, MalformedEnvelopeThrows) {
    const auto kms = make_kms();
    EXPECT_THROW((void)decrypt("{not json", kms), CipherError);
  }

} // namespace
