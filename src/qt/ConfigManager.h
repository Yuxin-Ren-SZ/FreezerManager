// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_CONFIGMANAGER_H
#define FMGR_QT_CONFIGMANAGER_H

#include <memory>

#include <QByteArray>
#include <QSettings>
#include <QString>

namespace fmgr::qt {

// Thin typed wrapper over QSettings for the desktop client's persisted
// preferences: server endpoint, auto-connect toggle, and main-window geometry.
//
// Pass an explicit INI file path (used by tests for an isolated, throwaway
// store); the default constructor uses the platform-native user scope keyed by
// the FreezerManager organization/application names.
class ConfigManager {
 public:
  explicit ConfigManager(const QString& ini_path = QString());

  QString serverUrl() const;
  void setServerUrl(const QString& url);

  bool autoConnect() const;
  void setAutoConnect(bool enabled);

  QByteArray windowGeometry() const;
  void setWindowGeometry(const QByteArray& geometry);

  // Flush pending writes to the backing store. QSettings also syncs on
  // destruction, but call this when persistence must be observable immediately.
  void sync();

 private:
  std::unique_ptr<QSettings> settings_;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_CONFIGMANAGER_H
