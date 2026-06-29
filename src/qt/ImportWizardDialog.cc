// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/ImportWizardDialog.h"

#include <utility>

#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "qt/SampleServiceClient.h"

namespace fmgr::qt {

ImportWizardDialog::ImportWizardDialog(SampleServiceClient* client,
                                       QString session_token, QString lab_id,
                                       QWidget* parent)
    : QDialog(parent),
      client_(client),
      token_(std::move(session_token)),
      lab_id_(std::move(lab_id)) {
  setWindowTitle(QStringLiteral("Import samples from CSV"));
  resize(560, 420);

  choose_button_ = new QPushButton(QStringLiteral("Choose CSV…"), this);
  file_label_ = new QLabel(QStringLiteral("No file selected."), this);
  file_label_->setWordWrap(true);

  auto* top = new QHBoxLayout;
  top->addWidget(choose_button_);
  top->addWidget(file_label_, /*stretch=*/1);

  report_ = new QTableWidget(this);
  report_->setColumnCount(3);
  report_->setHorizontalHeaderLabels({QStringLiteral("Row"),
                                      QStringLiteral("Status"),
                                      QStringLiteral("Message")});
  report_->horizontalHeader()->setStretchLastSection(true);
  report_->verticalHeader()->setVisible(false);
  report_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  report_->setSelectionMode(QAbstractItemView::NoSelection);

  status_label_ = new QLabel(
      QStringLiteral("Choose a CSV file to validate before importing."), this);
  status_label_->setWordWrap(true);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  import_button_ = buttons->addButton(QStringLiteral("Import"),
                                      QDialogButtonBox::AcceptRole);
  import_button_->setEnabled(false);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto* layout = new QVBoxLayout(this);
  layout->addLayout(top);
  layout->addWidget(report_, /*stretch=*/1);
  layout->addWidget(status_label_);
  layout->addWidget(buttons);

  connect(choose_button_, &QPushButton::clicked, this,
          &ImportWizardDialog::chooseFileAndValidate);
  connect(import_button_, &QPushButton::clicked, this,
          &ImportWizardDialog::commitImport);
}

void ImportWizardDialog::setStatus(const QString& message, bool error) {
  status_label_->setText(message);
  status_label_->setStyleSheet(error ? QStringLiteral("color: #b00020;")
                                     : QString());
}

void ImportWizardDialog::chooseFileAndValidate() {
  const QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("Choose CSV to import"), QString(),
      QStringLiteral("CSV files (*.csv);;All files (*)"));
  if (path.isEmpty()) {
    return;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    setStatus(QStringLiteral("Could not read %1").arg(path), /*error=*/true);
    return;
  }
  csv_content_ = QString::fromUtf8(file.readAll());
  file_label_->setText(path);
  import_button_->setEnabled(false);
  runDryRun();
}

void ImportWizardDialog::runDryRun() {
  const auto result = client_->importSamples(token_, lab_id_, csv_content_,
                                             /*dry_run=*/true);
  report_->setRowCount(0);

  if (!result.ok) {
    setStatus(QStringLiteral("Validation request failed: %1")
                  .arg(QString::fromStdString(result.error)),
              /*error=*/true);
    return;
  }
  if (!result.header_error.isEmpty()) {
    setStatus(QStringLiteral("CSV is unusable: %1").arg(result.header_error),
              /*error=*/true);
    return;
  }

  report_->setRowCount(static_cast<int>(result.rows.size()));
  for (int i = 0; i < static_cast<int>(result.rows.size()); ++i) {
    const auto& row = result.rows[static_cast<std::size_t>(i)];
    report_->setItem(i, 0,
                     new QTableWidgetItem(QString::number(row.row_number)));
    report_->setItem(i, 1,
                     new QTableWidgetItem(row.ok ? QStringLiteral("OK")
                                                 : QStringLiteral("✗")));
    report_->setItem(i, 2, new QTableWidgetItem(row.error));
  }

  const bool importable = result.failed == 0;
  import_button_->setEnabled(importable);
  if (importable) {
    setStatus(QStringLiteral("%1 row(s) valid — ready to import.")
                  .arg(result.succeeded),
              /*error=*/false);
  } else {
    setStatus(QStringLiteral("%1 valid, %2 failed — fix the CSV before "
                             "importing (nothing is written).")
                  .arg(result.succeeded)
                  .arg(result.failed),
              /*error=*/true);
  }
}

void ImportWizardDialog::commitImport() {
  if (csv_content_.isEmpty()) {
    return;
  }
  const auto result = client_->importSamples(token_, lab_id_, csv_content_,
                                             /*dry_run=*/false);
  if (!result.ok) {
    QMessageBox::warning(this, QStringLiteral("Import failed"),
                         QString::fromStdString(result.error));
    return;
  }
  if (!result.committed) {
    // A row failed against committed state since the dry-run (e.g. a position
    // taken in the meantime); re-run the dry-run so the report reflects it.
    runDryRun();
    QMessageBox::warning(
        this, QStringLiteral("Import not committed"),
        QStringLiteral("The import did not commit; the report has been "
                       "refreshed. Nothing was written."));
    return;
  }
  QMessageBox::information(
      this, QStringLiteral("Import complete"),
      QStringLiteral("Imported %1 sample(s).").arg(result.succeeded));
  accept();
}

}  // namespace fmgr::qt
