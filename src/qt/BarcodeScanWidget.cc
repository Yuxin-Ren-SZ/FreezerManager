// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/BarcodeScanWidget.h"

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

#include "fmgr/v1/sample.grpc.pb.h"
#include "qt/BarcodeScanController.h"

namespace fmgr::qt {

BarcodeScanWidget::BarcodeScanWidget(BarcodeScanController* controller,
                                     QWidget* parent)
    : QWidget(parent), controller_(controller) {
  auto* form = new QFormLayout;

  action_combo_ = new QComboBox(this);
  action_combo_->addItem(QStringLiteral("Check out"),
                         static_cast<int>(v1::CHECKOUT_ACTION_CHECKOUT));
  action_combo_->addItem(QStringLiteral("Check in"),
                         static_cast<int>(v1::CHECKOUT_ACTION_CHECKIN));
  action_combo_->addItem(QStringLiteral("Discard"),
                         static_cast<int>(v1::CHECKOUT_ACTION_DISCARD));
  form->addRow(QStringLiteral("Action"), action_combo_);

  input_ = new QLineEdit(this);
  input_->setPlaceholderText(QStringLiteral("Scan or type a barcode, then Enter"));
  form->addRow(QStringLiteral("Barcode"), input_);

  log_ = new QListWidget(this);

  auto* layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(log_);

  connect(action_combo_, &QComboBox::currentIndexChanged, this, [this]() {
    controller_->setAction(
        static_cast<v1::CheckoutAction>(action_combo_->currentData().toInt()));
  });
  connect(input_, &QLineEdit::returnPressed, this,
          &BarcodeScanWidget::submitScan);
  connect(controller_, &BarcodeScanController::scanned, this,
          [this](const BarcodeScanController::ScanResult& result) {
            auto* item = new QListWidgetItem(result.message, log_);
            item->setForeground(QBrush(result.ok ? QColor(0x0a, 0x7d, 0x33)
                                                  : QColor(0xb0, 0x00, 0x20)));
            log_->addItem(item);
            log_->scrollToBottom();
          });

  // Default action matches the combo's first entry.
  controller_->setAction(v1::CHECKOUT_ACTION_CHECKOUT);
}

void BarcodeScanWidget::submitScan() {
  const QString barcode = input_->text();
  input_->clear();
  input_->setFocus();
  if (!barcode.trimmed().isEmpty()) {
    controller_->processScan(barcode);
  }
}

}  // namespace fmgr::qt
