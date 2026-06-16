// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_PAGES_CHECKINOUTPAGE_H
#define FMGR_QT_PAGES_CHECKINOUTPAGE_H

#include "core/sample.h"
#include "mock/MockData.h"
#include "theme/Theme.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace fmgr::qt::pages {

using namespace fmgr::core;
using namespace fmgr::qt::mock;
using namespace fmgr::qt::theme;

class CheckInOutPage : public QWidget {
    Q_OBJECT
public:
    explicit CheckInOutPage(QWidget* parent = nullptr) : QWidget(parent) {
        const auto& t = Theme::instance();
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);

        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setStyleSheet("QScrollArea{background:transparent; border:none;}");

        auto* c = new QWidget(scroll);
        auto* l = new QVBoxLayout(c);
        l->setContentsMargins(28, 24, 28, 60);
        l->setSpacing(18);

        // Header
        auto* title = new QLabel("Check-in / out");
        title->setStyleSheet(QString("font-size:21px; font-weight:600; color:%1;").arg(t.text().name()));
        l->addWidget(title);
        auto* sub = new QLabel("Barcode-focused batch check-in and check-out with volume tracking");
        sub->setStyleSheet(QString("font-size:13px; color:%1;").arg(t.text2().name()));
        l->addWidget(sub);

        // Barcode focus input
        auto* scanCard = new QFrame();
        scanCard->setObjectName("card");
        scanCard->setStyleSheet(t.cardStyleSheet());
        auto* sl = new QVBoxLayout(scanCard);
        sl->setContentsMargins(24, 20, 24, 20);
        sl->setSpacing(12);

        auto* scanTitle = new QLabel("\xF0\x9F\x93\xA1 Barcode Scanner Focus Mode");
        scanTitle->setStyleSheet(QString("font-size:15px; font-weight:600; color:%1; background:transparent;")
                                  .arg(t.text().name()));
        sl->addWidget(scanTitle);

        auto* scanRow = new QHBoxLayout();
        auto* scanInput = new QLineEdit();
        scanInput->setPlaceholderText("Scan or type barcode (e.g., BC-20001)...");
        scanInput->setMinimumHeight(44);
        scanInput->setStyleSheet(QString(
            "QLineEdit{font-size:16px; padding:8px 16px; background:%1;"
            "border:2px solid %2; border-radius:7px; color:%3; font-family:\"IBM Plex Mono\",monospace;}"
            "QLineEdit:focus{border-color:%4;}")
            .arg(t.surface2().name(), t.borderStrong().name(),
                 t.text().name(), t.accent().name()));
        scanRow->addWidget(scanInput, 1);

        auto* actionCombo = new QComboBox();
        actionCombo->addItem("\xF0\x9F\x94\xB5 Check Out", "checkout");
        actionCombo->addItem("\xF0\x9F\x94\xB5 Check In",  "checkin");
        scanRow->addWidget(actionCombo);

        auto* addBtn = new QPushButton("Add to Batch");
        addBtn->setStyleSheet(QString(
            "QPushButton{background:%1; color:%2; border:none; border-radius:7px; "
            "padding:10px 20px; font-weight:600; font-size:14px;}"
            "QPushButton:hover{background:%3;}")
            .arg(t.accent().name(), t.onAccent().name(), t.accentPress().name()));
        scanRow->addWidget(addBtn);
        sl->addLayout(scanRow);

        auto* batchLabel = new QLabel("3 samples in batch");
        batchLabel->setStyleSheet(QString("color:%1; font-size:12px; background:transparent;")
                                   .arg(t.text2().name()));
        sl->addWidget(batchLabel);
        l->addWidget(scanCard);

        // Batch table
        auto* tableCard = new QFrame();
        tableCard->setObjectName("card");
        tableCard->setStyleSheet(t.cardStyleSheet());
        auto* tl = new QVBoxLayout(tableCard);
        tl->setContentsMargins(16, 14, 16, 14);

        auto* tblTitle = new QLabel("Current Batch");
        tblTitle->setStyleSheet(QString("font-size:14px; font-weight:600; color:%1; background:transparent;")
                                 .arg(t.text().name()));
        tl->addWidget(tblTitle);

        auto* table = new QTableWidget(6, 5);
        table->setHorizontalHeaderLabels({"Sample", "Barcode", "Action", "Volume Δ", "Reason"});
        table->horizontalHeader()->setStretchLastSection(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->verticalHeader()->hide();
        table->setShowGrid(false);
        table->setAlternatingRowColors(true);
        table->setStyleSheet(QString(
            "QTableWidget{background:%1; border:none; color:%2;}"
            "QTableWidget::item{padding:6px 12px; border-bottom:1px solid %3;}"
            "QHeaderView::section{background:%4; color:%5; border:none; "
            "border-bottom:1px solid %3; padding:8px 12px; font-weight:600; font-size:12px;}")
            .arg(t.surface().name(), t.text().name(), t.border().name(),
                 t.surface2().name(), t.text2().name()));

        QStringList samples = {"PBMC Donor 001", "Plasma Donor 015", "Whole Blood 005"};
        QStringList barcodes = {"BC-20001", "BC-20015", "BC-20006"};
        QStringList actions = {"Check Out", "Check Out", "Check In"};
        QStringList volumes = {"-50 µL", "-100 µL", "+200 µL"};
        QStringList reasons = {"qPCR analysis", "ELISA assay", "Return from use"};

        for (int i = 0; i < 3; ++i) {
            table->setItem(i, 0, new QTableWidgetItem(samples[i]));
            table->setItem(i, 1, new QTableWidgetItem(barcodes[i]));
            table->setItem(i, 2, new QTableWidgetItem(actions[i]));
            table->setItem(i, 3, new QTableWidgetItem(volumes[i]));
            table->setItem(i, 4, new QTableWidgetItem(reasons[i]));
        }
        tl->addWidget(table);

        // Action buttons
        auto* btnRow = new QHBoxLayout();
        btnRow->addStretch();
        auto* clearBtn = new QPushButton("Clear Batch");
        clearBtn->setStyleSheet(QString(
            "QPushButton{background:%1; color:%2; border:1px solid %3; border-radius:7px; "
            "padding:7px 16px; font-weight:550;}")
            .arg(t.surface().name(), t.text().name(), t.borderStrong().name()));
        btnRow->addWidget(clearBtn);

        auto* submitBtn = new QPushButton("\xF0\x9F\x94\x92 Submit Batch (3 items)");
        submitBtn->setStyleSheet(QString(
            "QPushButton{background:%1; color:%2; border:none; border-radius:7px; "
            "padding:8px 20px; font-weight:600;}")
            .arg(t.accent().name(), t.onAccent().name()));
        btnRow->addWidget(submitBtn);
        tl->addLayout(btnRow);

        l->addWidget(tableCard);
        l->addStretch();
        scroll->setWidget(c);
        outer->addWidget(scroll);
    }
};

}  // namespace fmgr::qt::pages
#endif
