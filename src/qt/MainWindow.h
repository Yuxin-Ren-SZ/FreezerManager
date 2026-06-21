// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_MAINWINDOW_H
#define FMGR_QT_MAINWINDOW_H

#include <memory>

#include <QMainWindow>

#include "qt/ConfigManager.h"
#include "qt/GrpcChannel.h"

class QLabel;
class QStackedWidget;

namespace fmgr::qt {

// Application shell: menu bar, central stacked widget (one page per future
// module), and a status bar showing connection state. Owns the ConfigManager
// and GrpcChannel so child pages can be wired to them in later modules.
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
  void onQuit();

 private:
  void buildMenus();
  void updateStatus();

  ConfigManager config_;
  GrpcChannel channel_{};

  QStackedWidget* pages_ = nullptr;
  QLabel* status_label_ = nullptr;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_MAINWINDOW_H
