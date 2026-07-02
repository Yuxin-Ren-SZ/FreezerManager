// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SampleSearchBar.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVariant>

namespace fmgr::qt {

SampleSearchBar::SampleSearchBar(QWidget* parent) : QWidget(parent) {
  auto* layout = new QHBoxLayout(this);

  layout->addWidget(new QLabel(QStringLiteral("Status"), this));
  status_combo_ = new QComboBox(this);
  // Index 0 is "All" (no status filter); the rest carry a SampleStatus in their
  // item data.
  status_combo_->addItem(QStringLiteral("All"), QVariant());
  status_combo_->addItem(QStringLiteral("Active"),
                         static_cast<int>(v1::SAMPLE_STATUS_ACTIVE));
  status_combo_->addItem(QStringLiteral("Checked out"),
                         static_cast<int>(v1::SAMPLE_STATUS_CHECKED_OUT));
  status_combo_->addItem(QStringLiteral("Depleted"),
                         static_cast<int>(v1::SAMPLE_STATUS_DEPLETED));
  status_combo_->addItem(QStringLiteral("Destroyed"),
                         static_cast<int>(v1::SAMPLE_STATUS_DESTROYED));
  layout->addWidget(status_combo_);

  layout->addWidget(new QLabel(QStringLiteral("Barcode"), this));
  barcode_edit_ = new QLineEdit(this);
  barcode_edit_->setPlaceholderText(QStringLiteral("exact barcode"));
  layout->addWidget(barcode_edit_);

  layout->addStretch();

  connect(status_combo_, &QComboBox::currentIndexChanged, this,
          &SampleSearchBar::emitChange);
  connect(barcode_edit_, &QLineEdit::editingFinished, this,
          &SampleSearchBar::emitChange);
}

void SampleSearchBar::setScope(const SampleFilter& base) {
  base_ = base;
  emitChange();
}

SampleFilter SampleSearchBar::currentFilter() const {
  SampleFilter filter = base_;

  const QVariant status_data = status_combo_->currentData();
  if (status_data.isValid()) {
    filter.status = static_cast<v1::SampleStatus>(status_data.toInt());
  } else {
    filter.status.reset();
  }

  const QString barcode = barcode_edit_->text().trimmed();
  if (barcode.isEmpty()) {
    filter.barcode.reset();
  } else {
    filter.barcode = barcode;
  }
  return filter;
}

void SampleSearchBar::emitChange() { emit filterChanged(currentFilter()); }

}  // namespace fmgr::qt
