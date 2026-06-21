// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/ConfigManager.h"

namespace fmgr::qt {
namespace {

constexpr auto kServerUrlKey = "server/url";
constexpr auto kAutoConnectKey = "server/auto_connect";
constexpr auto kWindowGeometryKey = "window/geometry";

constexpr auto kDefaultServerUrl = "0.0.0.0:50051";

constexpr auto kOrganization = "FreezerManager";
constexpr auto kApplication = "freezermanager-qt";

}  // namespace

ConfigManager::ConfigManager(const QString& ini_path) {
  if (ini_path.isEmpty()) {
    settings_ = std::make_unique<QSettings>(QSettings::UserScope,
                                            QLatin1String(kOrganization),
                                            QLatin1String(kApplication));
  } else {
    settings_ =
        std::make_unique<QSettings>(ini_path, QSettings::IniFormat);
  }
}

QString ConfigManager::serverUrl() const {
  return settings_
      ->value(QLatin1String(kServerUrlKey), QLatin1String(kDefaultServerUrl))
      .toString();
}

void ConfigManager::setServerUrl(const QString& url) {
  settings_->setValue(QLatin1String(kServerUrlKey), url);
}

bool ConfigManager::autoConnect() const {
  return settings_->value(QLatin1String(kAutoConnectKey), false).toBool();
}

void ConfigManager::setAutoConnect(bool enabled) {
  settings_->setValue(QLatin1String(kAutoConnectKey), enabled);
}

QByteArray ConfigManager::windowGeometry() const {
  return settings_->value(QLatin1String(kWindowGeometryKey), QByteArray())
      .toByteArray();
}

void ConfigManager::setWindowGeometry(const QByteArray& geometry) {
  settings_->setValue(QLatin1String(kWindowGeometryKey), geometry);
}

void ConfigManager::sync() { settings_->sync(); }

}  // namespace fmgr::qt
