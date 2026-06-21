// SPDX-License-Identifier: AGPL-3.0-or-later

#include "crypto/FileCipher.h"

#include "core/sodium_init.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace fmgr::crypto {

  namespace {

    constexpr int k_manifest_version = 1;
    constexpr const char* k_magic = "FMGRBAK";
    constexpr std::size_t k_chunk_size = std::size_t{64} * 1024; // plaintext bytes per chunk

    // Hard cap on the manifest header (the first line). A real manifest is a few
    // hundred bytes; the cap stops a crafted file from exhausting memory with a
    // multi-gigabyte first "line" before the chunk parser runs (review F-1).
    constexpr std::size_t k_max_manifest_bytes = 4096;

    void ensure_sodium() {
      if (!core::sodium_ready()) {
        throw CipherError("libsodium initialization failed");
      }
    }

    std::string b64_encode(const std::vector<std::uint8_t>& bytes) {
      const std::size_t encoded_len =
          sodium_base64_encoded_len(bytes.size(), sodium_base64_VARIANT_ORIGINAL);
      std::string out(encoded_len, '\0');
      sodium_bin2base64(out.data(), out.size(), bytes.data(), bytes.size(),
                        sodium_base64_VARIANT_ORIGINAL);
      if (!out.empty() && out.back() == '\0') {
        out.pop_back();
      }
      return out;
    }

    std::vector<std::uint8_t> b64_decode(const std::string& text) {
      std::vector<std::uint8_t> out(text.size());
      std::size_t decoded_len = 0;
      if (sodium_base642bin(out.data(), out.size(), text.data(), text.size(), nullptr, &decoded_len,
                            nullptr, sodium_base64_VARIANT_ORIGINAL) != 0) {
        throw CipherError("backup manifest contains invalid base64");
      }
      out.resize(decoded_len);
      return out;
    }

    std::string sha256_file_hex(const std::filesystem::path& path) {
      std::ifstream input(path, std::ios::binary);
      if (!input) {
        throw CipherError("cannot open input for hashing: " + path.string());
      }
      crypto_hash_sha256_state state;
      crypto_hash_sha256_init(&state);
      std::vector<char> buffer(k_chunk_size);
      while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(input.gcount());
        if (got > 0) {
          crypto_hash_sha256_update(&state, reinterpret_cast<const unsigned char*>(buffer.data()),
                                    got);
        }
      }
      std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
      crypto_hash_sha256_final(&state, digest.data());
      std::string hex(digest.size() * 2 + 1, '\0');
      sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
      hex.pop_back(); // drop the NUL sodium counts in the length
      return hex;
    }

    void write_u32_le(std::ostream& out, std::uint32_t value) {
      std::array<char, 4> bytes{
          static_cast<char>(value & 0xFF),
          static_cast<char>((value >> 8) & 0xFF),
          static_cast<char>((value >> 16) & 0xFF),
          static_cast<char>((value >> 24) & 0xFF),
      };
      out.write(bytes.data(), bytes.size());
    }

    // Read a 4-byte little-endian length. Returns false on clean EOF.
    bool read_u32_le(std::istream& input, std::uint32_t& value) {
      std::array<unsigned char, 4> bytes{};
      input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
      if (input.gcount() == 0) {
        return false;
      }
      if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw CipherError("truncated backup: incomplete chunk length");
      }
      value = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
              (static_cast<std::uint32_t>(bytes[2]) << 16) |
              (static_cast<std::uint32_t>(bytes[3]) << 24);
      return true;
    }

    // Read the manifest header (first line) with a hard size cap so a crafted
    // file cannot exhaust memory with a multi-gigabyte first "line" before the
    // chunk parser runs (review F-1).
    std::string read_manifest_line(std::istream& input) {
      std::string line;
      char next = 0;
      bool saw_any = false;
      while (input.get(next)) {
        saw_any = true;
        if (next == '\n') {
          break;
        }
        line.push_back(next);
        if (line.size() > k_max_manifest_bytes) {
          throw CipherError("backup manifest header exceeds maximum size");
        }
      }
      if (!saw_any) {
        throw CipherError("backup file has no manifest header");
      }
      return line;
    }

  } // namespace

  BackupManifest encrypt_file(const std::filesystem::path& in_path,
                              const std::filesystem::path& out_path, const kms::IKmsProvider& kms,
                              const FileEnvelopeMeta& meta) {
    ensure_sodium();

    // Pass 1: hash the plaintext so the manifest can record an integrity check.
    // This opens `in_path` a first time; pass 2 below opens it again to stream
    // the ciphertext. The caller guarantees `in_path` is a private temp file it
    // alone owns (e.g. `<out>.hotcopy.tmp` / `<out>.pgdump.tmp`), so no other
    // writer can swap the file between the two opens (review F-3). The decrypt
    // content-hash check would catch any such mismatch as corruption regardless.
    const std::string content_sha256 = sha256_file_hex(in_path);

    std::ifstream input(in_path, std::ios::binary);
    if (!input) {
      throw CipherError("cannot open input file: " + in_path.string());
    }
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      throw CipherError("cannot open output file: " + out_path.string());
    }

    std::vector<std::uint8_t> dek(crypto_secretstream_xchacha20poly1305_KEYBYTES);
    randombytes_buf(dek.data(), dek.size());
    const kms::WrappedDek wrapped = kms.wrap_dek(dek);

    crypto_secretstream_xchacha20poly1305_state state;
    std::vector<std::uint8_t> ss_header(crypto_secretstream_xchacha20poly1305_HEADERBYTES);
    crypto_secretstream_xchacha20poly1305_init_push(&state, ss_header.data(), dek.data());
    sodium_memzero(dek.data(), dek.size());

    const nlohmann::json manifest{
        {"magic", k_magic},
        {"v", k_manifest_version},
        {"kek_id", kms.key_id()},
        {"dek", {{"n", b64_encode(wrapped.nonce)}, {"c", b64_encode(wrapped.ciphertext)}}},
        {"ss_header", b64_encode(ss_header)},
        {"schema_version", meta.schema_version},
        {"backend", meta.backend},
        {"created_at_micros", meta.created_at_micros},
        {"content_sha256", content_sha256},
    };
    out << manifest.dump() << '\n';

    // Pass 2: stream-encrypt the plaintext in chunks.
    std::vector<unsigned char> plain(k_chunk_size);
    std::vector<unsigned char> cipher(k_chunk_size + crypto_secretstream_xchacha20poly1305_ABYTES);
    while (true) {
      input.read(reinterpret_cast<char*>(plain.data()), static_cast<std::streamsize>(plain.size()));
      const auto got = static_cast<std::size_t>(input.gcount());
      const bool last = !input; // EOF reached on this read
      const unsigned char tag = last ? crypto_secretstream_xchacha20poly1305_TAG_FINAL : 0;
      unsigned long long cipher_len = 0;
      crypto_secretstream_xchacha20poly1305_push(&state, cipher.data(), &cipher_len, plain.data(),
                                                 got, nullptr, 0, tag);
      write_u32_le(out, static_cast<std::uint32_t>(cipher_len));
      out.write(reinterpret_cast<const char*>(cipher.data()),
                static_cast<std::streamsize>(cipher_len));
      if (last) {
        break;
      }
    }
    if (!out) {
      throw CipherError("failed writing backup file: " + out_path.string());
    }

    return BackupManifest{
        .version = k_manifest_version,
        .kek_id = kms.key_id(),
        .schema_version = meta.schema_version,
        .backend = meta.backend,
        .created_at_micros = meta.created_at_micros,
        .content_sha256 = content_sha256,
    };
  }

  BackupManifest decrypt_file(const std::filesystem::path& in_path,
                              const std::filesystem::path& out_path, const kms::IKmsProvider& kms) {
    ensure_sodium();

    std::ifstream input(in_path, std::ios::binary);
    if (!input) {
      throw CipherError("cannot open backup file: " + in_path.string());
    }
    const std::string manifest_line = read_manifest_line(input);
    nlohmann::json manifest;
    try {
      manifest = nlohmann::json::parse(manifest_line);
    } catch (const nlohmann::json::exception&) {
      throw CipherError("backup manifest is not valid JSON");
    }
    if (!manifest.is_object() || manifest.value("magic", std::string{}) != k_magic ||
        !manifest.contains("dek") || !manifest.contains("ss_header")) {
      throw CipherError("backup manifest is malformed or not a FreezerManager backup");
    }

    const nlohmann::json& dek_obj = manifest.at("dek");
    if (!dek_obj.is_object() || !dek_obj.contains("n") || !dek_obj.contains("c")) {
      throw CipherError("backup manifest has a malformed wrapped DEK");
    }
    kms::WrappedDek wrapped;
    wrapped.nonce = b64_decode(dek_obj.at("n").get<std::string>());
    wrapped.ciphertext = b64_decode(dek_obj.at("c").get<std::string>());
    const auto kek_id = manifest.value("kek_id", std::string{});

    std::vector<std::uint8_t> dek;
    try {
      dek = kms.unwrap_dek(wrapped, kek_id);
    } catch (const kms::KmsError& error) {
      throw CipherError(std::string("cannot unwrap backup DEK: ") + error.what());
    }

    const std::vector<std::uint8_t> ss_header =
        b64_decode(manifest.at("ss_header").get<std::string>());
    crypto_secretstream_xchacha20poly1305_state state;
    if (ss_header.size() != crypto_secretstream_xchacha20poly1305_HEADERBYTES ||
        crypto_secretstream_xchacha20poly1305_init_pull(&state, ss_header.data(), dek.data()) !=
            0) {
      sodium_memzero(dek.data(), dek.size());
      throw CipherError("backup secretstream header is invalid");
    }
    sodium_memzero(dek.data(), dek.size());

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      throw CipherError("cannot open output file: " + out_path.string());
    }

    crypto_hash_sha256_state hash_state;
    crypto_hash_sha256_init(&hash_state);

    std::uint32_t cipher_len = 0;
    bool saw_final = false;
    std::vector<unsigned char> cipher;
    std::vector<unsigned char> plain;
    while (read_u32_le(input, cipher_len)) {
      cipher.resize(cipher_len);
      input.read(reinterpret_cast<char*>(cipher.data()), cipher_len);
      if (static_cast<std::uint32_t>(input.gcount()) != cipher_len) {
        throw CipherError("truncated backup: incomplete ciphertext chunk");
      }
      if (cipher_len < crypto_secretstream_xchacha20poly1305_ABYTES) {
        throw CipherError("backup ciphertext chunk is too short");
      }
      plain.resize(cipher_len - crypto_secretstream_xchacha20poly1305_ABYTES);
      unsigned long long plain_len = 0;
      unsigned char tag = 0;
      if (crypto_secretstream_xchacha20poly1305_pull(&state, plain.data(), &plain_len, &tag,
                                                     cipher.data(), cipher_len, nullptr, 0) != 0) {
        throw CipherError("backup chunk failed authentication (tampered or wrong key)");
      }
      crypto_hash_sha256_update(&hash_state, plain.data(), plain_len);
      out.write(reinterpret_cast<const char*>(plain.data()),
                static_cast<std::streamsize>(plain_len));
      if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
        saw_final = true;
        break;
      }
    }
    if (!saw_final) {
      throw CipherError("truncated backup: missing final chunk");
    }
    if (!out) {
      throw CipherError("failed writing restored file: " + out_path.string());
    }

    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&hash_state, digest.data());
    std::string hex(digest.size() * 2 + 1, '\0');
    sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
    hex.pop_back();

    const auto expected = manifest.value("content_sha256", std::string{});
    if (hex != expected) {
      throw CipherError("backup content hash mismatch (corruption)");
    }

    return BackupManifest{
        .version = manifest.value("v", 0),
        .kek_id = kek_id,
        .schema_version = manifest.value("schema_version", 0),
        .backend = manifest.value("backend", std::string{}),
        .created_at_micros = manifest.value("created_at_micros", std::int64_t{0}),
        .content_sha256 = expected,
    };
  }

} // namespace fmgr::crypto
