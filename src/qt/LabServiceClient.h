// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_LABSERVICECLIENT_H
#define FMGR_QT_LABSERVICECLIENT_H

#include <memory>
#include <string>
#include <vector>

#include <QString>

#include "fmgr/v1/lab.grpc.pb.h"

namespace fmgr::qt {

// Synchronous, UI-agnostic wrapper over LabService's read RPCs (ListLabs,
// GetLab). Mirrors AuthServiceClient: owns a stub interface (real channel in
// production, in-process fake in tests), never throws, and carries the bearer
// token as "authorization: Bearer <token>" metadata on every call.
class LabServiceClient {
 public:
  struct LabRow {
    QString id;
    QString name;
    QString contact;
    bool is_phi_enabled = false;
  };

  struct ListLabsResult {
    bool ok = false;
    std::string error;  // human-readable gRPC status message when !ok
    std::vector<LabRow> labs;
  };

  struct GetLabResult {
    bool ok = false;
    std::string error;
    LabRow lab;
  };

  explicit LabServiceClient(std::unique_ptr<v1::LabService::StubInterface> stub);

  // List the labs visible to the authenticated caller.
  ListLabsResult listLabs(const QString& session_token);

  // Fetch a single lab by id.
  GetLabResult getLab(const QString& session_token, const QString& lab_id);

 private:
  std::unique_ptr<v1::LabService::StubInterface> stub_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_LABSERVICECLIENT_H
