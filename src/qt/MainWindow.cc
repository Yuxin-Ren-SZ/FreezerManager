// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MainWindow.h"

#include "mock/MockData.h"
#include "mock/MockData_ext.h"
#include "pages/DashboardPage.h"
#include "pages/FreezerExplorerPage.h"
#include "pages/SampleBrowserPage.h"
#include "pages/SampleDetailPage.h"
#include "theme/Theme.h"
#include "widgets/BoxGridView.h"
#include "pages/checkinout/CheckInOutPage.h"
#include "pages/csvimport/CsvImportPage.h"
#include "pages/audit/AuditLogPage.h"
#include "pages/admin/AdminPage.h"
#include "pages/GovernanceStubs.h"

#include <QApplication>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QStatusBar>
#include <QStyleFactory>
#include <QVBoxLayout>

namespace fmgr::qt {

using namespace theme;

// ── Nav item descriptor ─────────────────────────────────────────────
struct NavItem {
    const char* icon;
    const char* label;
    PageIndex page;
    bool adminOnly;
    const char* groupLabel;  // nullptr = same group; non-null = new group header
};

static const NavItem kNavItems[] = {
    // Workspace
    {"\xF0\x9F\x8F\xA0", "Dashboard",           PageIndex::Dashboard,  false, nullptr},
    {"\xF0\x9F\x94\x8D", "Samples",             PageIndex::Samples,    false, nullptr},
    {"\xE2\x9D\x84\xEF\xB8\x8F", "Freezers & Boxes", PageIndex::Freezers,  false, nullptr},
    {"\xF0\x9F\x93\x8B", "Check-in / out",      PageIndex::CheckInOut, false, nullptr},
    {"\xF0\x9F\x93\xA5", "CSV Import",          PageIndex::CsvImport,  false, nullptr},
    // Governance
    {"\xF0\x9F\x94\x92", "Audit Log",           PageIndex::AuditLog,   false, "Governance"},
    {"\xE2\x9A\x99\xEF\xB8\x8F", "Administration",  PageIndex::Admin,  true,  nullptr},
    {"\xF0\x9F\x93\x8A", "Reports & Analytics", PageIndex::Reports,    false, nullptr},
    {"\xF0\x9F\x91\xA4", "Donors / Subjects",   PageIndex::Donors,     false, nullptr},
    {"\xF0\x9F\x93\x84", "Studies / Protocols", PageIndex::Studies,    false, nullptr},
    {"\xF0\x9F\x8C\xA1\xEF\xB8\x8F", "Temperature & Alarms", PageIndex::Monitoring, false, nullptr},
    {"\xF0\x9F\x93\x8B", "Pick Lists",           PageIndex::PickLists,  false, nullptr},
    {"\xF0\x9F\x94\x84", "Requests & Transfers",  PageIndex::Requests,  false, nullptr},
};
static constexpr int kNavCount = sizeof(kNavItems) / sizeof(kNavItems[0]);

// ── Constructor ─────────────────────────────────────────────────────
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("FreezerManager");
    resize(1320, 860);
    setMinimumSize(1000, 640);
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    qApp->setStyleSheet(Theme::instance().globalStyleSheet());

    setupStatusBar();
    setupCentralWidget();
    showPage(0);
}

// ── Menu bar (File, View, Help) ─────────────────────────────────────
void MainWindow::setupMenuBar() {
    QMenu* fileMenu = menuBar()->addMenu("&File");
    QAction* quit = fileMenu->addAction("&Quit");
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    QMenu* viewMenu = menuBar()->addMenu("&View");
    viewMenu->addAction("Dashboard")->setShortcut(Qt::CTRL | Qt::Key_1);
    viewMenu->addAction("Samples")->setShortcut(Qt::CTRL | Qt::Key_2);
    viewMenu->addAction("Freezers")->setShortcut(Qt::CTRL | Qt::Key_3);
    viewMenu->addSeparator();
    QAction* fs = viewMenu->addAction("Toggle &Full Screen");
    fs->setShortcut(Qt::Key_F11);
    connect(fs, &QAction::triggered, this, [this]() {
        isFullScreen() ? showNormal() : showFullScreen();
    });

    QMenu* helpMenu = menuBar()->addMenu("&Help");
    connect(helpMenu->addAction("&About"), &QAction::triggered, this, [this]() {
        QMessageBox::about(this, "About",
            "<h3 style='color:#2563c9'>FreezerManager 0.1.0</h3>"
            "<p>Self-hosted freezer & biospecimen management.</p>");
    });
}

