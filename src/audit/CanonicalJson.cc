// SPDX-License-Identifier: AGPL-3.0-or-later

#include "audit/CanonicalJson.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fmgr::audit {

  namespace {

    // Decode a UTF-8 string to its UTF-16 code-unit sequence. RFC 8785 §3.2.3
    // sorts object members by the UTF-16 code units of their keys; for ASCII keys
    // this is identical to a byte comparison, but non-BMP keys (which encode as a
    // surrogate pair, first unit 0xD800..) order differently from raw UTF-8, so
    // the transform is required for full compliance.
    [[nodiscard]] std::u16string to_utf16(std::string_view input) {
      std::u16string out;
      std::size_t pos = 0;
      while (pos < input.size()) {
        const auto byte0 = static_cast<unsigned char>(input[pos]);
        char32_t code_point = 0;
        std::size_t len = 1;
        if (byte0 < 0x80) {
          code_point = byte0;
          len = 1;
        } else if ((byte0 >> 5) == 0x6) {
          code_point = byte0 & 0x1FU;
          len = 2;
        } else if ((byte0 >> 4) == 0xE) {
          code_point = byte0 & 0x0FU;
          len = 3;
        } else if ((byte0 >> 3) == 0x1E) {
          code_point = byte0 & 0x07U;
          len = 4;
        } else {
          code_point = 0xFFFD; // invalid lead byte
          len = 1;
        }
        for (std::size_t cont = 1; cont < len && pos + cont < input.size(); ++cont) {
          code_point = (code_point << 6) | (static_cast<unsigned char>(input[pos + cont]) & 0x3FU);
        }
        pos += len;
        if (code_point <= 0xFFFF) {
          out.push_back(static_cast<char16_t>(code_point));
        } else {
          code_point -= 0x10000;
          out.push_back(static_cast<char16_t>(0xD800 + (code_point >> 10)));
          out.push_back(static_cast<char16_t>(0xDC00 + (code_point & 0x3FFU)));
        }
      }
      return out;
    }

    // Serialize a JSON string with RFC 8785 §3.2.2.2 minimal escaping: only ",
    // \, and control characters below U+0020 are escaped (short forms where
    // ECMAScript JSON.stringify defines them, else lowercase \u00xx). All other
    // bytes, including U+007F and multibyte UTF-8, pass through verbatim.
    void append_escaped_string(std::string& out, std::string_view value) {
      constexpr std::string_view hex = "0123456789abcdef";
      out.push_back('"');
      for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '\b':
          out += "\\b";
          break;
        case '\t':
          out += "\\t";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\f':
          out += "\\f";
          break;
        case '\r':
          out += "\\r";
          break;
        default:
          if (c < 0x20) {
            out += "\\u00";
            out.push_back(hex[(c >> 4) & 0xFU]);
            out.push_back(hex[c & 0xFU]);
          } else {
            out.push_back(raw);
          }
        }
      }
      out.push_back('"');
    }

    void serialize(std::string& out, const nlohmann::json& value) {
      switch (value.type()) {
      case nlohmann::json::value_t::null:
        out += "null";
        break;
      case nlohmann::json::value_t::boolean:
        out += value.get<bool>() ? "true" : "false";
        break;
      case nlohmann::json::value_t::string:
        append_escaped_string(out, value.get_ref<const nlohmann::json::string_t&>());
        break;
      case nlohmann::json::value_t::number_integer:
      case nlohmann::json::value_t::number_unsigned:
      case nlohmann::json::value_t::number_float:
        // Integers serialize as their exact decimal form (already the RFC 8785
        // §3.2.2.3 representation). FreezerManager's canonicalized content (audit
        // rows) contains no floats; any incidental float uses nlohmann's shortest
        // round-trip form, which matches ECMAScript for the common range — full
        // IEEE-754 edge cases are out of scope for scheme v1.
        out += value.dump();
        break;
      case nlohmann::json::value_t::array: {
        out.push_back('[');
        bool first = true;
        for (const auto& element : value) {
          if (!first) {
            out.push_back(',');
          }
          first = false;
          serialize(out, element);
        }
        out.push_back(']');
        break;
      }
      case nlohmann::json::value_t::object: {
        struct Member {
          std::u16string sort_key;
          std::string key;
          const nlohmann::json* value;
        };
        std::vector<Member> members;
        members.reserve(value.size());
        for (const auto& item : value.items()) {
          members.push_back({to_utf16(item.key()), item.key(), &item.value()});
        }
        std::sort(members.begin(), members.end(),
                  [](const Member& a, const Member& b) { return a.sort_key < b.sort_key; });
        out.push_back('{');
        bool first = true;
        for (const auto& member : members) {
          if (!first) {
            out.push_back(',');
          }
          first = false;
          append_escaped_string(out, member.key);
          out.push_back(':');
          serialize(out, *member.value);
        }
        out.push_back('}');
        break;
      }
      default:
        // discarded / binary are not produced by FreezerManager content.
        throw std::invalid_argument("canonical_json: unsupported JSON value type");
      }
    }

  } // namespace

  std::string canonical_json(const nlohmann::json& value) {
    // RFC 8785 (JSON Canonicalization Scheme), scheme v1: object members sorted
    // by UTF-16 code units, compact (no whitespace), minimal string escaping,
    // exact integer serialization. Deterministic and independent of nlohmann's
    // internal key ordering.
    std::string out;
    serialize(out, value);
    return out;
  }

  std::string compute_audit_hash(std::string_view prev_hash, std::string_view content_json) {
    if (sodium_init() < 0) {
      throw std::runtime_error("libsodium initialisation failed");
    }
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    crypto_hash_sha256_update(&state, reinterpret_cast<const unsigned char*>(prev_hash.data()),
                              prev_hash.size());
    crypto_hash_sha256_update(&state, reinterpret_cast<const unsigned char*>(content_json.data()),
                              content_json.size());
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256_final(&state, hash.data());

    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    return {hex.data()};
  }

} // namespace fmgr::audit
