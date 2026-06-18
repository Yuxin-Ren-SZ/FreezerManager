// SPDX-License-Identifier: AGPL-3.0-or-later

#include "crypto/FieldCipher.h"

#include <sodium.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fmgr::crypto {

  namespace {

    constexpr int k_envelope_version = 1;

    void ensure_sodium() {
      if (sodium_init() < 0) {
        throw CipherError("libsodium initialization failed");
      }
    }

    std::string b64_encode(const std::vector<std::uint8_t>& bytes) {
      const std::size_t encoded_len =
          sodium_base64_encoded_len(bytes.size(), sodium_base64_VARIANT_ORIGINAL);
      std::string out(encoded_len, '\0');
      sodium_bin2base64(out.data(), out.size(), bytes.data(), bytes.size(),
                        sodium_base64_VARIANT_ORIGINAL);
      // sodium writes a trailing NUL counted in encoded_len; trim it.
      if (!out.empty() && out.back() == '\0') {
        out.pop_back();
      }
      return out;
    }

    std::vector<std::uint8_t> b64_decode(const std::string& text) {
      std::vector<std::uint8_t> out(text.size()); // decoded is always <= input
      std::size_t decoded_len = 0;
      if (sodium_base642bin(out.data(), out.size(), text.data(), text.size(), nullptr, &decoded_len,
                            nullptr, sodium_base64_VARIANT_ORIGINAL) != 0) {
        throw CipherError("envelope contains invalid base64");
      }
      out.resize(decoded_len);
      return out;
    }

    // Seal a plaintext under `key` (32-byte secretbox key) with a fresh nonce.
    nlohmann::json seal(const std::string& plaintext, const std::vector<std::uint8_t>& key) {
      std::vector<std::uint8_t> nonce(crypto_secretbox_NONCEBYTES);
      randombytes_buf(nonce.data(), nonce.size());
      std::vector<std::uint8_t> ciphertext(plaintext.size() + crypto_secretbox_MACBYTES);
      crypto_secretbox_easy(ciphertext.data(),
                            reinterpret_cast<const unsigned char*>(plaintext.data()),
                            plaintext.size(), nonce.data(), key.data());
      return nlohmann::json{{"n", b64_encode(nonce)}, {"c", b64_encode(ciphertext)}};
    }

    // Open a {"n","c"} object sealed under `key`. Throws on auth failure.
    std::string open(const nlohmann::json& sealed, const std::vector<std::uint8_t>& key) {
      if (!sealed.is_object() || !sealed.contains("n") || !sealed.contains("c")) {
        throw CipherError("envelope field is malformed");
      }
      const std::vector<std::uint8_t> nonce = b64_decode(sealed.at("n").get<std::string>());
      const std::vector<std::uint8_t> ciphertext = b64_decode(sealed.at("c").get<std::string>());
      if (nonce.size() != crypto_secretbox_NONCEBYTES ||
          ciphertext.size() < crypto_secretbox_MACBYTES) {
        throw CipherError("envelope field has malformed nonce or ciphertext");
      }
      std::string plaintext(ciphertext.size() - crypto_secretbox_MACBYTES, '\0');
      if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char*>(plaintext.data()),
                                     ciphertext.data(), ciphertext.size(), nonce.data(),
                                     key.data()) != 0) {
        throw CipherError("PHI field failed authentication");
      }
      return plaintext;
    }

  } // namespace

  std::string encrypt(const PhiFields& fields, const kms::IKmsProvider& kms) {
    if (fields.empty()) {
      return "{}";
    }
    ensure_sodium();

    // Fresh per-record DEK.
    std::vector<std::uint8_t> dek(crypto_secretbox_KEYBYTES);
    randombytes_buf(dek.data(), dek.size());

    nlohmann::json out_fields = nlohmann::json::object();
    for (const auto& [key, value] : fields) {
      out_fields[key] = seal(value.dump(), dek);
    }

    const kms::WrappedDek wrapped = kms.wrap_dek(dek);
    sodium_memzero(dek.data(), dek.size());

    const nlohmann::json envelope{
        {"v", k_envelope_version},
        {"kek_id", kms.key_id()},
        {"dek", {{"n", b64_encode(wrapped.nonce)}, {"c", b64_encode(wrapped.ciphertext)}}},
        {"fields", out_fields},
    };
    return envelope.dump();
  }

  PhiFields decrypt(const std::string& envelope_json, const kms::IKmsProvider& kms) {
    PhiFields result;
    // Treat empty / "{}" as no PHI.
    std::string trimmed = envelope_json;
    if (trimmed.empty()) {
      return result;
    }
    nlohmann::json envelope;
    try {
      envelope = nlohmann::json::parse(trimmed);
    } catch (const nlohmann::json::exception&) {
      throw CipherError("PHI envelope is not valid JSON");
    }
    if (!envelope.is_object() || envelope.empty() || !envelope.contains("dek")) {
      return result; // "{}" or an empty object: no PHI present.
    }
    ensure_sodium();

    const nlohmann::json& dek_obj = envelope.at("dek");
    if (!dek_obj.is_object() || !dek_obj.contains("n") || !dek_obj.contains("c")) {
      throw CipherError("PHI envelope has a malformed wrapped DEK");
    }
    kms::WrappedDek wrapped;
    wrapped.nonce = b64_decode(dek_obj.at("n").get<std::string>());
    wrapped.ciphertext = b64_decode(dek_obj.at("c").get<std::string>());
    std::vector<std::uint8_t> dek = kms.unwrap_dek(wrapped);

    const auto fields_iter = envelope.find("fields");
    if (fields_iter != envelope.end() && fields_iter->is_object()) {
      for (const auto& [key, sealed] : fields_iter->items()) {
        result[key] = nlohmann::json::parse(open(sealed, dek));
      }
    }
    sodium_memzero(dek.data(), dek.size());
    return result;
  }

} // namespace fmgr::crypto
