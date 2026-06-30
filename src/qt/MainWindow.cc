// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QString>
#include <QTabWidget>

#include "qt/AuthServiceClient.h"
#include "qt/BarcodeScanController.h"
#include "qt/BarcodeScanWidget.h"
#include "qt/BoxGridModel.h"
#include "qt/BoxGridWidget.h"
#include "qt/BoxMapPdf.h"
#include "qt/BoxServiceClient.h"
#include "qt/LabServiceClient.h"
#include "qt/LabTreeWidget.h"
#include "qt/LabelPdf.h"
#include "qt/LoginDialog.h"
#include "qt/SampleBrowserWidget.h"
#include "qt/SampleLookupWidget.h"
#include "qt/SampleServiceClient.h"
#include "qt/SampleWatchSubscriber.h"

namespace fmgr::qt {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("FreezerManager"));

  pages_ = new QStackedWidget(this);
  // Placeholder landing page, shown before login and after logout/expiry. The
  // authenticated LabTreeWidget page is pushed on top once a session starts.
  auto* placeholder = new QLabel(
      QStringLiteral("FreezerManager desktop client\nConnect to begin."),
      pages_);
  placeholder->setAlignment(::Qt::AlignCenter);
  placeholder_ = placeholder;
  pages_->addWidget(placeholder);
  setCentralWidget(pages_);

  status_label_ = new QLabel(this);
  statusBar()->addPermanentWidget(status_label_);

  buildMenus();

  // Logout and expiry both end the session; return to the placeholder page.
  connect(&session_, &SessionManager::sessionEnded, this,
          &MainWindow::showPlaceholder);

  const QByteArray geometry = config_.windowGeometry();
  if (!geometry.isEmpty()) {
    restoreGeometry(geometry);
  } else {
    resize(1024, 720);
  }

  updateStatus();

  if (config_.autoConnect()) {
    onConnect();
  }
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus() {
  QMenu* file_menu = menuBar()->addMenu(QStringLiteral("&File"));

  QAction* connect_action = file_menu->addAction(QStringLiteral("&Connect"));
  connect(connect_action, &QAction::triggered, this, &MainWindow::onConnect);

  logout_action_ = file_menu->addAction(QStringLiteral("&Log out"));
  logout_action_->setEnabled(false);
  connect(logout_action_, &QAction::triggered, this, &MainWindow::onLogout);

  file_menu->addSeparator();

  // Global shortcut to jump to the single-handed lookup input. Lives on the menu
  // so it fires regardless of which page/tab holds focus.
  QAction* lookup_action =
      file_menu->addAction(QStringLiteral("Sample &Lookup"));
  lookup_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
  connect(lookup_action, &QAction::triggered, this, &MainWindow::focusLookup);

  file_menu->addSeparator();

  QAction* quit_action = file_menu->addAction(QStringLiteral("&Quit"));
  quit_action->setShortcut(QKeySequence::Quit);
  connect(quit_action, &QAction::triggered, this, &MainWindow::onQuit);
}

void MainWindow::onConnect() {
  channel_ = GrpcChannel(config_.serverUrl().toStdString());
  channel_.connect();
  updateStatus();

  auth_ = std::make_unique<AuthServiceClient>(channel_.makeAuthStub());
  if (runLoginFlow()) {
    showAuthenticated();
  }
}

bool MainWindow::runLoginFlow() {
  LoginDialog dlg(this);
  for (;;) {
    if (dlg.exec() != QDialog::Accepted) {
      // Cancelled. Drop any half-started (pre-MFA) session.
      if (session_.hasToken()) {
        session_.clear();
      }
      return false;
    }

    if (!session_.hasToken()) {
      // First stage: email + password.
      const auto result = auth_->login(dlg.email(), dlg.password());
      if (!result.ok) {
        dlg.setError(QString::fromStdString(result.error));
        continue;
      }
      session_.startSession(result.session_token, result.session_id,
                            result.user_id, result.mfa_required);
      if (!result.mfa_required) {
        return true;
      }
      dlg.showTotpField(
          QStringLiteral("Enter the 6-digit authenticator code."));
      continue;
    }

    // Second stage: a session token is held; complete MFA.
    const auto result = auth_->submitMfa(session_.token(), dlg.totpCode());
    if (!result.ok) {
      dlg.setError(QString::fromStdString(result.error));
      continue;
    }
    session_.markMfaSatisfied();
    return true;
  }
}

