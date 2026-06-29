// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_IMPORTWIZARDDIALOG_H
#define FMGR_QT_IMPORTWIZARDDIALOG_H

#include <QDialog>
#include <QString>

class QLabel;
class QPushButton;
class QTableWidget;

namespace fmgr::qt {

class SampleServiceClient;

// Modal CSV-import flow for the current lab. The user picks an RFC 4180 CSV
// (same schema as export), the dialog runs a server dry-run and shows a per-row
// validation report, and the Import button — enabled only when every row
// validates — commits the import all-or-nothing. accept()s on a committed
// import so the caller can refresh the sample list.
class ImportWizardDialog : public QDialog {
  Q_OBJECT

 public:
  ImportWizardDialog(SampleServiceClient* client, QString session_token,
                     QString lab_id, QWidget* parent = nullptr);

 private slots:
  // Pick a CSV file, load it, and run the dry-run validation pass.
  void chooseFileAndValidate();
  // Commit the loaded CSV (non-dry-run, all-or-nothing).
  void commitImport();

 private:
  void runDryRun();
  void setStatus(const QString& message, bool error);

  SampleServiceClient* client_;
  QString token_;
  QString lab_id_;
  QString csv_content_;  // the loaded file, replayed verbatim on commit

  QPushButton* choose_button_ = nullptr;
  QPushButton* import_button_ = nullptr;
  QLabel* file_label_ = nullptr;
  QLabel* status_label_ = nullptr;
  QTableWidget* report_ = nullptr;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_IMPORTWIZARDDIALOG_H
