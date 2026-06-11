// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_PAGES_DASHBOARDPAGE_H
#define FMGR_QT_PAGES_DASHBOARDPAGE_H

#include "core/sample.h"
#include "mock/MockData.h"

#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>
#include <vector>

namespace fmgr::qt::pages {

using namespace fmgr::core;
using namespace fmgr::qt::mock;

// ── Mini bar chart widget for freezer fill rates ───────────────────
class MiniFillBar : public QWidget {
public:
  explicit MiniFillBar(const QString& label, double fillPct, QWidget* parent = nullptr)
      : QWidget(parent), label_(label), fillPct_(fillPct) {
    setFixedHeight(32);
    setMinimumWidth(120);
  }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Background
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(22, 27, 34));
    p.drawRoundedRect(rect(), 4, 4);

    // Fill
    int fillW = static_cast<int>(width() * fillPct_ / 100.0);
    if (fillW > 0) {
      QColor c = fillPct_ > 80 ? QColor(218, 54, 51)   // red
               : fillPct_ > 60 ? QColor(210, 153, 29)   // amber
               : QColor(46, 160, 67);                    // green
      p.setBrush(c);
      p.drawRoundedRect(0, 0, fillW, height(), 4, 4);
    }

    // Label + percentage
    p.setPen(QColor(201, 209, 217));
    QFont f = p.font();
    f.setPixelSize(12);
    p.setFont(f);
    QString txt = QString("%1  %2%").arg(label_, QString::number(fillPct_, 'f', 0));
    p.drawText(rect().adjusted(8, 0, -8, 0), Qt::AlignVCenter, txt);
  }
private:
  QString label_;
  double fillPct_;
};

// ── Stat card ──────────────────────────────────────────────────────
class StatCard : public QFrame {
public:
  StatCard(const QString& icon, const QString& value, const QString& label,
           const QString& accentColor, QWidget* parent = nullptr)
      : QFrame(parent) {
    setObjectName("card");
    setFixedSize(180, 110);
    setStyleSheet(QString(
      "QFrame#card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:12px;}"
      "QFrame#card:hover{border-color:%1;}").arg(accentColor));

    auto* l = new QVBoxLayout(this);
    l->setSpacing(4);
    l->setContentsMargins(14, 12, 14, 12);

    auto* ic = new QLabel(icon);
    ic->setStyleSheet("font-size:22px; background:transparent;");
    l->addWidget(ic);

    auto* v = new QLabel(value);
    v->setStyleSheet(QString("font-size:26px; font-weight:800; color:%1; background:transparent;").arg(accentColor));
    l->addWidget(v);

    auto* lb = new QLabel(label);
    lb->setStyleSheet("font-size:11px; color:#8b949e; background:transparent;");
    l->addWidget(lb);
  }
};

// ── Activity item ──────────────────────────────────────────────────
struct ActivityItem {
  QString icon;
  QString user;
  QString action;
  QString detail;
  QString time;
};

// ── Dashboard page ─────────────────────────────────────────────────
class DashboardPage : public QWidget {
  Q_OBJECT

public:
  explicit DashboardPage(QWidget* parent = nullptr) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea{background:transparent; border:none;}");

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(20);

    // ── Header ──
    auto* title = new QLabel("Dashboard");
    title->setStyleSheet("font-size:24px; font-weight:800; color:#e6edf3;");
    layout->addWidget(title);

    auto* sub = new QLabel("Overview of your lab's freezer inventory and recent activity");
    sub->setStyleSheet("font-size:13px; color:#8b949e; margin-bottom:4px;");
    layout->addWidget(sub);

    // ── Quick search ──
    auto* searchBox = new QLineEdit();
    searchBox->setPlaceholderText("Quick search — sample name, barcode, freezer, position...");
    searchBox->setClearButtonEnabled(true);
    searchBox->setMinimumHeight(42);
    searchBox->setStyleSheet(
      "QLineEdit{font-size:14px; padding:10px 16px; background:#161b22;"
      "border:1px solid #30363d; border-radius:8px; color:#c9d1d9;}"
      "QLineEdit:focus{border-color:#1f6feb;}");
    layout->addWidget(searchBox);

    // ── Stats row ──
    auto* stats = new QHBoxLayout();
    stats->setSpacing(16);
    auto allSamples = makeSamples();
    auto freezers = makeFreezers();

    int active = 0, checkedOut = 0, depleted = 0;
    for (auto& s : allSamples) {
      if (s.status == SampleStatus::Active) active++;
      else if (s.status == SampleStatus::CheckedOut) checkedOut++;
      else if (s.status == SampleStatus::Depleted) depleted++;
    }