void MainWindow::showAuthenticated() {
  lab_client_ = std::make_unique<LabServiceClient>(channel_.makeLabStub());
  box_client_ = std::make_unique<BoxServiceClient>(channel_.makeBoxStub());
  sample_client_ =
      std::make_unique<SampleServiceClient>(channel_.makeSampleStub());
  // Live sample feed rides its own stub (a server-stream must not share the
  // unary client's blocking calls). Token set here; scope is driven per
  // selection in onNodeSelected via the browser.
  watch_ = std::make_unique<SampleWatchSubscriber>(channel_.makeSampleStub());
  watch_->setToken(session_.token());

  // Recreate the page so it binds to the current clients (a re-login mints new
  // stubs).
  if (auth_page_ != nullptr) {
    pages_->removeWidget(auth_page_);
    delete auth_page_;
  }
  auto* splitter = new QSplitter(::Qt::Horizontal);
  tree_ = new LabTreeWidget(lab_client_.get(), box_client_.get());

  // Right pane: a tab over the sample table and the box layout grid.
  browser_ = new SampleBrowserWidget(sample_client_.get());
  grid_model_ =
      std::make_unique<BoxGridModel>(box_client_.get(), sample_client_.get());
  grid_model_->setToken(session_.token());
  auto* box_map = new BoxMapPdf(box_client_.get(), sample_client_.get());
  auto* label_pdf = new LabelPdf(sample_client_.get());
  box_map_.reset(box_map);
  label_pdf_.reset(label_pdf);
  grid_ = new BoxGridWidget(grid_model_.get(), box_map, label_pdf);
  scan_controller_ =
      std::make_unique<BarcodeScanController>(sample_client_.get());
  scan_controller_->setToken(session_.token());
  scan_ = new BarcodeScanWidget(scan_controller_.get());
  // Single-handed lookup is the primary landing tab (PRD §9 #1 daily op).
  lookup_ = new SampleLookupWidget(sample_client_.get(), box_client_.get());
  lookup_->setToken(session_.token());
  auto* tabs = new QTabWidget;
  tabs->addTab(lookup_, QStringLiteral("Lookup"));
  tabs->addTab(browser_, QStringLiteral("Samples"));
  tabs->addTab(grid_, QStringLiteral("Box Layout"));
  tabs->addTab(scan_, QStringLiteral("Scan"));

  splitter->addWidget(tree_);
  splitter->addWidget(tabs);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 3);
  auth_page_ = splitter;

  connect(tree_, &LabTreeWidget::nodeSelected, this,
          &MainWindow::onNodeSelected);

  tree_->setToken(session_.token());
  browser_->setToken(session_.token());
  browser_->setWatchSubscriber(watch_.get());
  tree_->reload();

  pages_->addWidget(auth_page_);
  pages_->setCurrentWidget(auth_page_);

  if (logout_action_ != nullptr) {
    logout_action_->setEnabled(true);
  }
}

void MainWindow::onNodeSelected(const QString& kind, const QString& id,
                                const QString& lab_id) {
  if (browser_ == nullptr || lab_id.isEmpty()) {
    return;
  }
  // Scope the barcode scanner to the selected lab too.
  if (scan_controller_ != nullptr) {
    scan_controller_->setScope(lab_id);
  }
  // Scope the single-handed lookup to the selected lab.
  if (lookup_ != nullptr) {
    lookup_->setScope(lab_id);
  }
  if (kind == QStringLiteral("box")) {
    browser_->setScope(lab_id, id);
    if (grid_model_ != nullptr) {
      grid_model_->setBox(lab_id, id);
    }
    // Surface the box layout (the grid lives next to the table in the right
    // pane's tab).
    if (grid_ != nullptr) {
      auto* tabs = qobject_cast<QTabWidget*>(grid_->parentWidget());
      if (tabs != nullptr) {
        tabs->setCurrentWidget(grid_);
      }
    }
  } else {
    // lab / freezer / container → lab-wide (ListSamples scopes by box, not
    // container).
    browser_->setScope(lab_id);
  }
}

void MainWindow::showPlaceholder() {
  // Stop the live feed (joins its worker thread) before the browser that
  // listens to it is destroyed.
  watch_.reset();
  if (auth_page_ != nullptr) {
    pages_->removeWidget(auth_page_);
    delete auth_page_;
    auth_page_ = nullptr;
    tree_ = nullptr;
    browser_ = nullptr;
    lookup_ = nullptr;
    grid_ = nullptr;
    scan_ = nullptr;
    grid_model_.reset();
    scan_controller_.reset();
    box_map_.reset();
    label_pdf_.reset();
  }
  if (placeholder_ != nullptr) {
    pages_->setCurrentWidget(placeholder_);
  }
  if (logout_action_ != nullptr) {
    logout_action_->setEnabled(false);
  }
}

void MainWindow::focusLookup() {
  if (lookup_ == nullptr) {
    return;
  }
  // Surface the lookup tab and hand it keyboard focus.
  auto* tabs = qobject_cast<QTabWidget*>(lookup_->parentWidget());
  if (tabs != nullptr) {
    tabs->setCurrentWidget(lookup_);
  }
  lookup_->setFocus();
}

void MainWindow::onLogout() {
  if (session_.hasToken() && auth_ != nullptr) {
    auth_->logout(session_.token());
  }
  // Emits sessionEnded(), which is wired to showPlaceholder().
  session_.clear();
}

void MainWindow::onQuit() { close(); }

void MainWindow::updateStatus() {
  if (channel_.isConnected()) {
    status_label_->setText(QStringLiteral("Connected: %1")
                               .arg(QString::fromStdString(channel_.target())));
  } else {
    status_label_->setText(QStringLiteral("Not connected"));
  }
}

void MainWindow::closeEvent(QCloseEvent* event) {
  config_.setWindowGeometry(saveGeometry());
  config_.sync();
  event->accept();
}

}  // namespace fmgr::qt
