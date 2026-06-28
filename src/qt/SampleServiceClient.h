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

  explicit SampleServiceClient(
      std::unique_ptr<v1::SampleService::StubInterface> stub);

  // One page of samples matching the filter. page_token is the opaque cursor
  // from a previous call's next_page_token (empty for the first page).
  ListSamplesResult listSamples(const QString& session_token,
                                const SampleFilter& filter,
                                const QString& page_token = QString());

  GetSampleResult getSample(const QString& session_token,
                            const QString& sample_id);

 private:
  std::unique_ptr<v1::SampleService::StubInterface> stub_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_SAMPLESERVICECLIENT_H