// ── Status bar ──────────────────────────────────────────────────────
void MainWindow::setupStatusBar() {
    connectionLabel_ = new QLabel("  \xF0\x9F\x9F\xA2 Connected · grpc://freezerd:8443");
    connectionLabel_->setStyleSheet("color:#1f9d57; font-weight:500;");
    statusBar()->addPermanentWidget(connectionLabel_);

    auto* auditLabel = new QLabel("\xF0\x9F\x94\x92 Audit chain verified");
    auditLabel->setStyleSheet(
        QString("color:%1; font-size:11px;").arg(Theme::instance().text3().name()));
    statusBar()->addPermanentWidget(auditLabel);
}

// ── Top bar (brand, search, lab switcher, gear, user menu) ──────────
void MainWindow::setupTopbar(QVBoxLayout* topLayout) {
    auto& t = Theme::instance();
    auto* bar = new QWidget(centralWidget_);
    bar->setFixedHeight(52);
    bar->setStyleSheet(QString(
        "background:%1; border-bottom:1px solid %2;")
        .arg(t.surface().name(), t.border().name()));
    auto* hl = new QHBoxLayout(bar);
    hl->setContentsMargins(14, 0, 14, 0);
    hl->setSpacing(12);

    // Brand
    auto* brand = new QLabel("\xF0\x9F\xA7\x8A <b>FreezerManager</b>");
    brand->setStyleSheet(QString(
        "font-size:14px; font-weight:600; color:%1; background:transparent; "
        "letter-spacing:-0.01em;")
        .arg(t.text().name()));
    hl->addWidget(brand);

    auto* ver = new QLabel("v0.1 · pre-α");
    ver->setStyleSheet(QString(
        "font-family:\"IBM Plex Mono\",monospace; font-size:10px; color:%1; "
        "border:1px solid %2; padding:1px 5px; border-radius:20px; background:transparent;")
        .arg(t.text3().name(), t.border().name()));
    hl->addWidget(ver);

    hl->addSpacing(10);

    // Global search
    globalSearch_ = new QLineEdit();
    globalSearch_->setPlaceholderText("Search samples, boxes, donors, barcodes…");
    globalSearch_->setClearButtonEnabled(true);
    globalSearch_->setMaximumWidth(460);
    globalSearch_->setStyleSheet(QString(
        "QLineEdit{font-size:13px; padding:0px 12px 0px 32px; "
        "background:%1; border:1px solid %2; border-radius:7px; color:%3;}")
        .arg(t.surface2().name(), t.borderStrong().name(), t.text().name()));
    hl->addWidget(globalSearch_, 1);

    hl->addStretch();

    // Lab switcher
    auto* labBtn = new QPushButton("  \xF0\x9F\x94\xB5 Chen Lab  ▾");
    labBtn->setStyleSheet(QString(
        "QPushButton{background:%1; border:1px solid %2; border-radius:7px; "
        "color:%3; font-size:13px; padding:6px 11px;}")
        .arg(t.surface2().name(), t.border().name(), t.text().name()));
    hl->addWidget(labBtn);

    // Tweaks / gear
    auto* gearBtn = new QPushButton("\xE2\x9A\x99");
    gearBtn->setToolTip("Tweaks: theme, layout, density");
    gearBtn->setFixedSize(34, 34);
    gearBtn->setStyleSheet(QString(
        "QPushButton{background:%1; border:1px solid %2; border-radius:7px; "
        "color:%3; font-size:16px;}")
        .arg(t.surface().name(), t.borderStrong().name(), t.text2().name()));
    connect(gearBtn, &QPushButton::clicked, this, &MainWindow::openTweaks);
    hl->addWidget(gearBtn);

    // User menu
    auto* userBtn = new QPushButton();
    userBtn->setStyleSheet(QString(
        "QPushButton{background:transparent; border:1px solid transparent; "
        "border-radius:30px; padding:3px 8px 3px 3px; color:%1; font-size:12.5px;}")
        .arg(t.text().name()));
    hl->addWidget(userBtn);

    topLayout->addWidget(bar);
    setupMenuBar(); // menu bar is separate from topbar in QMainWindow
}

