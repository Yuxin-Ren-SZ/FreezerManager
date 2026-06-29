// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SampleBrowserWidget.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QTableView>
#include <QVBoxLayout>

#include "qt/SampleSearchBar.h"
#include "qt/SampleServiceClient.h"
#include "qt/SampleTableModel.h"

namespace fmgr::qt {

SampleBrowserWidget::SampleBrowserWidget(SampleServiceClient* client,
                                         QWidget* parent)
    : QWidget(parent), client_(client) {
  model_ = new SampleTableModel(client, this);
  search_bar_ = new SampleSearchBar(this);

  auto* export_button = new QPushButton(QStringLiteral("Export CSV…"), this);
  export_button->setToolTip(
      QStringLiteral("Export every sample in the current lab (whole-lab; the "
                     "box/status filter does not apply)"));

  table_ = new QTableView(this);
  table_->setModel(model_);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->verticalHeader()->setVisible(false);

  auto* top = new QHBoxLayout;
  top->addWidget(search_bar_, /*stretch=*/1);
  top->addWidget(export_button);

  auto* layout = new QVBoxLayout(this);
  layout->addLayout(top);
  layout->addWidget(table_);

  // The search bar assembles the merged filter; feed it straight to the model.
  connect(search_bar_, &SampleSearchBar::filterChanged, model_,
          &SampleTableModel::setScope);
  connect(export_button, &QPushButton::clicked, this,
          &SampleBrowserWidget::exportCsv);
}

void SampleBrowserWidget::setToken(const QString& session_token) {
  token_ = session_token;
  model_->setToken(session_token);
}

void SampleBrowserWidget::setScope(const QString& lab_id,
                                   const std::optional<QString>& box_id) {
  lab_id_ = lab_id;
  SampleFilter base;
  base.lab_id = lab_id;
  base.box_id = box_id;
  // Emits filterChanged → model_->setScope, which loads the first page.
  search_bar_->setScope(base);
}

void SampleBrowserWidget::exportCsv() {
  if (lab_id_.isEmpty()) {
    return;
  }
  const auto result = client_->exportSamplesCsv(token_, lab_id_);
  if (!result.ok) {
    QMessageBox::warning(this, QStringLiteral("Export failed"),
                         QString::fromStdString(result.error));
    return;
  }
  const QString path = QFileDialog::getSaveFileName(
      this, QStringLiteral("Export samples CSV"), QStringLiteral("samples.csv"),
      QStringLiteral("CSV files (*.csv)"));
  if (path.isEmpty()) {
    return;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(result.csv.toUtf8()) < 0 || !file.commit()) {
    QMessageBox::warning(this, QStringLiteral("Export failed"),
                         QStringLiteral("Could not write %1").arg(path));
  }
}

}  // namespace fmgr::qt
