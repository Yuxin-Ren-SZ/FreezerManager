// SPDX-License-Identifier: AGPL-3.0-or-later
// Governance stub pages — structured layouts with mock data, ready for full implementation

#ifndef FMGR_QT_PAGES_GOVERNANCE_STUBS_H
#define FMGR_QT_PAGES_GOVERNANCE_STUBS_H

#include "mock/MockData_ext.h"
#include "theme/Theme.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace fmgr::qt::pages {
using namespace fmgr::qt::mock;
using namespace fmgr::qt::theme;

// ── Shared helper: page boilerplate ─────────────────────────────────
struct PageHelper {
    QVBoxLayout* layout;
    const Theme& t;
    PageHelper(QWidget* parent) : t(Theme::instance()) {
        auto* outer = new QVBoxLayout(parent); outer->setContentsMargins(0,0,0,0);
        auto* scroll = new QScrollArea(parent); scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setStyleSheet("QScrollArea{background:transparent;border:none;}");
        auto* c = new QWidget(scroll); layout = new QVBoxLayout(c);
        layout->setContentsMargins(28,24,28,60); layout->setSpacing(16);
        scroll->setWidget(c); outer->addWidget(scroll);
    }
    void addTitle(const QString& title, const QString& sub = {}) {
        auto* tl = new QLabel(title);
        tl->setStyleSheet(QString("font-size:21px;font-weight:600;color:%1;").arg(t.text().name()));
        layout->addWidget(tl);
        if (!sub.isEmpty()) {
            auto* sl = new QLabel(sub);
            sl->setStyleSheet(QString("font-size:13px;color:%1;").arg(t.text2().name()));
            layout->addWidget(sl);
        }
    }
    QFrame* addCard() {
        auto* card = new QFrame(); card->setObjectName("card");
        card->setStyleSheet(t.cardStyleSheet());
        layout->addWidget(card);
        return card;
    }
};

// ── Reports & Analytics ─────────────────────────────────────────────
class ReportsPage : public QWidget {
public:
    explicit ReportsPage(QWidget* p = nullptr) : QWidget(p) {
        PageHelper h(this); h.addTitle("Reports & Analytics", "Capacity trends, sample aging, and consumption charts");
        auto* card = h.addCard();
        auto* cl = new QVBoxLayout(card); cl->setContentsMargins(20,16,20,16);
        auto* info = new QLabel("\xF0\x9F\x93\x8A Chart integration coming — capacity trends, sample aging, freeze-thaw cycles");
        info->setStyleSheet(QString("color:%1;font-size:13px;").arg(h.t.text2().name()));
        cl->addWidget(info);
    }
};

// ── Donors / Subjects ───────────────────────────────────────────────
class DonorsPage : public QWidget {
public:
    explicit DonorsPage(QWidget* p = nullptr) : QWidget(p) {
        PageHelper h(this); h.addTitle("Donors / Subjects", "PHI-aware registry linking samples to donor IDs");
        auto* card = h.addCard();
        auto* cl = new QVBoxLayout(card); cl->setContentsMargins(16,14,16,14);
        auto* tbl = new QTableWidget(8, 6);
        tbl->setHorizontalHeaderLabels({"Code", "Name", "MRN", "Consent", "Study", "Samples"});
        tbl->horizontalHeader()->setStretchLastSection(true);
        tbl->setShowGrid(false); tbl->setAlternatingRowColors(true);
        auto donors = makeDonors();
        for (int i = 0; i < 8; ++i) {
            auto& d = donors[i];
            tbl->setItem(i,0,new QTableWidgetItem(d.donorCode));
            tbl->setItem(i,1,new QTableWidgetItem(d.consentStatus=="Consented"?d.fullName:"•••••••"));
            tbl->setItem(i,2,new QTableWidgetItem("•••-"+d.mrn.right(5)));
            tbl->setItem(i,3,new QTableWidgetItem(d.consentStatus));
            tbl->setItem(i,4,new QTableWidgetItem(d.irbProtocol));
            tbl->setItem(i,5,new QTableWidgetItem(QString::number(d.sampleCount)));
        }
        cl->addWidget(tbl);
    }
};

// ── Studies / Protocols ─────────────────────────────────────────────
class StudiesPage : public QWidget {
public:
    explicit StudiesPage(QWidget* p = nullptr) : QWidget(p) {
        PageHelper h(this); h.addTitle("Studies / Protocols", "IRB protocol view: samples per study, enrollment, consent");
        auto studies = makeStudies();
        for (auto& s : studies) {
            auto* card = h.addCard();
            auto* cl = new QVBoxLayout(card); cl->setContentsMargins(16,14,16,14);
            auto* title = new QLabel(s.name);
            title->setStyleSheet(QString("font-weight:600;color:%1;font-size:14px;").arg(h.t.text().name()));
            cl->addWidget(title);
            auto* detail = new QLabel(QString("PI: %1 | IRB: %2 | Status: %3 | Donors: %4 | Samples: %5")
                .arg(s.piName,s.irbProtocol,s.status).arg(s.enrolledDonors).arg(s.totalSamples));
            detail->setStyleSheet(QString("color:%1;font-size:12px;").arg(h.t.text2().name()));
            cl->addWidget(detail);
        }
    }
};

