// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_MAINWINDOW_H
#define FMGR_QT_MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVector>

class QAction;
class QLineEdit;
class QVBoxLayout;
class QHBoxLayout;

namespace fmgr::core { struct Sample; }

namespace fmgr::qt {

namespace pages {
class DashboardPage;
class SampleBrowserPage;
class FreezerExplorerPage;
class CheckInOutPage;
class CsvImportPage;
class AuditLogPage;
class AdminPage;
class ReportsPage;
class DonorsPage;
class StudiesPage;
class MonitoringPage;
class PickListsPage;
class RequestsPage;
class SampleDetailPage;
}  // namespace pages

namespace widgets { class BoxGridView; }

/// All application pages in navigation order.
enum class PageIndex {
    Dashboard = 0,
    Samples,
    Freezers,
    CheckInOut,
    CsvImport,
    AuditLog,
    Admin,
    Reports,
    Donors,
    Studies,
    Monitoring,
    PickLists,
    Requests,
    Count
};

/// App shell: sidebar or top-tab nav, topbar with global search,
/// lab switcher, user/role menu, and tweaks drawer trigger.
///
/// Navigation groups:
///   Workspace: Dashboard, Samples, Freezers & Boxes, Check-in/out, CSV Import
///   Governance: Audit Log, Administration, Reports, Donors, Studies,
///               Temperature & Alarms, Pick Lists, Requests & Transfers
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

    void showSampleDetail(const core::Sample& sample);
    void navigateToFreezerBox(const QString& boxId);

public slots:
    void setRole(const QString& role);       // "Member" | "LabAdmin"
    void setNavLayout(const QString& layout); // "sidebar" | "tabs"
    void setDarkMode(bool dark);
    void setAccent(const QString& hex);
    void setDensity(const QString& d);       // "comfortable" | "compact"
    void openTweaks();

private slots:
    void showPage(int index);
    void onSearchTextChanged(const QString& text);

private:
    void setupMenuBar();
    void setupStatusBar();
    void setupCentralWidget();
    void setupSidebar(QWidget* sidebarWidget);
    void setupTopbar(QVBoxLayout* topbarLayout);
    void rebuildNav();

    /// Helper: create a nav button in sidebar or top-tab style.
    QPushButton* makeNavButton(const QString& icon, const QString& label,
                               int pageIndex, bool adminOnly);

    QWidget* centralWidget_ = nullptr;
    QStackedWidget* pageStack_ = nullptr;
    QLabel* connectionLabel_ = nullptr;
    QLineEdit* globalSearch_ = nullptr;

    // Nav containers
    QWidget* sidebarWidget_ = nullptr;
    QWidget* topTabsWidget_ = nullptr;
    QVBoxLayout* sidebarLayout_ = nullptr;
    QHBoxLayout* topTabsLayout_ = nullptr;
    QVector<QPushButton*> sidebarButtons_;
    QVector<QPushButton*> topTabButtons_;

    // Pages — one per PageIndex
    QVector<QWidget*> pages_;

    // Current state
    QString role_{"Member"};
    QString navLayout_{"sidebar"};
};

}  // namespace fmgr::qt

#endif
