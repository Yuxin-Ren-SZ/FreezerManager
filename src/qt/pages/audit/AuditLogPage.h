// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_PAGES_AUDITLOGPAGE_H
#define FMGR_QT_PAGES_AUDITLOGPAGE_H

#include "mock/MockData.h"
#include "theme/Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace fmgr::qt::pages {
using namespace fmgr::qt::mock;
using namespace fmgr::qt::theme;

class AuditLogPage : public QWidget {
    Q_OBJECT
public:
    explicit AuditLogPage(QWidget* parent = nullptr) : QWidget(parent) {
        const auto& t = Theme::instance();
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0,0,0,0);

        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setStyleSheet("QScrollArea{background:transparent;border:none;}");

        auto* c = new QWidget(scroll);
        auto* l = new QVBoxLayout(c);
        l->setContentsMargins(28,24,28,60);
        l->setSpacing(16);

        // Header
        auto* hdr = new QHBoxLayout();
        auto* th = new QVBoxLayout();
        auto* ttl = new QLabel("Audit Log");
        ttl->setStyleSheet(QString("font-size:21px;font-weight:600;color:%1;").arg(t.text().name()));
        th->addWidget(ttl);
        auto* sub = new QLabel("Append-only chain-of-custody with hash-chain verification");
        sub->setStyleSheet(QString("font-size:13px;color:%1;").arg(t.text2().name()));
        th->addWidget(sub);
        hdr->addLayout(th);
        hdr->addStretch();
        auto* verifyBtn = new QPushButton("\xF0\x9F\x94\x92 Verify Chain Integrity");
        verifyBtn->setStyleSheet(QString(
            "QPushButton{background:%1;color:%2;border:none;border-radius:7px;"
            "padding:8px 18px;font-weight:600;}")
            .arg(t.stActive().name(), "#fff"));
        hdr->addWidget(verifyBtn);
        l->addLayout(hdr);

        // Verification status
        auto* statusCard = new QFrame();
        statusCard->setObjectName("card");
        statusCard->setStyleSheet(t.cardStyleSheet());
        auto* sl = new QHBoxLayout(statusCard);
        sl->setContentsMargins(16,12,16,12);
        auto* check = new QLabel("\xE2\x9C\x85 Hash chain verified — all 247 events intact");
        check->setStyleSheet(QString("color:%1;font-size:14px;font-weight:600;background:transparent;")
                              .arg(t.stActive().name()));
        sl->addWidget(check);
        sl->addStretch();
        l->addWidget(statusCard);

        // Audit table
        auto* card = new QFrame();
        card->setObjectName("card");
        card->setStyleSheet(t.cardStyleSheet());
        auto* cl = new QVBoxLayout(card);
        cl->setContentsMargins(16,14,16,14);
        cl->setSpacing(8);

        auto* tblTitle = new QLabel("Audit Events");
        tblTitle->setStyleSheet(QString("font-size:14px;font-weight:600;color:%1;background:transparent;")
                                 .arg(t.text().name()));
        cl->addWidget(tblTitle);

        auto* tbl = new QTableWidget(12, 6);
        tbl->setHorizontalHeaderLabels({"Timestamp", "User", "Sample", "Action", "Reason", "Hash"});
        tbl->horizontalHeader()->setStretchLastSection(true);
        tbl->verticalHeader()->hide();
        tbl->setShowGrid(false);
        tbl->setAlternatingRowColors(true);
        tbl->setStyleSheet(QString(
            "QTableWidget{background:%1;border:none;color:%2;}"
            "QTableWidget::item{padding:5px 10px;border-bottom:1px solid %3;}"
            "QHeaderView::section{background:%4;color:%5;border:none;"
            "border-bottom:1px solid %3;padding:8px 10px;font-weight:600;font-size:11px;}")
            .arg(t.surface().name(),t.text().name(),t.border().name(),
                 t.surface2().name(),t.text2().name()));

        auto events = makeAuditEvents();
        for (int i = 0; i < (int)events.size() && i < 12; ++i) {
            auto& e = events[i];
            tbl->setItem(i, 0, new QTableWidgetItem("2026-06-14 " + QString::number(8 + i % 12) + ":00"));
            tbl->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(e.userName)));
            tbl->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(e.sampleId).left(12) + "..."));
            tbl->setItem(i, 3, new QTableWidgetItem(e.action == CheckoutAction::CheckedOut ? "Check Out" :
                                                      e.action == CheckoutAction::CheckedIn ? "Check In" :
                                                      e.action == CheckoutAction::Destroyed ? "Destroy" : "Other"));
            tbl->setItem(i, 4, new QTableWidgetItem(QString::fromStdString(e.reason)));
            tbl->setItem(i, 5, new QTableWidgetItem("sha256:abc" + QString::number(i) + "..."));
        }
        cl->addWidget(tbl);
        l->addWidget(card);
        l->addStretch();

        scroll->setWidget(c);
        outer->addWidget(scroll);
    }
};

} // namespace
#endif
