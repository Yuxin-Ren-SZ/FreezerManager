// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_BARCODESCANCONTROLLER_H
#define FMGR_QT_BARCODESCANCONTROLLER_H

#include <QObject>
#include <QString>

#include "fmgr/v1/sample.grpc.pb.h"

namespace fmgr::qt {

class SampleServiceClient;

// Resolves a scanned barcode to a sample (ListSamples with a barcode filter) and
// applies the current check-out/in/discard action to it (CheckoutSample). All
// logic lives here (a QObject, not a widget) so the scan→resolve→checkout path is
// unit-testable headlessly; BarcodeScanWidget is thin glue over processScan().
class BarcodeScanController : public QObject {
  Q_OBJECT

 public:
  struct ScanResult {
    bool ok = false;
    QString message;    // human-readable outcome for the scan log
    QString sample_id;  // resolved sample, when found
  };

  explicit BarcodeScanController(SampleServiceClient* client,
                                 QObject* parent = nullptr);

  void setToken(const QString& session_token) { token_ = session_token; }
  void setScope(const QString& lab_id) { lab_id_ = lab_id; }
  void setAction(v1::CheckoutAction action) { action_ = action; }
  v1::CheckoutAction action() const { return action_; }

  // Resolve the barcode within the current lab and apply the current action.
  // Synchronous; also emits scanned() so the widget can append to its log.
  ScanResult processScan(const QString& barcode);

 signals:
  void scanned(const ScanResult& result);

 private:
  SampleServiceClient* client_;
  QString token_;
  QString lab_id_;
  v1::CheckoutAction action_ = v1::CHECKOUT_ACTION_CHECKOUT;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_BARCODESCANCONTROLLER_H
