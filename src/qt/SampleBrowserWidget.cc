// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SampleBrowserWidget.h"

#include <QHeaderView>
#include <QTableView>
#include <QVBoxLayout>

#include "qt/SampleSearchBar.h"
#include "qt/SampleServiceClient.h"
#include "qt/SampleTableModel.h"

namespace fmgr::qt {

SampleBrowserWidget::SampleBrowserWidget(SampleServiceClient* client,
                                         QWidget* parent)
    : QWidget(parent) {
  model_ = new SampleTableModel(client, this);
  search_bar_ = new SampleSearchBar(this);

  table_ = new QTableView(this);
  table_->setModel(model_);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->verticalHeader()->setVisible(false);

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(search_bar_);
  layout->addWidget(table_);

  // The search bar assembles the merged filter; feed it straight to the model.
  connect(search_bar_, &SampleSearchBar::filterChanged, model_,
          &SampleTableModel::setScope);
}

void SampleBrowserWidget::setToken(const QString& session_token) {
  model_->setToken(session_token);
}

void SampleBrowserWidget::setScope(const QString& lab_id,
                                   const std::optional<QString>& box_id) {
  SampleFilter base;
  base.lab_id = lab_id;
  base.box_id = box_id;
  // Emits filterChanged → model_->setScope, which loads the first page.
  search_bar_->setScope(base);
}

}  // namespace fmgr::qt
