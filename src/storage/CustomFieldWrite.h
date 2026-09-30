// SPDX-License-Identifier: AGPL-3.0-or-later

// Write-side counterpart of CustomFieldResolver.h: validate an incoming custom
// field blob and split it into the two columns a Sample carries —
// `custom_fields_json` (plaintext, non-PHI) and `phi_fields_enc_json` (the AEAD
// envelope) — resolving the item type's definitions from the caller's own open
// transaction. Header-only and layered on the public repository API (no SQL),
// like SampleOps.h.
//
// One implementation, every write path: CreateSample, UpdateSample and both CSV
// imports (the ImportSamples RPC and `freezerctl sample import`) call this. They
// disagreeing about *who* performs the split is #108: the import path assigned
// the CSV cell verbatim, so an is_phi key was stored unencrypted in the
// plaintext column and disclosed to every sample.read holder.
#ifndef FMGR_STORAGE_CUSTOMFIELDWRITE_H
#define FMGR_STORAGE_CUSTOMFIELDWRITE_H

#include "core/custom_field_validator.h"
#include "core/identity.h"
#include "crypto/FieldCipher.h"
#include "kms/IKmsProvider.h"
#include "storage/CustomFieldResolver.h"
#include "storage/IStorageBackend.h"
#include "storage/IdentityTraits.h"

#include <nlohmann/json.hpp>

#include <set>
#include <stdexcept>
#include <string>

namespace fmgr::storage {

  // A PHI value that says nothing: an explicit JSON null or an empty string.
  // Both are a blank the caller is asking for, not a value it is supplying, so
  // they cannot count as a PHI write on their own. Anything else — including 0
  // and false, which are real values — is a value. Fields whose data type is
  // not a string are already rejected by validation before this matters.
  [[nodiscard]] inline bool is_blank_phi_value(const nlohmann::json& value) {
    return value.is_null() || (value.is_string() && value.get_ref<const std::string&>().empty());
  }

  // Outcome of partitioning an incoming custom-field blob into the plaintext
  // (non-PHI) column and the encrypted PHI envelope.
  struct PreparedCustomFields {
    std::string custom_fields_json{"{}"};  // non-PHI, validated
    std::string phi_fields_enc_json{"{}"}; // AEAD envelope; "{}" when no PHI
    // The PHI keys the request carried, still in the clear, so UpdateSample can
    // merge a supplied value into the stored envelope without re-parsing the
    // request. Stays in the request scope, is never logged, and is never
    // returned to a caller.
    crypto::PhiFields phi_values;
    // True when at least one supplied PHI key carried a *value* rather than a
    // blank (see is_blank_phi_value). A blank is not a write, so it cannot make
    // a request authoritative for PHI; UpdateSample honours a blank only from a
    // caller that could see what it is clearing. Implies that the request named
    // a PHI-tagged key at all, which is why there is no separate "keys present"
    // flag (review F5 on #83).
    bool has_non_blank_phi_value{false};
    // The PHI keys a *current* definition covers. A request can only name the
    // keys a definition renders a control for, so UpdateSample uses this to tell
    // a key the caller could have sent back — and may therefore clear — from one
    // whose definition is archived and which the request cannot express at all
    // (#87). The keys are metadata, never values.
    //
    // Definitions only, deliberately, even though the set below holds more. This
    // one answers "which stored keys could this request have named", and the stored
    // envelope cannot answer that: a key the envelope holds as PHI but no definition
    // covers has no control that could send it back, so it must stay outside this
    // set — UpdateSample's preservation loop carries over exactly the stored keys
    // this set does *not* contain. Widening this set to the union below would make
    // such a key "a key the request could have named", the loop would stop carrying
    // it, and #87's silent loss would return through the path meant to close #126.
    std::set<std::string> current_phi_keys;
    // Which *column* a request key goes to: the union of the keys a current
    // definition marks is_phi and the keys the stored envelope already holds. Two
    // sets rather than one because the two questions have different answers (#126):
    // the stored envelope is direct evidence that a key is PHI, and it does not stop
    // being evidence when an admin archives the definition or clears its is_phi
    // flag. Classifying from the definitions alone put such a key in
    // custom_fields_json — the plaintext column — where every sample.read holder
    // could read it, including one without phi.read, and where the client cannot
    // even tell it apart from an ordinary field (reveal_phi merges decrypted PHI
    // into custom_fields_json without marking which keys are PHI). Merging these two
    // sets into one is the change that must not happen; see current_phi_keys above.
    std::set<std::string> phi_keys_for_classification;
  };

