// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_PAGES_ADMINPAGE_H
#define FMGR_QT_PAGES_ADMINPAGE_H

#include "mock/MockData_ext.h"
#include "theme/Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace fmgr::qt::pages {
using namespace fmgr::qt::mock;
using namespace fmgr::qt::theme;

class AdminPage : public QWidget {
    Q_OBJECT
public:
    explicit AdminPage(QWidget* parent = nullptr) : QWidget(parent) {
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

        auto* ttl = new QLabel("Administration");
        ttl->setStyleSheet(QString("font-size:21px;font-weight:600;color:%1;").arg(t.text().name()));
        l->addWidget(ttl);

        auto* tabs = new QTabWidget();
        tabs->setStyleSheet(QString(
            "QTabWidget::pane{background:%1;border:1px solid %2;border-radius:11px;padding:16px;}"
            "QTabBar::tab{background:%3;color:%4;border:1px solid %2;padding:8px 16px;margin-right:2px;"
            "border-top-left-radius:7px;border-top-right-radius:7px;}"
            "QTabBar::tab:selected{background:%1;color:%5;border-bottom:2px solid %5;font-weight:600;}")
            .arg(t.surface().name(),t.border().name(),t.surface2().name(),
                 t.text2().name(),t.accent().name()));

        tabs->addTab(buildItemTypesTab(t), "Item Types");
        tabs->addTab(buildCustomFieldsTab(t), "Custom Fields");
        tabs->addTab(buildBoxTypesTab(t), "Box Types");
        tabs->addTab(buildMembersTab(t), "Members & Roles");
        l->addWidget(tabs);
        l->addStretch();

        scroll->setWidget(c);
        outer->addWidget(scroll);
    }

private:
    QWidget* buildItemTypesTab(const Theme& t) {
        auto* w = new QWidget(this);
        auto* l = new QVBoxLayout(w);
        auto* desc = new QLabel("Define sample types and field inheritance hierarchy");
        desc->setStyleSheet(QString("color:%1;font-size:13px;").arg(t.text2().name()));
        l->addWidget(desc);

        auto* tbl = new QTableWidget(8, 5);
        tbl->setHorizontalHeaderLabels({"Name", "Category", "Parent Type", "Accepted Containers", "Sample Count"});
        tbl->horizontalHeader()->setStretchLastSection(true);
        tbl->setShowGrid(false);
        tbl->setAlternatingRowColors(true);

        auto types = makeItemTypes();
        for (int i = 0; i < 8 && i < (int)types.size(); ++i) {
            auto& it = types[i];
            tbl->setItem(i, 0, new QTableWidgetItem(it.name));
            tbl->setItem(i, 1, new QTableWidgetItem(it.category));
            tbl->setItem(i, 2, new QTableWidgetItem(it.parentTypeId ? "Yes" : "—"));
            auto* cont = new QTableWidgetItem(it.acceptedContainers.join(", "));
            tbl->setItem(i, 3, cont);
            tbl->setItem(i, 4, new QTableWidgetItem(QString::number(5 + i * 3)));
        }
        l->addWidget(tbl);
        return w;
    }

