// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_BARCODESCANWIDGET_H
#define FMGR_QT_BARCODESCANWIDGET_H

#include <QWidget>

class QComboBox;
class QLineEdit;
class QListWidget;

namespace fmgr::qt {

class BarcodeScanController;

// Barcode-scanner focus mode for bulk check-in/out: an action combo over a
// focused input that auto-submits on Enter (HID wedge scanners send Enter), and a
// running log of outcomes. All work is delegated to BarcodeScanController; this is
// thin glue (manual e2e), the controller is what's unit-tested.
class BarcodeScanWidget : public QWidget {
  Q_OBJECT

 public:
  explicit BarcodeScanWidget(BarcodeScanController* controller,
                             QWidget* parent = nullptr);

 private slots:
  void submitScan();

 private:
  BarcodeScanController* controller_;
  QComboBox* action_combo_ = nullptr;
  QLineEdit* input_ = nullptr;
  QListWidget* log_ = nullptr;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_BARCODESCANWIDGET_H
