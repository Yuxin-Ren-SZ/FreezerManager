// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/BarcodeScanController.h"

#include "qt/SampleServiceClient.h"

namespace fmgr::qt {
namespace {

QString actionVerb(v1::CheckoutAction action) {
  switch (action) {
    case v1::CHECKOUT_ACTION_CHECKOUT:
      return QStringLiteral("checked out");
    case v1::CHECKOUT_ACTION_CHECKIN:
      return QStringLiteral("checked in");
    case v1::CHECKOUT_ACTION_DISCARD:
      return QStringLiteral("discarded");
    default:
      return QStringLiteral("updated");
  }
}

}  // namespace

BarcodeScanController::BarcodeScanController(SampleServiceClient* client,
                                             QObject* parent)
    : QObject(parent), client_(client) {}

BarcodeScanController::ScanResult BarcodeScanController::processScan(
    const QString& barcode) {
  const QString code = barcode.trimmed();
  ScanResult result;
  if (code.isEmpty()) {
    result.message = QStringLiteral("empty barcode");
    emit scanned(result);
    return result;
  }

  SampleFilter filter;
  filter.lab_id = lab_id_;
  filter.barcode = code;
  const auto found = client_->listSamples(token_, filter);
  if (!found.ok) {
    result.message = QStringLiteral("lookup failed: %1")
                         .arg(QString::fromStdString(found.error));
    emit scanned(result);
    return result;
  }
  if (found.samples.empty()) {
    result.message = QStringLiteral("no sample with barcode %1").arg(code);
    emit scanned(result);
    return result;
  }

  const auto& sample = found.samples.front();
  result.sample_id = sample.id;
  const auto outcome = client_->checkoutSample(token_, sample.id, action_);
  if (!outcome.ok) {
    result.message = QStringLiteral("%1: %2").arg(
        code, QString::fromStdString(outcome.error));
    emit scanned(result);
    return result;
  }

  result.ok = true;
  result.message =
      QStringLiteral("%1 %2 (%3)").arg(sample.name, actionVerb(action_), code);
  emit scanned(result);
  return result;
}

}  // namespace fmgr::qt
