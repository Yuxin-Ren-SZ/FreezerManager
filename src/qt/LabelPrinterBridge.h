// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_LABELPRINTERBRIDGE_H
#define FMGR_QT_LABELPRINTERBRIDGE_H

#include <utility>
#include <vector>

#include <QByteArray>
#include <QString>

#include "qt/BoxMapPdf.h"
#include "qt/LabelPdf.h"

namespace fmgr::qt {

// PRD §12 abstract printer contract. v1 emits PDF byte streams (saved or sent
// to the OS print dialog); a v2 implementation can bridge the same calls to a
// physical label printer (e.g. ZPL/EPL over USB) without touching callers.
class ILabelPrinter {
 public:
  virtual ~ILabelPrinter() = default;

  // Render a box's map. Returns the PDF bytes (empty on failure).
  virtual QByteArray printBoxMap(const QString& box_id,
                                 const QString& lab_id) = 0;

  // Render labels for the given samples. Returns the PDF bytes.
  virtual QByteArray printLabels(const std::vector<QString>& sample_ids) = 0;
};

// Default v1 bridge: fulfils ILabelPrinter via the BoxMapPdf / LabelPdf
// renderers. Holds the session token so callers pass only ids. Borrows the
// renderers; it does not own them.
class LabelPrinterBridge : public ILabelPrinter {
 public:
  LabelPrinterBridge(BoxMapPdf* box_map, LabelPdf* labels,
                     QString session_token)
      : box_map_(box_map), labels_(labels), token_(std::move(session_token)) {}

  QByteArray printBoxMap(const QString& box_id,
                         const QString& lab_id) override {
    return box_map_->generate(box_id, lab_id, token_);
  }

  QByteArray printLabels(const std::vector<QString>& sample_ids) override {
    return labels_->generate(sample_ids, token_);
  }

 private:
  BoxMapPdf* box_map_;
  LabelPdf* labels_;
  QString token_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_LABELPRINTERBRIDGE_H
