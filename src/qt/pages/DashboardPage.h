// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_PAGES_DASHBOARDPAGE_H
#define FMGR_QT_PAGES_DASHBOARDPAGE_H

#include "core/sample.h"
#include "mock/MockData.h"
#include "mock/MockData_ext.h"
#include "theme/Theme.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace fmgr::qt::pages {

using namespace fmgr::core;
using namespace fmgr::qt::mock;
using namespace fmgr::qt::theme;

// ── Mini fill bar (freezer utilization) ─────────────────────────────
class MiniFillBar : public QWidget {
public:
    explicit MiniFillBar(const QString& label, const QString& detail,
                         double fillPct, QWidget* parent = nullptr)
        : QWidget(parent), label_(label), detail_(detail), fillPct_(fillPct) {
        setFixedHeight(36);
        setMinimumWidth(140);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto& t = Theme::instance();

        // Background track
        p.setPen(Qt::NoPen);
        p.setBrush(t.surface3());
        p.drawRoundedRect(QRect(0, 0, width(), 8), 4, 4);

        // Fill
        int fillW = static_cast<int>((width() - 4) * fillPct_ / 100.0);
        if (fillW > 2) {
            QColor c = fillPct_ > 80 ? t.stDestroyed()
                     : fillPct_ > 60 ? t.stChecked()
                     : t.stActive();
            p.setBrush(c);
            p.drawRoundedRect(QRect(0, 0, fillW, 8), 4, 4);
        }

        // Label
        p.setPen(t.text());
        QFont f = p.font();
        f.setPixelSize(12);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.drawText(QRect(0, 12, width(), 22), Qt::AlignLeft | Qt::AlignVCenter, label_);

        p.setPen(t.text2());
        f.setPixelSize(11);
        f.setWeight(QFont::Normal);
        p.setFont(f);
        p.drawText(QRect(0, 12, width(), 22), Qt::AlignRight | Qt::AlignVCenter,
                   QString("%1%").arg(QString::number(fillPct_, 'f', 0)));

        p.setPen(t.text3());
        p.drawText(QRect(0, 26, width(), 12), Qt::AlignLeft, detail_);
    }
private:
    QString label_, detail_;
    double fillPct_;
};

// ── Stat card ───────────────────────────────────────────────────────
class StatCard : public QFrame {
public:
    StatCard(const QString& icon, const QString& value, const QString& label,
             const QColor& accent, QWidget* parent = nullptr)
        : QFrame(parent) {
        setObjectName("statcard");
        setFixedSize(170, 105);
        const auto& t = Theme::instance();
        setStyleSheet(QString(
            "QFrame#statcard{background:%1;border:1px solid %2;border-radius:11px;padding:12px;}"
            "QFrame#statcard:hover{border-color:%3;}")
            .arg(t.surface().name(), t.border().name(), accent.name()));

        auto* l = new QVBoxLayout(this);
        l->setSpacing(2);
        l->setContentsMargins(14, 10, 14, 10);

        auto* ic = new QLabel(icon);
        ic->setStyleSheet("font-size:20px; background:transparent;");
        l->addWidget(ic);

        auto* v = new QLabel(value);
        v->setStyleSheet(QString(
            "font-size:26px; font-weight:600; letter-spacing:-0.02em; "
            "color:%1; background:transparent;").arg(accent.name()));
        l->addWidget(v);

        auto* lb = new QLabel(label);
        lb->setStyleSheet(QString(
            "font-size:12px; color:%1; background:transparent; font-weight:600;")
            .arg(t.text2().name()));
        l->addWidget(lb);
    }
};

// ── Dashboard page ──────────────────────────────────────────────────
class DashboardPage : public QWidget {
    Q_OBJECT

public:
    explicit DashboardPage(QWidget* parent = nullptr) : QWidget(parent) {
        const auto& t = Theme::instance();
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);

        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setStyleSheet("QScrollArea{background:transparent; border:none;}");

        auto* content = new QWidget(scroll);
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(28, 24, 28, 60);
        layout->setSpacing(18);

        // ── Page header ──
        auto* headerRow = new QHBoxLayout();
        auto* headerText = new QVBoxLayout();
        auto* title = new QLabel("Dashboard");
        title->setStyleSheet(QString(
            "font-size:21px; font-weight:600; color:%1; letter-spacing:-0.015em;")
            .arg(t.text().name()));
        headerText->addWidget(title);

        auto* sub = new QLabel("Overview of your lab's freezer inventory and recent activity");
        sub->setStyleSheet(QString("font-size:13px; color:%1;").arg(t.text2().name()));
        headerText->addWidget(sub);
        headerRow->addLayout(headerText);
        headerRow->addStretch();
        layout->addLayout(headerRow);

