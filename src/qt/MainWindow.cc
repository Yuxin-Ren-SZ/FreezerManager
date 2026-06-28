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
#include <QStackedWidget>
#include <QStatusBar>
#include <QString>

#include "qt/AuthServiceClient.h"
#include "qt/BoxServiceClient.h"
#include "qt/LabServiceClient.h"
#include "qt/LabTreeWidget.h"
#include "qt/LoginDialog.h"

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

  // Recreate the page so it binds to the current clients (a re-login mints new
  // stubs).
  if (tree_page_ != nullptr) {
    pages_->removeWidget(tree_page_);
    delete tree_page_;
  }
  tree_page_ = new LabTreeWidget(lab_client_.get(), box_client_.get());
  pages_->addWidget(tree_page_);
  tree_page_->setToken(session_.token());
  tree_page_->reload();
  pages_->setCurrentWidget(tree_page_);

  if (logout_action_ != nullptr) {
    logout_action_->setEnabled(true);
  }
}

void MainWindow::showPlaceholder() {
  if (tree_page_ != nullptr) {
    pages_->removeWidget(tree_page_);
    delete tree_page_;
    tree_page_ = nullptr;
  }
  if (placeholder_ != nullptr) {
    pages_->setCurrentWidget(placeholder_);
  }
  if (logout_action_ != nullptr) {
    logout_action_->setEnabled(false);
  }
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