// ── Sidebar ─────────────────────────────────────────────────────────
void MainWindow::setupSidebar(QWidget* sidebar) {
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(Theme::instance().sidebarW());
    sidebar->setStyleSheet(Theme::instance().sidebarStyleSheet());

    sidebarLayout_ = new QVBoxLayout(sidebar);
    sidebarLayout_->setContentsMargins(0, 0, 0, 0);
    sidebarLayout_->setSpacing(0);

    // Brand at top of sidebar
    auto* brand = new QLabel("\xF0\x9F\xA7\x8A  FreezerManager");
    brand->setStyleSheet(QString(
        "font-size:14px; font-weight:600; color:%1; background:transparent; "
        "padding:14px 14px 10px;")
        .arg(Theme::instance().text().name()));
    sidebarLayout_->addWidget(brand);

    sidebarLayout_->addSpacing(8);

    // Nav items
    rebuildNav();

    sidebarLayout_->addStretch();

    // Footer
    auto* foot = new QWidget(sidebar);
    foot->setStyleSheet("background:transparent; border-top:1px solid " +
                        Theme::instance().border().name() +
                        "; padding:10px 14px; font-size:11px; color:" +
                        Theme::instance().text3().name() + ";");
    auto* fl = new QVBoxLayout(foot);
    fl->setSpacing(4);
    fl->addWidget(new QLabel("\xF0\x9F\x9F\xA2 Connected · grpc://freezerd:8443"));
    fl->addWidget(new QLabel("\xF0\x9F\x94\x92 Audit chain verified"));
    sidebarLayout_->addWidget(foot);
}

// ── Nav buttons ─────────────────────────────────────────────────────
QPushButton* MainWindow::makeNavButton(const QString& icon,
                                        const QString& label,
                                        int pageIndex, bool adminOnly) {
    auto* btn = new QPushButton(icon + "  " + label);
    btn->setCheckable(true);
    btn->setCursor(Qt::PointingHandCursor);
    if (adminOnly) btn->setVisible(role_ == "LabAdmin");
    connect(btn, &QPushButton::clicked, this, [this, pageIndex]() {
        showPage(pageIndex);
    });
    return btn;
}

void MainWindow::rebuildNav() {
    // Clear old buttons
    sidebarButtons_.clear();
    topTabButtons_.clear();

    // Remove old nav buttons from sidebar layout
    if (sidebarLayout_) {
        // Remove everything except the first 2 widgets (brand + spacer)
        while (sidebarLayout_->count() > 3) {
            auto* item = sidebarLayout_->takeAt(2);
            if (item->widget()) {
                item->widget()->deleteLater();
            }
            delete item;
        }
    }

    // Remove old top tabs (keep the layout — it is reused below)
    if (topTabsLayout_) {
        while (QLayoutItem* item = topTabsLayout_->takeAt(0)) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
    }

    QString currentGroup;
    for (int i = 0; i < kNavCount; ++i) {
        const auto& ni = kNavItems[i];
        bool visible = !ni.adminOnly || role_ == "LabAdmin";

        // Group header (sidebar only)
        if (ni.groupLabel && sidebarLayout_) {
            auto* header = new QLabel(ni.groupLabel);
            header->setProperty("role", "nav-group");
            header->setStyleSheet(QString(
                "font-size:10.5px; text-transform:uppercase; letter-spacing:0.07em; "
                "color:%1; font-weight:600; padding:14px 10px 5px; background:transparent;")
                .arg(Theme::instance().text3().name()));
            sidebarLayout_->addWidget(header);
        }

        // Sidebar button — add to layout BEFORE setVisible (same reason as
        // the top-tab button: parent first, then show).
        auto* sb = makeNavButton(ni.icon, ni.label, static_cast<int>(ni.page), ni.adminOnly);
        if (sidebarLayout_) sidebarLayout_->addWidget(sb);
        sb->setVisible(visible);
        sidebarButtons_.append(sb);

        // Top-tab button — parent into the tabs layout BEFORE setVisible,
        // else a parentless visible widget shows as its own top-level window.
        auto* tb = makeNavButton(ni.icon, ni.label, static_cast<int>(ni.page), ni.adminOnly);
        if (topTabsLayout_) topTabsLayout_->addWidget(tb);
        tb->setVisible(visible);
        topTabButtons_.append(tb);
    }
}