// ── Temperature & Alarms ────────────────────────────────────────────
class MonitoringPage : public QWidget {
public:
    explicit MonitoringPage(QWidget* p = nullptr) : QWidget(p) {
        PageHelper h(this); h.addTitle("Temperature & Alarms", "Freezer temperature logs, excursion events, defrost cycles");
        auto* card = h.addCard();
        auto* cl = new QVBoxLayout(card); cl->setContentsMargins(16,14,16,14);
        auto* tbl = new QTableWidget(10, 5);
        tbl->setHorizontalHeaderLabels({"Time", "Freezer", "Temp", "Event", "Severity"});
        tbl->horizontalHeader()->setStretchLastSection(true);
        tbl->setShowGrid(false); tbl->setAlternatingRowColors(true);
        auto logs = makeTempLogs();
        for (int i = 0; i < 10 && i < (int)logs.size(); ++i) {
            auto& log = logs[i];
            tbl->setItem(i,0,new QTableWidgetItem(log.timestamp));
            tbl->setItem(i,1,new QTableWidgetItem(log.freezerName));
            tbl->setItem(i,2,new QTableWidgetItem(QString::number(log.temperature,'f',1)+"°C"));
            tbl->setItem(i,3,new QTableWidgetItem(log.eventType));
            auto* sev = new QTableWidgetItem(log.severity);
            sev->setForeground(log.severity=="critical"?QColor("#c63a3a"):log.severity=="warning"?QColor("#c5841a"):QColor("#1f9d57"));
            tbl->setItem(i,4,sev);
        }
        cl->addWidget(tbl);
    }
};

// ── Pick Lists / Worklists ──────────────────────────────────────────
class PickListsPage : public QWidget {
public:
    explicit PickListsPage(QWidget* p = nullptr) : QWidget(p) {
        PageHelper h(this); h.addTitle("Pick Lists / Worklists", "Build pull lists for experiments with optimized retrieval paths");
        auto lists = makePickLists();
        for (auto& pl : lists) {
            auto* card = h.addCard();
            auto* cl = new QVBoxLayout(card); cl->setContentsMargins(16,14,16,14);
            auto* title = new QLabel(pl.name);
            title->setStyleSheet(QString("font-weight:600;color:%1;font-size:14px;").arg(h.t.text().name()));
            cl->addWidget(title);
            auto* meta = new QLabel(QString("By: %1 | Created: %2 | Route: %3").arg(pl.createdBy,pl.createdAt,pl.optimizedRoute));
            meta->setStyleSheet(QString("color:%1;font-size:11px;").arg(h.t.text2().name()));
            cl->addWidget(meta);
            for (auto& item : pl.items) {
                auto* row = new QLabel(QString("  %1 — %2 [%3]").arg(item.sampleName,item.position,item.status));
                row->setStyleSheet(QString("color:%1;font-size:12px;").arg(h.t.text3().name()));
                cl->addWidget(row);
            }
        }
    }
};

// ── Requests & Transfers ────────────────────────────────────────────
class RequestsPage : public QWidget {
public:
    explicit RequestsPage(QWidget* p = nullptr) : QWidget(p) {
        PageHelper h(this); h.addTitle("Requests & Transfers", "Sample transfer requests with approval workflow");
        auto* card = h.addCard();
        auto* cl = new QVBoxLayout(card); cl->setContentsMargins(16,14,16,14);
        auto* tbl = new QTableWidget(5, 6);
        tbl->setHorizontalHeaderLabels({"Sample", "From → To", "Requested By", "Reason", "Status", "Approved"});
        tbl->horizontalHeader()->setStretchLastSection(true);
        tbl->setShowGrid(false); tbl->setAlternatingRowColors(true);
        auto reqs = makeTransferRequests();
        for (int i = 0; i < 5; ++i) {
            auto& r = reqs[i];
            tbl->setItem(i,0,new QTableWidgetItem(r.sampleName));
            tbl->setItem(i,1,new QTableWidgetItem(r.fromLab+" → "+r.toLab));
            tbl->setItem(i,2,new QTableWidgetItem(r.requestedBy));
            tbl->setItem(i,3,new QTableWidgetItem(r.reason));
            auto* st = new QTableWidgetItem(r.status);
            if (r.status=="Approved") st->setForeground(QColor("#1f9d57"));
            else if (r.status=="Rejected") st->setForeground(QColor("#c63a3a"));
            else st->setForeground(QColor("#c5841a"));
            tbl->setItem(i,4,st);
            tbl->setItem(i,5,new QTableWidgetItem(r.approvedBy.value_or("—")));
        }
        cl->addWidget(tbl);
    }
};

} // namespace
#endif
