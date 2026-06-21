// SPDX-License-Identifier: AGPL-3.0-or-later

// JSON <-> protobuf message translation for the REST gateway.
//
// The gateway speaks JSON to external clients (React SPA, Python) but forwards
// to the gRPC services as protobuf messages. protobuf ships a canonical JSON
// mapping (proto3 JSON), so this is a thin, lossless wrapper around it rather
// than a hand-written serializer.
#ifndef FMGR_REST_JSONPROTOMAPPING_H
#define FMGR_REST_JSONPROTOMAPPING_H

#include <google/protobuf/message.h>

#include <stdexcept>
#include <string>

namespace fmgr::rest {

  // Thrown when an inbound JSON body cannot be parsed into the target message
  // (malformed JSON, unknown field, type mismatch). The gateway maps this to a
  // 400 Bad Request.
  class BadJson : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };

  // Parse a JSON request body into a protobuf message. Unknown fields are
  // rejected (fail-closed: a typo'd field name is a client error, not silently
  // dropped). Throws BadJson on any failure.
  void json_to_message(const std::string& json, google::protobuf::Message& out);

  // Serialize a protobuf message to a JSON response body. Uses proto field
  // names (snake_case) to stay consistent with the .proto source of truth and
  // the CSV column model.
  [[nodiscard]] std::string message_to_json(const google::protobuf::Message& msg);

} // namespace fmgr::rest

#endif // FMGR_REST_JSONPROTOMAPPING_H
