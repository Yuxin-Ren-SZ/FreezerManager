// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_MAINWINDOW_H
#define FMGR_QT_MAINWINDOW_H

#include <memory>

#include <QMainWindow>

#include "qt/ConfigManager.h"
#include "qt/GrpcChannel.h"
#include "qt/SessionManager.h"

class QAction;
class QLabel;
class QStackedWidget;
class QWidget;

namespace fmgr::qt {

class AuthServiceClient;
class LabServiceClient;
class BoxServiceClient;
class SampleServiceClient;
class LabTreeWidget;
class SampleBrowserWidget;

// Application shell: menu bar, central stacked widget (one page per module), and
// a status bar showing connection state. Owns the ConfigManager, GrpcChannel,
// SessionManager and the service clients, and drives the login flow: Connect →
// LoginDialog → AuthService → SessionManager → swap the stacked page from the
// placeholder to the authenticated LabTreeWidget. Session end (logout or expiry)
// swaps back to the placeholder.
class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  QStackedWidget* pages() const { return pages_; }

 protected:
  void closeEvent(QCloseEvent* event) override;

 private slots:
  void onConnect();
  void onLogout();
  void onQuit();
  // Map a tree selection to a sample-browser scope.
  void onNodeSelected(const QString& kind, const QString& id,
                      const QString& lab_id);

 private:
  void buildMenus();
  void updateStatus();

  // Run the modal login (+ optional MFA) loop. Returns true once the session is
  // authenticated; false if the user cancelled.
  bool runLoginFlow();
  // Build the service clients + tree page and show it.
  void showAuthenticated();
  // Tear down the tree page and return to the placeholder. Slot for
  // SessionManager::sessionEnded (logout / expiry).
  void showPlaceholder();

  ConfigManager config_;
  GrpcChannel channel_{};
  SessionManager session_;

  std::unique_ptr<AuthServiceClient> auth_;
  std::unique_ptr<LabServiceClient> lab_client_;
  std::unique_ptr<BoxServiceClient> box_client_;
  std::unique_ptr<SampleServiceClient> sample_client_;

  QStackedWidget* pages_ = nullptr;
  QWidget* placeholder_ = nullptr;
  QWidget* auth_page_ = nullptr;  // splitter: tree | browser
  LabTreeWidget* tree_ = nullptr;
  SampleBrowserWidget* browser_ = nullptr;
  QLabel* status_label_ = nullptr;
  QAction* logout_action_ = nullptr;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_MAINWINDOW_H
