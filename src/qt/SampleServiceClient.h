// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_SAMPLESERVICECLIENT_H
#define FMGR_QT_SAMPLESERVICECLIENT_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QString>

#include "fmgr/v1/sample.grpc.pb.h"

namespace fmgr::qt {

// Structured filter for ListSamples. Mirrors the proto request's filter fields
// (sample.proto). lab_id is required; the rest narrow the result. Full-text and
// custom-field filtering are not represented because ListSamples does not yet
// support them server-side.
struct SampleFilter {
  QString lab_id;
  std::optional<QString> box_id;
  std::optional<QString> item_type_id;
  std::optional<QString> barcode;
  std::optional<v1::SampleStatus> status;
  bool include_archived = false;
};

// Synchronous, UI-agnostic wrapper over SampleService's read RPCs (ListSamples,
// GetSample). Mirrors LabServiceClient/BoxServiceClient: owns a stub interface,
// never throws, attaches the bearer on every call, and returns flat row structs.
class SampleServiceClient {
 public:
  struct SampleRow {
    QString id;
    QString name;
    QString barcode;
    v1::SampleStatus status = v1::SAMPLE_STATUS_UNSPECIFIED;
    QString box_id;
    QString position_label;
    std::optional<double> volume_value;
    QString volume_unit;
    QString item_type_id;
  };

  struct ListSamplesResult {
    bool ok = false;
    std::string error;
    std::vector<SampleRow> samples;
    QString next_page_token;  // empty when there are no more pages
  };

  struct GetSampleResult {
    bool ok = false;
    std::string error;
    SampleRow sample;
  };

  struct ExportCsvResult {
    bool ok = false;
    std::string error;
    QString csv;  // RFC 4180 CSV with chain-of-custody header
  };

  // One row of an import validation/commit report (mirrors
  // ImportSampleRowResult).
  struct ImportRowResult {
    int row_number = 0;  // 1-based, excludes the header
    bool ok = false;
    QString error;      // empty when ok
    QString sample_id;  // set only for a committed (non-dry-run) row
  };

  struct ImportResult {
    bool ok = false;    // gRPC transport status
    std::string error;  // transport / status error message
    bool committed =
        false;             // true only when a non-dry-run import wrote all rows
    QString header_error;  // non-empty when the CSV is structurally unusable
    int succeeded = 0;
    int failed = 0;
    std::vector<ImportRowResult> rows;
  };

  explicit SampleServiceClient(
      std::unique_ptr<v1::SampleService::StubInterface> stub);

  // One page of samples matching the filter. page_token is the opaque cursor
  // from a previous call's next_page_token (empty for the first page).
  ListSamplesResult listSamples(const QString& session_token,
                                const SampleFilter& filter,
                                const QString& page_token = QString());

  GetSampleResult getSample(const QString& session_token,
                            const QString& sample_id);

  // Place/relocate a sample into a box position. The server validates size-class
  // compatibility and atomicity; a rejected move comes back as {ok=false, error}.
  GetSampleResult moveSample(const QString& session_token,
                             const QString& sample_id,
                             const QString& dest_box_id,
                             const QString& dest_position);

  // Check a sample out / in / discard. Optional volume_used (+ unit) records a
  // draw; optional reason is audited. Returns the updated sample.
  GetSampleResult checkoutSample(
      const QString& session_token, const QString& sample_id,
      v1::CheckoutAction action,
      std::optional<double> volume_used = std::nullopt,
      const QString& volume_unit = QString(),
      const QString& reason = QString());

  // Export every sample in a lab as RFC 4180 CSV (lab-scoped; no per-box filter).
  ExportCsvResult exportSamplesCsv(const QString& session_token,
                                   const QString& lab_id,
                                   bool include_archived = false);

  // Import samples from RFC 4180 CSV (same schema as export). dry_run validates
  // and returns a per-row report without persisting; a non-dry-run import is
  // all-or-nothing (committed=true only if every row wrote).
  ImportResult importSamples(const QString& session_token,
                             const QString& lab_id, const QString& csv_content,
                             bool dry_run);

 private:
  std::unique_ptr<v1::SampleService::StubInterface> stub_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_SAMPLESERVICECLIENT_H
