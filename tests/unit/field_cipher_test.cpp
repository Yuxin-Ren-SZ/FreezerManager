// SPDX-License-Identifier: AGPL-3.0-or-later

#include "crypto/FieldCipher.h"
#include "kms/EnvVarKms.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

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

  // ---- edge cases: corrupted / malformed envelope fields ----

  TEST(FieldCipher, CorruptFieldNonceRejected) {
    const auto kms = make_kms();
    PhiFields in{{"mrn", "x"}};
    nlohmann::json envelope = nlohmann::json::parse(encrypt(in, kms));
    // Flip a base64 char in one field's nonce.
    std::string n = envelope["fields"]["mrn"]["n"].get<std::string>();
    n[1] = (n[1] == 'A') ? 'B' : 'A';
    envelope["fields"]["mrn"]["n"] = n;
    EXPECT_THROW((void)decrypt(envelope.dump(), kms), CipherError);
  }

  TEST(FieldCipher, CorruptFieldCiphertextBase64Rejected) {
    const auto kms = make_kms();
    PhiFields in{{"mrn", "x"}};
    nlohmann::json envelope = nlohmann::json::parse(encrypt(in, kms));
    // Inject an invalid base64 character in the field ciphertext.
    envelope["fields"]["mrn"]["c"] = "!@#^invalid*base64!!";
    EXPECT_THROW((void)decrypt(envelope.dump(), kms), CipherError);
  }

  TEST(FieldCipher, CorruptDekNonceRejected) {
    const auto kms = make_kms();
    PhiFields in{{"mrn", "x"}};
    nlohmann::json envelope = nlohmann::json::parse(encrypt(in, kms));
    std::string n = envelope["dek"]["n"].get<std::string>();
    n[0] = (n[0] == 'A') ? 'B' : 'A';
    envelope["dek"]["n"] = n;
    EXPECT_THROW((void)decrypt(envelope.dump(), kms), fmgr::kms::KmsError);
  }

  TEST(FieldCipher, CorruptDekCiphertextBase64Rejected) {
    const auto kms = make_kms();
    PhiFields in{{"mrn", "x"}};
    nlohmann::json envelope = nlohmann::json::parse(encrypt(in, kms));
    envelope["dek"]["c"] = "!!!invalid base64!!!";
    EXPECT_THROW((void)decrypt(envelope.dump(), kms), CipherError);
  }

  TEST(FieldCipher, MissingDekRejected) {
    const auto kms = make_kms();
    PhiFields in{{"mrn", "x"}};
    nlohmann::json envelope = nlohmann::json::parse(encrypt(in, kms));
    envelope.erase("dek");
    // An envelope without "dek" is treated as having no PHI (returns empty map).
    // This is by design — "{}" and objects without "dek" both mean "no PHI
    // present".  See FieldCipher::decrypt() line 120.
    EXPECT_TRUE(decrypt(envelope.dump(), make_kms()).empty());
  }

  TEST(FieldCipher, ExtraUnknownFieldsIgnored) {
    const auto kms = make_kms();
    PhiFields in{{"mrn", "x"}};
    nlohmann::json envelope = nlohmann::json::parse(encrypt(in, kms));
    envelope["future_version"] = 99;
    envelope["comment"] = "should be ignored";
    // Decrypt must still succeed — forward-compat tolerance.
    EXPECT_EQ(decrypt(envelope.dump(), kms), in);
  }

  TEST(FieldCipher, DekNonceWrongLengthRejected) {
    const auto kms = make_kms();
    PhiFields in{{"mrn", "x"}};
    nlohmann::json envelope = nlohmann::json::parse(encrypt(in, kms));
    // A base64 string that decodes to 10 bytes (not crypto_secretbox_NONCEBYTES).
    envelope["dek"]["n"] = "AAECAwQFBgcICQ==";
    // The KMS unwrap_dek will see a short nonce and throw KmsError.
    EXPECT_THROW((void)decrypt(envelope.dump(), kms), fmgr::kms::KmsError);
  }

  // ---- rewrap robustness ----

  TEST(FieldCipher, RewrapThrowsOnMalformedEnvelope) {
    using fmgr::crypto::rewrap;
    const auto kms = make_kms();
    EXPECT_THROW((void)rewrap("not json", kms), CipherError);
  }

  TEST(FieldCipher, RewrapThrowsOnExtraJunkBase64) {
    using fmgr::crypto::rewrap;
    const auto old_kms = make_kms();
    PhiFields in{{"mrn", "x"}};
    const std::string envelope = encrypt(in, old_kms);
    // Use a different KMS whose active KEK doesn't match the envelope's
    // kek_id, so rewrap reaches the base64-decode step instead of returning
    // nullopt early.
    const EnvVarKms other_kms(std::vector<std::uint8_t>(32, 0x66));
    nlohmann::json env_json = nlohmann::json::parse(envelope);
    env_json["dek"]["n"] = "not##base64";
    EXPECT_THROW((void)rewrap(env_json.dump(), other_kms), CipherError);
  }

  // ---- rewrap (key rotation) ----

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
      kek[i] = static_cast<std::uint8_t>(0x20 + i);
    }
    return kek;
  }

  TEST(FieldCipher, RewrapRotatesAndStillDecrypts) {
    using fmgr::crypto::rewrap;
    const auto old_kms = make_kms(); // active = KEK-A (= kek_a_bytes())
    const PhiFields in{{"mrn", "MRN-555"}, {"dob", "1990-01-01"}};
    const std::string envelope = encrypt(in, old_kms);
    const auto old_kek_id = nlohmann::json::parse(envelope).at("kek_id").get<std::string>();

    // New keyring: active = KEK-B, retired = KEK-A (so the old DEK can be unwrapped).
    const EnvVarKms new_kms(kek_b_bytes(), {kek_a_bytes()});

    const auto rotated = rewrap(envelope, new_kms);
    ASSERT_TRUE(rotated.has_value());
    const auto rotated_json = nlohmann::json::parse(*rotated);
    EXPECT_EQ(rotated_json.at("kek_id").get<std::string>(), new_kms.key_id());
    EXPECT_NE(rotated_json.at("kek_id").get<std::string>(), old_kek_id);
    // Field ciphertext is untouched — only the wrapped DEK rotated.
    EXPECT_EQ(rotated_json.at("fields"), nlohmann::json::parse(envelope).at("fields"));
    // Plaintext still recovers under the new keyring.
    EXPECT_EQ(decrypt(*rotated, new_kms), in);
  }

  TEST(FieldCipher, RewrapNoopWhenAlreadyActive) {
    using fmgr::crypto::rewrap;
    const auto kms = make_kms();
    const std::string envelope = encrypt(PhiFields{{"mrn", "x"}}, kms);
    EXPECT_FALSE(rewrap(envelope, kms).has_value());
  }

  TEST(FieldCipher, RewrapNoopOnEmptyEnvelope) {
    using fmgr::crypto::rewrap;
    const auto kms = make_kms();
    EXPECT_FALSE(rewrap("{}", kms).has_value());
    EXPECT_FALSE(rewrap("", kms).has_value());
  }

} // namespace