        // ── Quick search ──
        auto* searchBox = new QLineEdit();
        searchBox->setPlaceholderText("Quick search — sample name, barcode, freezer, position...");
        searchBox->setClearButtonEnabled(true);
        searchBox->setMinimumHeight(40);
        searchBox->setStyleSheet(QString(
            "QLineEdit{font-size:13px; padding:8px 14px; background:%1;"
            "border:1px solid %2; border-radius:7px; color:%3;}"
            "QLineEdit:focus{border-color:%4;}")
            .arg(t.surface2().name(), t.borderStrong().name(),
                 t.text().name(), t.accent().name()));
        layout->addWidget(searchBox);

        // ── Stats row ──
        auto* stats = new QHBoxLayout();
        stats->setSpacing(14);

        auto allSamples = makeSamples();
        auto freezers = makeFreezers();
        int active = 0, checkedOut = 0, depleted = 0, destroyed = 0;
        for (auto& s : allSamples) {
            switch (s.status) {
            case SampleStatus::Active:     active++; break;
            case SampleStatus::CheckedOut: checkedOut++; break;
            case SampleStatus::Depleted:   depleted++; break;
            case SampleStatus::Destroyed:  destroyed++; break;
            default: break;
            }
        }

        stats->addWidget(new StatCard("\xF0\x9F\xA7\xAB", QString::number(allSamples.size()),
                                       "Total Samples", QColor(0x25, 0x63, 0xC9)));
        stats->addWidget(new StatCard("\xF0\x9F\x94\xAC", QString::number(checkedOut),
                                       "Checked Out", QColor(0xC5, 0x84, 0x1A)));
        stats->addWidget(new StatCard("\xE2\x9D\x84\xEF\xB8\x8F", QString::number(freezers.size()),
                                       "Freezers", QColor(0x1F, 0x9D, 0x57)));
        stats->addWidget(new StatCard("\xF0\x9F\x93\x8B", QString::number(active),
                                       "Available", QColor(0x7B, 0x3F, 0xE4)));
        stats->addStretch();
        layout->addLayout(stats);

        // ── Two columns: Activity | Freezer utilization ──
        auto* columns = new QHBoxLayout();
        columns->setSpacing(14);

        // Left: Recent activity
        auto* activityCard = new QFrame();
        activityCard->setObjectName("card");
        activityCard->setStyleSheet(t.cardStyleSheet());
        auto* al = new QVBoxLayout(activityCard);
        al->setContentsMargins(20, 16, 20, 16);
        al->setSpacing(8);

        auto* actTitle = new QLabel("Recent Activity");
        actTitle->setStyleSheet(QString(
            "font-size:14px; font-weight:600; color:%1; background:transparent;")
            .arg(t.text().name()));
        al->addWidget(actTitle);

        auto activities = makeActivities();
        for (auto& a : activities) {
            auto* row = new QWidget();
            row->setStyleSheet("background:transparent;");
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 3, 0, 3);

            auto* dot = new QLabel(a.icon);
            dot->setStyleSheet("font-size:11px; background:transparent;");
            dot->setFixedWidth(18);
            rl->addWidget(dot);

            QString detailColor = (a.action == "checked out") ? "#c5841a"
                                : (a.action == "depleted" || a.action == "destroyed")
                                    ? "#c63a3a" : "#1f9d57";
            auto* text = new QLabel(QString(
                "<b style='color:%1'>%2</b> "
                "<span style='color:%3'>%4</span> "
                "<span style='color:%5'>%6</span>")
                .arg(t.text().name(), a.user, t.text2().name(), a.action,
                     detailColor, a.detail));
            text->setStyleSheet("font-size:12px; background:transparent;");
            rl->addWidget(text, 1);

            auto* time = new QLabel(a.time);
            time->setStyleSheet(QString("font-size:11px; color:%1; background:transparent;")
                                 .arg(t.text3().name()));
            rl->addWidget(time);
            al->addWidget(row);
        }
        al->addStretch();
        columns->addWidget(activityCard, 3);

        // Right: Freezer utilization
        auto* freezerCard = new QFrame();
        freezerCard->setObjectName("card");
        freezerCard->setStyleSheet(t.cardStyleSheet());
        auto* fl = new QVBoxLayout(freezerCard);
        fl->setContentsMargins(20, 16, 20, 16);
        fl->setSpacing(10);

        auto* fTitle = new QLabel("Freezer Utilization");
        fTitle->setStyleSheet(QString(
            "font-size:14px; font-weight:600; color:%1; background:transparent;")
            .arg(t.text().name()));
        fl->addWidget(fTitle);

        auto fu = makeFreezerUtil();
        for (auto& fi : fu) {
            fl->addWidget(new MiniFillBar(
                QString("%1 (%2°C)").arg(fi.name).arg(QString::number(fi.temp, 'f', 0)),
                QString("%1 / %2 slots occupied").arg(fi.occupiedSlots).arg(fi.totalSlots),
                fi.fillPct));
        }

        auto* flSub = new QLabel("Fill rates based on occupied vs. total positions");
        flSub->setStyleSheet(QString("font-size:11px; color:%1; background:transparent;")
                              .arg(t.text3().name()));
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

#endif
