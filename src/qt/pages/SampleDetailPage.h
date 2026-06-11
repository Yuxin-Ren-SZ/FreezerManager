// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_PAGES_SAMPLEDETAILPAGE_H
#define FMGR_QT_PAGES_SAMPLEDETAILPAGE_H

#include "core/sample.h"
#include "mock/MockData.h"

#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace fmgr::qt::pages {

using namespace fmgr::core;
using namespace fmgr::qt::mock;

class SampleDetailPage : public QWidget {
  Q_OBJECT

public:
  explicit SampleDetailPage(QWidget* parent = nullptr) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea{background:transparent;border:none;}");

    content_ = new QWidget();
    layout_ = new QVBoxLayout(content_);
    layout_->setContentsMargins(28, 24, 28, 24);
    layout_->setSpacing(16);
    layout_->addStretch();

    // Default: show instructions
    auto* placeholder = new QLabel("Select a sample to view details");
    placeholder->setStyleSheet("font-size:15px; color:#8b949e;");
    placeholder->setAlignment(Qt::AlignCenter);
    layout_->insertWidget(0, placeholder);
    placeholder_ = placeholder;

    scroll->setWidget(content_);
    outer->addWidget(scroll);
  }

  void showSample(const Sample& sample) {
    // Clear previous content except stretch
    while (layout_->count() > 1) {
      auto* item = layout_->takeAt(0);
      if (item->widget()) item->widget()->deleteLater();
      delete item;
    }
    placeholder_ = nullptr;

    auto addWidget = [&](const QString& key, const QString& val,
                         const QString& valColor = "#e6edf3") {
      auto* row = new QHBoxLayout();
      auto* k = new QLabel(key);
      k->setStyleSheet("font-size:13px; color:#8b949e; background:transparent; min-width:130px;");
      k->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      row->addWidget(k);
      auto* v = new QLabel(val);
      v->setStyleSheet(QString("font-size:14px; color:%1; background:transparent; font-weight:500;").arg(valColor));
      v->setWordWrap(true);
      row->addWidget(v, 1);
      auto* w = new QWidget();
      w->setStyleSheet("background:transparent;");
      w->setLayout(row);
      layout_->insertWidget(layout_->count() - 1, w);
    };

    // Title
    auto* title = new QLabel(QString::fromStdString(sample.name));
    title->setStyleSheet("font-size:22px; font-weight:800; color:#58a6ff;");
    layout_->insertWidget(layout_->count() - 1, title);

    // Status badge
    QString statusText;
    QString statusColor;
    switch (sample.status) {
      case SampleStatus::Active:     statusText="Active"; statusColor="#3fb950"; break;
      case SampleStatus::CheckedOut: statusText="Checked Out"; statusColor="#d2991d"; break;
      case SampleStatus::Depleted:   statusText="Depleted"; statusColor="#8b949e"; break;
      case SampleStatus::Destroyed:  statusText="Destroyed"; statusColor="#da3633"; break;
      case SampleStatus::Tombstoned: statusText="Tombstoned"; statusColor="#484f58"; break;
    }
    auto* badge = new QLabel(QString("  %1  ").arg(statusText));
    badge->setStyleSheet(QString(
      "font-size:12px; font-weight:600; color:#fff; background:%1; border-radius:4px; padding:2px 8px;").arg(statusColor));
    badge->setFixedWidth(badge->sizeHint().width());
    layout_->insertWidget(layout_->count() - 1, badge);

    // Details
    addWidget("Sample ID", QString::fromStdString(sample.id.to_string()).left(24) + "...", "#8b949e");
    addWidget("Barcode", QString::fromStdString(sample.barcode.value_or("N/A")));
    addWidget("Item Type", "Biospecimen");
    if (sample.box_id.has_value())
      addWidget("Position", QString::fromStdString(sample.position_label.value_or("—")));
    if (sample.volume_value.has_value())
      addWidget("Volume", QString::number(sample.volume_value.value()) + " \u00B5L");
    addWidget("Created", "2024-01-15 09:30");
    addWidget("Modified", "2024-06-08 14:22");

    if (sample.parent_sample_id.has_value())
      addWidget("Parent Sample", QString::fromStdString(sample.parent_sample_id->to_string()).left(24) + "...", "#58a6ff");

    // Separator
    auto* sep = new QFrame();
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("background:#30363d; max-height:1px;");
    layout_->insertWidget(layout_->count() - 1, sep);

    // Audit trail header
    auto* audHeader = new QLabel("Audit Trail");
    audHeader->setStyleSheet("font-size:16px; font-weight:700; color:#e6edf3; margin-top:4px;");
    layout_->insertWidget(layout_->count() - 1, audHeader);

    // Show relevant audit events
    auto events = makeAuditEvents();
    std::string sid = sample.id.to_string();
    bool foundAny = false;

    for (auto& e : events) {
      if (e.sampleId != sid) continue;
      foundAny = true;

      QString icon, color;
      if (e.action == CheckoutAction::CheckedOut) { icon = "\xF0\x9F\x94\xB5"; color = "#d2991d"; }
      else if (e.action == CheckoutAction::CheckedIn) { icon = "\xF0\x9F\x9F\xA2"; color = "#3fb950"; }
      else { icon = "\xF0\x9F\x94\xB4"; color = "#da3633"; }

      QString actStr = e.action == CheckoutAction::CheckedOut ? "Checked out"
                     : e.action == CheckoutAction::CheckedIn  ? "Returned"
                     : "Destroyed";

      int hours = int(e.microsAgo / 3600000000LL);
      QString timeStr = hours > 24 ? QString("%1 days ago").arg(hours / 24)
                      : hours > 1  ? QString("%1 hours ago").arg(hours)
                      : "Recently";

      auto* evtRow = new QHBoxLayout();
      auto* dotIcon = new QLabel(icon);
      dotIcon->setStyleSheet("font-size:12px; background:transparent;");
      evtRow->addWidget(dotIcon);

      auto* evtText = new QLabel(QString("<b style='color:%1'>%2</b> "
                                          "<span style='color:#8b949e'>%3</span> — "
                                          "<span style='color:#c9d1d9'>%4</span>")
                                    .arg(color, QString::fromStdString(e.userName),
                                         actStr, QString::fromStdString(e.reason)));
      evtText->setStyleSheet("font-size:12px; background:transparent;");
      evtRow->addWidget(evtText, 1);

      auto* timeLbl = new QLabel(timeStr);
      timeLbl->setStyleSheet("font-size:11px; color:#484f58; background:transparent;");
      evtRow->addWidget(timeLbl);

      auto* ew = new QWidget();
      ew->setStyleSheet("background:transparent;");
      ew->setLayout(evtRow);
      layout_->insertWidget(layout_->count() - 1, ew);
    }

    if (!foundAny) {
      auto* noEvt = new QLabel("No audit events recorded");
      noEvt->setStyleSheet("font-size:12px; color:#484f58;");
      layout_->insertWidget(layout_->count() - 1, noEvt);
    }
    layout_->addStretch();
  }

private:
  QWidget* content_ = nullptr;
  QVBoxLayout* layout_ = nullptr;
  QLabel* placeholder_ = nullptr;
};

}  // namespace fmgr::qt::pages

#endif