  // The "no master key is wired" message, and the keys that need one. Naming the
  // keys is what makes the error actionable — key *names* are metadata, not
  // values, and the PHI-read audit records them the same way — so the message
  // keeps them. Built here rather than inline in prepare_custom_fields: the loop
  // sits at nesting level 2 there and pushed that function's cognitive
  // complexity over the threshold CI enforces, and the function genuinely has
  // two jobs once it also has to explain a misconfiguration.
  [[nodiscard]] inline std::string describe_missing_kek(const crypto::PhiFields& phi) {
    std::string message = "no master key is configured; cannot store PHI custom fields:";
    for (const auto& entry : phi) {
      message += " [" + entry.first + "]";
    }
    return message;
  }

  // Validate the combined incoming custom fields, then split them: PHI-tagged
  // keys are encrypted into the envelope under a fresh per-record DEK, the rest
  // stay in the plaintext column. Throws ConstraintViolation (→ INVALID_ARGUMENT)
  // on validation failure or when a PHI value is supplied for a lab that has PHI
  // mode disabled. Throws when PHI is supplied but no KMS is configured: storing
  // it unencrypted is the disclosure this function exists to prevent, so the
  // caller must fail instead.
  //
  // `stored_phi_keys` is the set of field *names* the row's existing envelope holds,
  // as crypto::envelope_field_names() reads them — metadata, no values, no KMS, no
  // decryption. It matters only when the row already has an envelope, which is why
  // the insert-only paths (CreateSample, both CSV imports) leave it empty by
  // default: a row that does not exist yet has no stored envelope whose keys could
  // be evidence. UpdateSample passes it, and that is the only caller that must.
  [[nodiscard]] inline PreparedCustomFields
  prepare_custom_fields(ITransaction& txn, const core::LabId& lab_id,
                        const core::ItemTypeId& item_type_id, const std::string& incoming_json,
                        const kms::IKmsProvider* kms,
                        const std::set<std::string>& stored_phi_keys = {}) {
    const auto definitions = resolve_custom_field_defs(txn, lab_id, item_type_id);
    const auto incoming =
        incoming_json.empty() ? nlohmann::json::object() : nlohmann::json::parse(incoming_json);
    const auto errors = core::validate_custom_fields(definitions, incoming);
    if (!errors.empty()) {
      std::string message = "custom field validation failed:";
      for (const auto& error : errors) {
        message += " [" + error.key + ": " + error.message + "]";
      }
      throw ConstraintViolation(message);
    }

    std::set<std::string> phi_keys;
    for (const auto& def : definitions) {
      if (def.is_phi) {
        phi_keys.insert(def.key);
      }
    }

    PreparedCustomFields prepared;
    prepared.current_phi_keys = phi_keys;
    // The union the split below runs on: what the definitions say, plus what the
    // stored envelope already holds. The two sets above carry the reasoning; the
    // short version is that the envelope is evidence too, so a key it holds as PHI
    // never falls through to the plaintext column (#126) — while the preservation
    // loop keeps using current_phi_keys, definitions alone (#87).
    prepared.phi_keys_for_classification = phi_keys;
    prepared.phi_keys_for_classification.insert(stored_phi_keys.begin(), stored_phi_keys.end());
    nlohmann::json non_phi = nlohmann::json::object();
    crypto::PhiFields phi;
    if (incoming.is_object()) {
      for (const auto& [key, value] : incoming.items()) {
        if (prepared.phi_keys_for_classification.contains(key)) {
          phi.emplace(key, value);
          if (!is_blank_phi_value(value)) {
            prepared.has_non_blank_phi_value = true;
          }
        } else {
          non_phi[key] = value;
        }
      }
    }
    prepared.custom_fields_json = non_phi.dump();

    if (!phi.empty()) {
      const auto lab = txn.repo<core::Lab>().find_by_id(lab_id);
      if (!lab.has_value() || !lab->is_phi_enabled) {
        throw ConstraintViolation(
            "PHI custom fields supplied but PHI mode is disabled for this lab");
      }
      if (kms == nullptr) {
        // Misconfiguration, not a client error: no master key is wired, so the
        // PHI cannot be encrypted. The message names the keys
        // (describe_missing_kek) so the error says what to configure rather than
        // only that something is wrong.
        throw std::runtime_error(describe_missing_kek(phi));
      }
      prepared.phi_fields_enc_json = crypto::encrypt(phi, *kms);
    }
    prepared.phi_values = phi;
    return prepared;
  }

} // namespace fmgr::storage

#endif // FMGR_STORAGE_CUSTOMFIELDWRITE_H
