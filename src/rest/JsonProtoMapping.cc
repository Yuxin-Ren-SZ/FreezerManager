// SPDX-License-Identifier: AGPL-3.0-or-later

#include "rest/JsonProtoMapping.h"

#include <google/protobuf/util/json_util.h>

#include <string>

namespace fmgr::rest {

  void json_to_message(const std::string& json, google::protobuf::Message& out) {
    google::protobuf::util::JsonParseOptions opts;
    // Fail-closed: an unrecognized field is a client mistake, not something to
    // silently ignore — surfacing it as a 400 catches typos early.
    opts.ignore_unknown_fields = false;
    const auto status = google::protobuf::util::JsonStringToMessage(json, &out, opts);
    if (!status.ok()) {
      throw BadJson(std::string(status.message()));
    }
  }

  std::string message_to_json(const google::protobuf::Message& msg) {
    google::protobuf::util::JsonPrintOptions opts;
    // Emit proto field names (snake_case) rather than lowerCamelCase, matching
    // the .proto definitions and the CSV column model so a field has one name
    // across every interface.
    opts.preserve_proto_field_names = true;
    std::string out;
    const auto status = google::protobuf::util::MessageToJsonString(msg, &out, opts);
    if (!status.ok()) {
      // Serializing a well-formed message should not fail; treat as internal.
      throw BadJson(std::string(status.message()));
    }
    return out;
  }

} // namespace fmgr::rest