    QWidget* buildCustomFieldsTab(const Theme& t) {
        auto* w = new QWidget(this);
        auto* l = new QVBoxLayout(w);
        auto* desc = new QLabel("Custom fields grouped by sample type — with field inheritance");
        desc->setStyleSheet(QString("color:%1;font-size:13px;").arg(t.text2().name()));
        l->addWidget(desc);

        // Group by type
        auto fields = makeCustomFields();
        auto types = makeItemTypes();
        QStringList doneTypes;
        for (auto& it : types) {
            if (doneTypes.contains(it.name)) continue;
            doneTypes.append(it.name);

            auto* card = new QFrame();
            card->setObjectName("card");
            card->setStyleSheet(t.cardStyleSheet());
            auto* cl = new QVBoxLayout(card);
            cl->setContentsMargins(14,12,14,12);
            cl->setSpacing(6);

            auto* typeLabel = new QLabel(it.name);
            typeLabel->setStyleSheet(QString("font-weight:600;color:%1;font-size:13px;background:transparent;")
                                      .arg(t.accent().name()));
            cl->addWidget(typeLabel);

            int count = 0;
            for (auto& f : fields) {
                if (f.scopeTypeId.isEmpty() || f.scopeTypeId == QString::fromStdString(it.id)
                    || (it.parentTypeId && f.scopeTypeId == *it.parentTypeId)) {
                    auto* row = new QHBoxLayout();
                    auto* fn = new QLabel(f.name);
                    fn->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;")
                                       .arg(t.text().name()));
                    row->addWidget(fn);
                    auto* tag = f.scopeTypeId.isEmpty()
                        ? new QLabel("Shared · All")
                        : (it.parentTypeId && f.scopeTypeId == *it.parentTypeId)
                            ? new QLabel("Shared · Parent")
                            : new QLabel("Type-specific");
                    tag->setStyleSheet(QString(
                        "font-size:10px;padding:1px 6px;border-radius:10px;%1;background:transparent;")
                        .arg(f.scopeTypeId.isEmpty() ? "color:" + t.text3().name()
                             : "color:" + t.stActive().name()));
                    row->addWidget(tag);
                    row->addStretch();
                    cl->addLayout(row);
                    count++;
                }
            }
            if (count == 0) cl->addWidget(new QLabel("(no custom fields)"));
            l->addWidget(card);
        }
        return w;
    }

    QWidget* buildBoxTypesTab(const Theme& t) {
        auto* w = new QWidget(this);
        auto* l = new QVBoxLayout(w);
        auto* desc = new QLabel("Box and rack templates with position geometry");
        desc->setStyleSheet(QString("color:%1;font-size:13px;").arg(t.text2().name()));
        l->addWidget(desc);

        auto* tbl = new QTableWidget(3, 5);
        tbl->setHorizontalHeaderLabels({"Name", "Dimensions", "Manufacturer", "SKU", "Mixed Format"});
        tbl->horizontalHeader()->setStretchLastSection(true);
        tbl->setShowGrid(false);
        tbl->setAlternatingRowColors(true);
        tbl->setItem(0, 0, new QTableWidgetItem("9×9 Cryobox")); tbl->setItem(0, 1, new QTableWidgetItem("9×9 = 81 pos")); tbl->setItem(0, 2, new QTableWidgetItem("Thermo Scientific")); tbl->setItem(0, 3, new QTableWidgetItem("CRYO-9X9")); tbl->setItem(0, 4, new QTableWidgetItem("No"));
        tbl->setItem(1, 0, new QTableWidgetItem("10×10 Cryobox")); tbl->setItem(1, 1, new QTableWidgetItem("10×10 = 100 pos")); tbl->setItem(1, 2, new QTableWidgetItem("Thermo Scientific")); tbl->setItem(1, 3, new QTableWidgetItem("CRYO-10X10")); tbl->setItem(1, 4, new QTableWidgetItem("No"));
        tbl->setItem(2, 0, new QTableWidgetItem("Falcon-Mixed-01")); tbl->setItem(2, 1, new QTableWidgetItem("3×3 + 2×2 interstitial")); tbl->setItem(2, 2, new QTableWidgetItem("Eppendorf")); tbl->setItem(2, 3, new QTableWidgetItem("EP-MIX5X5")); tbl->setItem(2, 4, new QTableWidgetItem("Yes (50mL + 15mL)"));
        l->addWidget(tbl);
        return w;
    }

    QWidget* buildMembersTab(const Theme& t) {
        auto* w = new QWidget(this);
        auto* l = new QVBoxLayout(w);
        auto* desc = new QLabel("Lab members and role-based access control");
        desc->setStyleSheet(QString("color:%1;font-size:13px;").arg(t.text2().name()));
        l->addWidget(desc);

        auto* tbl = new QTableWidget(5, 4);
        tbl->setHorizontalHeaderLabels({"Name", "Email", "Role", "Permissions"});
        tbl->horizontalHeader()->setStretchLastSection(true);
        tbl->setShowGrid(false);
        tbl->setAlternatingRowColors(true);

        auto members = makeMembers();
        for (int i = 0; i < 5 && i < (int)members.size(); ++i) {
            auto& m = members[i];
            tbl->setItem(i, 0, new QTableWidgetItem(m.name));
            tbl->setItem(i, 1, new QTableWidgetItem(m.email));
            tbl->setItem(i, 2, new QTableWidgetItem(m.role));
            tbl->setItem(i, 3, new QTableWidgetItem(m.role == "LabAdmin" ? "Read, Write, Admin, Export" : "Read, Write, Export"));
        }
        l->addWidget(tbl);
        return w;
    }
};

} // namespace
#endif
