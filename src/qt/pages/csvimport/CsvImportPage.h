// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_PAGES_CSVIMPORTPAGE_H
#define FMGR_QT_PAGES_CSVIMPORTPAGE_H

#include "theme/Theme.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

namespace fmgr::qt::pages {
using namespace fmgr::qt::theme;

class CsvImportPage : public QWidget {
    Q_OBJECT
public:
    explicit CsvImportPage(QWidget* parent = nullptr) : QWidget(parent) {
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
        l->setSpacing(18);

        auto* title = new QLabel("CSV Import Wizard");
        title->setStyleSheet(QString("font-size:21px;font-weight:600;color:%1;").arg(t.text().name()));
        l->addWidget(title);
        auto* sub = new QLabel("Step 2 of 4 — Validate & Confirm");
        sub->setStyleSheet(QString("font-size:13px;color:%1;").arg(t.text2().name()));
        l->addWidget(sub);

        // Progress
        auto* pg = new QProgressBar();
        pg->setValue(50);
        pg->setTextVisible(false);
        pg->setFixedHeight(6);
        pg->setStyleSheet(QString(
            "QProgressBar{background:%1;border-radius:3px;border:none;}"
            "QProgressBar::chunk{background:%2;border-radius:3px;}")
            .arg(t.surface3().name(), t.accent().name()));
        l->addWidget(pg);

        // Steps
        auto* steps = new QHBoxLayout();
        QString stepStyle = QString("color:%1;font-size:12px;font-weight:600;");
        auto* s1 = new QLabel("\xE2\x9C\x85 Select File"); s1->setStyleSheet(stepStyle.arg(t.stActive().name())); steps->addWidget(s1);
        steps->addWidget(new QLabel("→"));
        auto* s2 = new QLabel("\xF0\x9F\x94\x8D Validate"); s2->setStyleSheet(stepStyle.arg(t.accent().name())); steps->addWidget(s2);
        steps->addWidget(new QLabel("→"));
        auto* s3 = new QLabel("Map Columns"); s3->setStyleSheet(stepStyle.arg(t.text3().name())); steps->addWidget(s3);
        steps->addWidget(new QLabel("→"));
        auto* s4 = new QLabel("Import"); s4->setStyleSheet(stepStyle.arg(t.text3().name())); steps->addWidget(s4);
        steps->addStretch();
        l->addLayout(steps);

        // Dry-run report
        auto* card = new QFrame();
        card->setObjectName("card");
        card->setStyleSheet(t.cardStyleSheet());
        auto* cl = new QVBoxLayout(card);
        cl->setContentsMargins(20,16,20,16);
        cl->setSpacing(12);

        auto* ct = new QLabel("\xF0\x9F\x93\x8B Dry-run Validation Report");
        ct->setStyleSheet(QString("font-size:14px;font-weight:600;color:%1;").arg(t.text().name()));
        cl->addWidget(ct);

        // Summary
        auto* summary = new QHBoxLayout();
        auto stat = [&](const QString& v, const QString& k, const QColor& c) {
            auto* w = new QWidget(this);
            auto* wl = new QVBoxLayout(w);
            auto* vl = new QLabel(v); vl->setStyleSheet(QString("font-size:20px;font-weight:600;color:%1;").arg(c.name())); wl->addWidget(vl);
            auto* kl = new QLabel(k); kl->setStyleSheet(QString("font-size:11px;color:%1;").arg(t.text2().name())); wl->addWidget(kl);
            return w;
        };
        summary->addWidget(stat("12", "Rows parsed", t.stActive()));
        summary->addWidget(stat("2", "With errors", t.stDestroyed()));
        summary->addWidget(stat("10", "Ready to import", t.accent()));
        summary->addWidget(stat("0", "Duplicates", t.text3()));
        summary->addStretch();
        cl->addLayout(summary);

        // Error detail
        auto* errTable = new QTableWidget(2, 4);
        errTable->setHorizontalHeaderLabels({"Row", "Field", "Error", "Suggestion"});
        errTable->horizontalHeader()->setStretchLastSection(true);
        errTable->setItem(0, 0, new QTableWidgetItem("3"));
        errTable->setItem(0, 1, new QTableWidgetItem("volume"));
        errTable->setItem(0, 2, new QTableWidgetItem("Invalid value 'N/A'"));
        errTable->setItem(0, 3, new QTableWidgetItem("Expected numeric value in µL"));
        errTable->setItem(1, 0, new QTableWidgetItem("7"));
        errTable->setItem(1, 1, new QTableWidgetItem("barcode"));
        errTable->setItem(1, 2, new QTableWidgetItem("Duplicate barcode BC-20001"));
        errTable->setItem(1, 3, new QTableWidgetItem("Barcode must be unique"));
        cl->addWidget(errTable);

        auto* btnRow = new QHBoxLayout();
        btnRow->addStretch();
        auto* back = new QPushButton("← Back");
        btnRow->addWidget(back);
        auto* next = new QPushButton("Map Columns →");
        next->setStyleSheet(QString("QPushButton{background:%1;color:%2;border:none;border-radius:7px;padding:8px 20px;font-weight:600;}")
            .arg(t.accent().name(),t.onAccent().name()));
        btnRow->addWidget(next);
        cl->addLayout(btnRow);

        l->addWidget(card);
        l->addStretch();
        scroll->setWidget(c);
        outer->addWidget(scroll);
    }
};

} // namespace
#endif
