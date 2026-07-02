// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SampleBrowserWidget.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include "qt/ImportWizardDialog.h"
#include "qt/SampleSearchBar.h"
#include "qt/SampleServiceClient.h"
#include "qt/SampleTableModel.h"
#include "qt/SampleWatchSubscriber.h"

namespace fmgr::qt {
namespace {

// Coalesce a burst of live updates (e.g. a bulk import streams many rows) into
// one reload. A change resets the timer; the reload fires once it goes quiet.
constexpr int kRefreshDebounceMs = 400;

}  // namespace

SampleBrowserWidget::SampleBrowserWidget(SampleServiceClient* client,
                                         QWidget* parent)
    : QWidget(parent), client_(client) {
  model_ = new SampleTableModel(client, this);
  search_bar_ = new SampleSearchBar(this);

  auto* import_button = new QPushButton(QStringLiteral("Import CSV…"), this);
  import_button->setToolTip(
      QStringLiteral("Import samples from a CSV file (dry-run validation "
                     "report, then all-or-nothing commit)"));

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
  top->addWidget(import_button);
  top->addWidget(export_button);

  auto* layout = new QVBoxLayout(this);
  layout->addLayout(top);
  layout->addWidget(table_);

  // Debounce timer for live updates: a single-shot timer restarted on every
  // change; its timeout reloads the current scope in place.
  refresh_timer_ = new QTimer(this);
  refresh_timer_->setSingleShot(true);
  refresh_timer_->setInterval(kRefreshDebounceMs);

  // The search bar assembles the merged filter; feed it straight to the model.
  connect(search_bar_, &SampleSearchBar::filterChanged, model_,
          &SampleTableModel::setScope);
  connect(export_button, &QPushButton::clicked, this,
          &SampleBrowserWidget::exportCsv);
  connect(import_button, &QPushButton::clicked, this,
          &SampleBrowserWidget::importCsv);
  connect(refresh_timer_, &QTimer::timeout, model_, &SampleTableModel::reload);
}

void SampleBrowserWidget::setWatchSubscriber(SampleWatchSubscriber* watch) {
  watch_ = watch;
  if (watch_ != nullptr) {
    // A live change schedules a debounced reload (coalesces bursts).
    connect(watch_, &SampleWatchSubscriber::changed, refresh_timer_,
            qOverload<>(&QTimer::start));
  }
}

void SampleBrowserWidget::setToken(const QString& session_token) {
  token_ = session_token;
  model_->setToken(session_token);
}

void SampleBrowserWidget::setScope(const QString& lab_id,
                                   const std::optional<QString>& box_id) {
  lab_id_ = lab_id;
  box_id_ = box_id;
  SampleFilter base;
  base.lab_id = lab_id;
  base.box_id = box_id;
  // Emits filterChanged → model_->setScope, which loads the first page.
  search_bar_->setScope(base);
  // (Re)subscribe the live feed to the new scope. The status/barcode filters
  // from the search bar are not forwarded — the feed tails by lab/box and the
  // debounced reload re-applies the active filter on the model.
  if (watch_ != nullptr) {
    watch_->start(base);
  }
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
  if (!file.open(QIODevice::WriteOnly) || file.write(result.csv.toUtf8()) < 0 ||
      !file.commit()) {
    QMessageBox::warning(this, QStringLiteral("Export failed"),
                         QStringLiteral("Could not write %1").arg(path));
  }
}

void SampleBrowserWidget::importCsv() {
  if (lab_id_.isEmpty()) {
    return;
  }
  ImportWizardDialog dialog(client_, token_, lab_id_, this);
  if (dialog.exec() == QDialog::Accepted) {
    // A committed import added/changed rows; reload the current scope.
    setScope(lab_id_, box_id_);
  }
}

}  // namespace fmgr::qt