    stats->addWidget(new StatCard("\xF0\x9F\xA7\xAB", QString::number(allSamples.size()), "Total Samples", "#58a6ff"));
    stats->addWidget(new StatCard("\xF0\x9F\x94\xAC", QString::number(checkedOut), "Checked Out", "#d2991d"));
    stats->addWidget(new StatCard("\xE2\x9D\x84\xEF\xB8\x8F", QString::number(freezers.size()), "Freezers", "#3fb950"));
    stats->addWidget(new StatCard("\xF0\x9F\x93\x8B", QString::number(active), "Available", "#a371f7"));
    stats->addStretch();
    layout->addLayout(stats);

    // ── Two columns: Activity | Freezer fill ──
    auto* columns = new QHBoxLayout();
    columns->setSpacing(16);

    // Left: Recent activity
    auto* activityCard = new QFrame();
    activityCard->setObjectName("card");
    activityCard->setStyleSheet("QFrame#card{background:#161b22;border:1px solid #30363d;border-radius:12px;}");
    auto* al = new QVBoxLayout(activityCard);
    al->setContentsMargins(20, 16, 20, 16);
    al->setSpacing(10);

    auto* actTitle = new QLabel("Recent Activity");
    actTitle->setStyleSheet("font-size:15px; font-weight:700; color:#e6edf3; background:transparent;");
    al->addWidget(actTitle);

    std::vector<ActivityItem> activities = {
      {"\xF0\x9F\x94\xB5", "Zhang Wei", "checked out", "PBMC Donor 001", "10 min ago"},
      {"\xF0\x9F\x9F\xA2", "Li Na",    "added",       "Plasma Donor 015",  "32 min ago"},
      {"\xF0\x9F\x94\xB4", "Wang Lei", "depleted",    "Buffy Coat 008",    "1 hour ago"},
      {"\xF0\x9F\x9F\xA1", "Zhang Wei", "checked out", "Whole Blood 005",  "2 hours ago"},
      {"\xF0\x9F\x94\xB5", "Chen Yi",  "returned",    "DNA Extract 003",   "3 hours ago"},
      {"\xF0\x9F\x9F\xA2", "Li Na",    "added",       "CSF Donor 021",     "5 hours ago"},
    };

    for (auto& a : activities) {
      auto* row = new QWidget();
      row->setStyleSheet("background:transparent;");
      auto* rl = new QHBoxLayout(row);
      rl->setContentsMargins(0, 4, 0, 4);

      auto* dot = new QLabel(a.icon);
      dot->setStyleSheet("font-size:12px; background:transparent;");
      dot->setFixedWidth(20);
      rl->addWidget(dot);

      auto* text = new QLabel(QString("<b style='color:#c9d1d9'>%1</b> "
                                      "<span style='color:#8b949e'>%2</span> "
                                      "<span style='color:#58a6ff'>%3</span>")
                                  .arg(a.user, a.action, a.detail));
      text->setStyleSheet("font-size:12px; background:transparent;");
      rl->addWidget(text, 1);

      auto* time = new QLabel(a.time);
      time->setStyleSheet("font-size:11px; color:#484f58; background:transparent;");
      rl->addWidget(time);

      al->addWidget(row);
    }
    al->addStretch();
    columns->addWidget(activityCard, 3);

    // Right: Freezer fill rates
    auto* freezerCard = new QFrame();
    freezerCard->setObjectName("card");
    freezerCard->setStyleSheet("QFrame#card{background:#161b22;border:1px solid #30363d;border-radius:12px;}");
    auto* fl = new QVBoxLayout(freezerCard);
    fl->setContentsMargins(20, 16, 20, 16);
    fl->setSpacing(12);

    auto* fTitle = new QLabel("Freezer Utilization");
    fTitle->setStyleSheet("font-size:15px; font-weight:700; color:#e6edf3; background:transparent;");
    fl->addWidget(fTitle);

    struct FInfo { QString name; double temp; double fill; };
    FInfo finfo[] = {
      {"Ultra-Low A", -80, 72},
      {"LN2 Dewar 1", -196, 45},
      {"-20C Walk-in", -20, 88},
      {"-80C Chest 3", -80, 31},
    };
    for (auto& fi : finfo) {
      auto* bar = new MiniFillBar(
        QString("%1  (%2°C)").arg(fi.name).arg(QString::number(fi.temp, 'f', 0)),
        fi.fill);
      fl->addWidget(bar);
    }

    auto* flSub = new QLabel("Fill rates based on occupied vs. total positions");
    flSub->setStyleSheet("font-size:11px; color:#484f58; background:transparent;");
    fl->addWidget(flSub);
    fl->addStretch();
    columns->addWidget(freezerCard, 2);

    layout->addLayout(columns);
    layout->addStretch();

    scroll->setWidget(content);
    outer->addWidget(scroll);
  }
};

}  // namespace fmgr::qt::pages

#endif  // FMGR_QT_PAGES_DASHBOARDPAGE_H
