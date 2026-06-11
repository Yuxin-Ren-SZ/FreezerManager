// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_PAGES_SAMPLEBROWSERPAGE_H
#define FMGR_QT_PAGES_SAMPLEBROWSERPAGE_H

#include "core/sample.h"
#include "mock/MockData.h"
#include "models/SampleTableModel.h"

#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSplitter>
#include <QTableView>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

namespace fmgr::qt::pages {

using namespace fmgr::core;
using namespace fmgr::qt::mock;
using namespace fmgr::qt::models;

class SampleBrowserPage : public QWidget {
  Q_OBJECT

public:
  explicit SampleBrowserPage(QWidget* parent = nullptr) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(10);

    auto* title = new QLabel("Sample Browser");
    title->setStyleSheet("font-size:20px; font-weight:700; color:#e6edf3;");
    layout->addWidget(title);

    auto* sub = new QLabel("Search, filter and inspect biospecimen samples");
    sub->setStyleSheet("font-size:12px; color:#8b949e; margin-bottom:4px;");
    layout->addWidget(sub);

    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(10);

    searchBox_ = new QLineEdit();
    searchBox_->setPlaceholderText("Search by name, barcode, or position...");
    searchBox_->setClearButtonEnabled(true);
    searchBox_->setMinimumWidth(300);
    toolbar->addWidget(searchBox_);

    statusFilter_ = new QComboBox();
    statusFilter_->addItem("All Statuses", -1);
    statusFilter_->addItem("Active", static_cast<int>(SampleStatus::Active));
    statusFilter_->addItem("Checked Out", static_cast<int>(SampleStatus::CheckedOut));
    statusFilter_->addItem("Depleted", static_cast<int>(SampleStatus::Depleted));
    statusFilter_->addItem("Destroyed", static_cast<int>(SampleStatus::Destroyed));
    toolbar->addWidget(statusFilter_);

    toolbar->addStretch();

    countLabel_ = new QLabel();
    countLabel_->setStyleSheet("color:#8b949e; font-size:12px;");
    toolbar->addWidget(countLabel_);
    layout->addLayout(toolbar);

    auto* splitter = new QSplitter(Qt::Horizontal);

    tableView_ = new QTableView();
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableView_->setAlternatingRowColors(true);
    tableView_->setSortingEnabled(true);
    tableView_->verticalHeader()->hide();
    tableView_->horizontalHeader()->setStretchLastSection(true);
    tableView_->setShowGrid(false);
    tableView_->horizontalHeader()->setHighlightSections(false);

    model_ = new SampleTableModel(this);
    model_->setSamples(makeSamples());
    tableView_->setModel(model_);
    tableView_->setColumnHidden(SampleTableModel::ColType, true);
    tableView_->setColumnWidth(SampleTableModel::ColName, 200);
    tableView_->setColumnWidth(SampleTableModel::ColBarcode, 110);
    tableView_->setColumnWidth(SampleTableModel::ColBox, 100);
    tableView_->setColumnWidth(SampleTableModel::ColPosition, 80);
    tableView_->setColumnWidth(SampleTableModel::ColStatus, 110);
    tableView_->setColumnWidth(SampleTableModel::ColVolume, 90);
    splitter->addWidget(tableView_);

    detailPanel_ = new QTextEdit();
    detailPanel_->setReadOnly(true);
    detailPanel_->setPlaceholderText("Select a sample to view details");
    detailPanel_->setMinimumWidth(240);
    detailPanel_->setStyleSheet(
      "QTextEdit{background:#161b22;border:1px solid #30363d;"
      "border-radius:8px;padding:16px;color:#c9d1d9;}");
    splitter->addWidget(detailPanel_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter);

    connect(searchBox_, &QLineEdit::textChanged, this, &SampleBrowserPage::applyFilter);
    connect(statusFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SampleBrowserPage::applyFilter);
    connect(tableView_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &SampleBrowserPage::showDetail);
    updateCountLabel();
  }

private slots:
  void applyFilter() {
    auto allSamples = makeSamples();
    QString search = searchBox_->text().trimmed().toLower();
    int statusVal = statusFilter_->currentData().toInt();
    std::vector<Sample> filtered;
    for (const auto& s : allSamples) {
      if (statusVal >= 0 && static_cast<int>(s.status) != statusVal) continue;
      if (!search.isEmpty()) {
        auto nm = QString::fromStdString(s.name).toLower();
        auto bc = QString::fromStdString(s.barcode.value_or("")).toLower();
        auto ps = QString::fromStdString(s.position_label.value_or("")).toLower();
        if (!nm.contains(search) && !bc.contains(search) && !ps.contains(search)) continue;
      }
      filtered.push_back(std::move(s));
    }
    model_->setSamples(filtered);
    updateCountLabel();
  }

  void showDetail(const QModelIndex& current, const QModelIndex&) {
    if (!current.isValid()) { detailPanel_->clear(); return; }
    const auto& s = model_->samples().at(current.row());
    auto val = [&](int col) {
      return model_->data(current.sibling(current.row(), col)).toString();
    };
    int n = model_->rowCount();
    QString h = "<div style='font-size:18px;font-weight:700;color:#58a6ff;margin-bottom:12px'>"
              + QString::fromStdString(s.name) + "</div>"
              + "<table cellspacing='8' style='color:#c9d1d9'>";
    auto row = [&](const QString& k, const QString& v) {
      h += "<tr><td style='color:#8b949e;padding-right:16px'>" + k
         + "</td><td>" + v + "</td></tr>";
    };
    row("ID", QString::fromStdString(s.id.to_string()).left(18) + "...");
    row("Barcode", QString::fromStdString(s.barcode.value_or("-")));
    row("Status", val(SampleTableModel::ColStatus));
    if (s.box_id.has_value())
      row("Position", QString::fromStdString(s.position_label.value_or("-")));
    if (s.volume_value.has_value())
      row("Volume", QString::number(s.volume_value.value()) + " uL");
    h += "</table>";
    detailPanel_->setHtml(h);
  }

private:
  void updateCountLabel() {
    int n = model_->rowCount();
    countLabel_->setText(QString("%1 sample%2").arg(n).arg(n == 1 ? "" : "s"));
  }

  QLineEdit* searchBox_ = nullptr;
  QComboBox* statusFilter_ = nullptr;
  QLabel* countLabel_ = nullptr;
  QTableView* tableView_ = nullptr;
  QTextEdit* detailPanel_ = nullptr;
  SampleTableModel* model_ = nullptr;
};

}  // namespace fmgr::qt::pages

#endif