// ── Central widget setup ────────────────────────────────────────────
void MainWindow::setupCentralWidget() {
    centralWidget_ = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(centralWidget_);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    setupTopbar(rootLayout);

    auto* body = new QWidget(centralWidget_);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    // Top-tabs (hidden by default) — created BEFORE the sidebar so that
    // rebuildNav() can parent the top-tab buttons into its layout. Otherwise
    // makeNavButton()'s parentless QPushButtons get setVisible(true) and pop
    // up as stray top-level windows.
    topTabsWidget_ = new QWidget(body);
    topTabsWidget_->setStyleSheet(QString(
        "background:%1; border-bottom:1px solid %2;")
        .arg(Theme::instance().surface().name(),
             Theme::instance().border().name()));
    topTabsLayout_ = new QHBoxLayout(topTabsWidget_);
    topTabsLayout_->setContentsMargins(10, 0, 10, 0);
    topTabsLayout_->setSpacing(2);
    topTabsWidget_->setFixedHeight(40);
    topTabsWidget_->setVisible(false);

    // Sidebar (calls rebuildNav, which now sees a valid topTabsWidget_)
    sidebarWidget_ = new QWidget(body);
    setupSidebar(sidebarWidget_);
    bodyLayout->addWidget(sidebarWidget_);

    // Stacked pages
    pageStack_ = new QStackedWidget(this);
    pageStack_->setStyleSheet("background:transparent;");

    // Create all page placeholders
    pages_.resize(static_cast<int>(PageIndex::Count));

    // Existing pages
    pages_[0] = new pages::DashboardPage(pageStack_);
    pages_[1] = new pages::SampleBrowserPage(pageStack_);
    pages_[2] = new pages::FreezerExplorerPage(pageStack_);

    pages_[3] = new pages::CheckInOutPage(pageStack_);
    pages_[4] = new pages::CsvImportPage(pageStack_);
    pages_[5] = new pages::AuditLogPage(pageStack_);
    pages_[6] = new pages::AdminPage(pageStack_);
    pages_[7] = new pages::ReportsPage(pageStack_);
    pages_[8] = new pages::DonorsPage(pageStack_);
    pages_[9] = new pages::StudiesPage(pageStack_);
    pages_[10] = new pages::MonitoringPage(pageStack_);
    pages_[11] = new pages::PickListsPage(pageStack_);
    pages_[12] = new pages::RequestsPage(pageStack_);

    for (auto* page : pages_) {
        pageStack_->addWidget(page);
    }

    auto* rightPanel = new QWidget(body);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->addWidget(topTabsWidget_);
    rightLayout->addWidget(pageStack_, 1);

    bodyLayout->addWidget(rightPanel, 1);
    rootLayout->addWidget(body, 1);

    setCentralWidget(centralWidget_);
}

// ── Page navigation ─────────────────────────────────────────────────
void MainWindow::showPage(int index) {
    pageStack_->setCurrentIndex(index);

    // Update sidebar highlights
    for (int i = 0; i < sidebarButtons_.size(); ++i) {
        sidebarButtons_[i]->setChecked(i == index);
    }
    for (int i = 0; i < topTabButtons_.size(); ++i) {
        topTabButtons_[i]->setChecked(i == index);
    }
}

void MainWindow::showSampleDetail(const core::Sample& /*sample*/) {
    // Future: open a detail drawer/overlay
}

void MainWindow::navigateToFreezerBox(const QString& /*boxId*/) {
    showPage(static_cast<int>(PageIndex::Freezers));
}

// ── Theme tweaks ────────────────────────────────────────────────────
void MainWindow::setRole(const QString& role) {
    role_ = role;
    rebuildNav();
}

void MainWindow::setNavLayout(const QString& layout) {
    navLayout_ = layout;
    bool useSidebar = (layout == "sidebar");
    sidebarWidget_->setVisible(useSidebar);
    topTabsWidget_->setVisible(!useSidebar);
}

void MainWindow::setDarkMode(bool dark) {
    Theme::instance().setMode(dark ? Mode::Dark : Mode::Light);
}

void MainWindow::setAccent(const QString& hex) {
    Theme::instance().setAccent(hex);
}

void MainWindow::setDensity(const QString& d) {
    Theme::instance().setDensity(
        d == "compact" ? Density::Compact : Density::Comfortable);
}

void MainWindow::openTweaks() {
    // Future: open tweaks panel
}

void MainWindow::onSearchTextChanged(const QString& /*text*/) {
    // Global search — will be wired to SampleBrowser
}

}  // namespace fmgr::qt
