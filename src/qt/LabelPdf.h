// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_LABELPDF_H
#define FMGR_QT_LABELPDF_H

#include <vector>

#include <QByteArray>
#include <QString>

namespace fmgr::qt {

class SampleServiceClient;

// Renders printable sample labels — name, barcode and compact location — laid
// out on a standard 2×4 Avery-compatible grid (8 labels/page). Pure data → PDF,
// no widget dependency, so it is headless-testable. The paintable layout is
// exposed via buildModel() so tests can assert label content without parsing
// the (flate-compressed) PDF content stream.
class LabelPdf {
 public:
  struct Label {
    QString name;
    QString barcode;   // human-readable barcode text
    QString location;  // "<box> · <position>"
  };

  struct Model {
    std::vector<Label> labels;
  };

  explicit LabelPdf(SampleServiceClient* samples);

  // Resolve each sample id into a printable label (skips ids that fail to load).
  Model buildModel(const std::vector<QString>& sample_ids,
                   const QString& session_token);

  // Resolve + paint the labels to a self-contained PDF byte stream. An empty id
  // list still yields a valid (single blank page) PDF.
  QByteArray generate(const std::vector<QString>& sample_ids,
                      const QString& session_token);

 private:
  SampleServiceClient* samples_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_LABELPDF_H
