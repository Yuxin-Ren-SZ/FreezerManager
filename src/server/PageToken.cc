// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/PageToken.h"

#include "storage/IStorageBackend.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <string>
#include <vector>

namespace fmgr::server {
  namespace {

    void ensure_sodium() {
      if (sodium_init() < 0) {
        throw storage::ConstraintViolation("page token codec unavailable");
      }
    }

    [[nodiscard]] std::string b64url_encode(const std::string& text) {
      ensure_sodium();
      const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
      const std::size_t encoded_len =
          sodium_base64_encoded_len(text.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
      std::string out(encoded_len, '\0');
      sodium_bin2base64(out.data(), out.size(), bytes, text.size(),
                        sodium_base64_VARIANT_URLSAFE_NO_PADDING);
      if (!out.empty() && out.back() == '\0') {
        out.pop_back();
      }
      return out;
    }

    [[nodiscard]] std::string b64url_decode(std::string_view token) {
      ensure_sodium();
      std::string out(token.size(), '\0');
      std::size_t decoded_len = 0;
      if (sodium_base642bin(reinterpret_cast<unsigned char*>(out.data()), out.size(), token.data(),
                            token.size(), nullptr, &decoded_len, nullptr,
                            sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        throw storage::ConstraintViolation("malformed page token");
      }
      out.resize(decoded_len);
      return out;
    }

  } // namespace

  std::string encode_sample_page_token(const SamplePageCursor& cursor) {
    const nlohmann::json payload{{"v", 1},
                                 {"kind", "sample"},
                                 {"last_modified_at", cursor.last_modified_at_micros},
                                 {"id", cursor.id}};
    return b64url_encode(payload.dump());
  }

  SamplePageCursor decode_sample_page_token(std::string_view token) {
    try {
      const auto payload = nlohmann::json::parse(b64url_decode(token));
      if (!payload.is_object() || payload.value("v", 0) != 1 ||
          payload.value("kind", std::string{}) != "sample" ||
          !payload.contains("last_modified_at") || !payload.contains("id") ||
          !payload.at("last_modified_at").is_number_integer() || !payload.at("id").is_string()) {
        throw storage::ConstraintViolation("malformed page token");
      }
      return SamplePageCursor{.last_modified_at_micros =
                                  payload.at("last_modified_at").get<std::int64_t>(),
                              .id = payload.at("id").get<std::string>()};
    } catch (const storage::ConstraintViolation&) {
      throw;
    } catch (const std::exception&) {
      throw storage::ConstraintViolation("malformed page token");
    }
  }

} // namespace fmgr::server
