// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_SAMPLESEARCHBAR_H
#define FMGR_QT_SAMPLESEARCHBAR_H

#include <QWidget>

#include "qt/SampleServiceClient.h"

class QComboBox;
class QLineEdit;

namespace fmgr::qt {

// Filter controls above the sample table: a status combo and a barcode field.
// The lab/box scope comes from the tree (setScope); this widget layers the
// status + barcode refinements on top and emits the merged filter whenever any
// control changes. Thin glue — no business logic beyond assembling SampleFilter.
class SampleSearchBar : public QWidget {
  Q_OBJECT

 public:
  explicit SampleSearchBar(QWidget* parent = nullptr);

  // Set the tree-supplied scope (lab_id + optional box_id). Resets the controls'
  // contribution and emits filterChanged with the new base.
  void setScope(const SampleFilter& base);

  SampleFilter currentFilter() const;

 signals:
  void filterChanged(const SampleFilter& filter);

 private slots:
  void emitChange();

 private:
  SampleFilter base_;
  QComboBox* status_combo_ = nullptr;
  QLineEdit* barcode_edit_ = nullptr;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_SAMPLESEARCHBAR_H
