// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/SessionManager.h"

#include <algorithm>
#include <utility>

#include <QTimer>

namespace fmgr::qt {

SessionManager::SessionManager(QObject* parent)
    : QObject(parent),
      clock_([] { return std::chrono::steady_clock::now(); }),
      logout_timer_(new QTimer(this)) {
  logout_timer_->setSingleShot(true);
  connect(logout_timer_, &QTimer::timeout, this,
          &SessionManager::handleTimeout);
}

SessionManager::~SessionManager() = default;

void SessionManager::startSession(const QString& token,
                                  const QString& session_id,
                                  const QString& user_id, bool mfa_required,
                                  std::chrono::seconds ttl) {
  token_ = token;
  session_id_ = session_id;
  user_id_ = user_id;
  mfa_required_ = mfa_required;
  mfa_satisfied_ = false;
  expiry_ = clock_() + ttl;

  logout_timer_->stop();
  // QTimer takes milliseconds; clamp negative/zero TTLs to fire immediately.
  const auto ms = std::max<std::chrono::milliseconds::rep>(
      0, std::chrono::duration_cast<std::chrono::milliseconds>(ttl).count());
  logout_timer_->start(static_cast<int>(ms));

  emit sessionStarted();
}

void SessionManager::markMfaSatisfied() {
  if (mfa_satisfied_) {
    return;
  }
  mfa_satisfied_ = true;
  emit mfaSatisfiedChanged();
}

void SessionManager::clear() {
  logout_timer_->stop();
  const bool had_token = !token_.isEmpty();

  token_.clear();
  session_id_.clear();
  user_id_.clear();
  mfa_required_ = false;
  mfa_satisfied_ = false;
  expiry_ = {};

  if (had_token) {
    emit sessionEnded();
  }
}

bool SessionManager::isExpired() const {
  if (token_.isEmpty()) {
    return false;
  }
  return clock_() >= expiry_;
}

bool SessionManager::isAuthenticated() const {
  if (token_.isEmpty() || isExpired()) {
    return false;
  }
  return !mfa_required_ || mfa_satisfied_;
}

std::chrono::seconds SessionManager::remaining() const {
  if (token_.isEmpty()) {
    return std::chrono::seconds(0);
  }
  const auto left =
      std::chrono::duration_cast<std::chrono::seconds>(expiry_ - clock_());
  return std::max(std::chrono::seconds(0), left);
}

void SessionManager::handleTimeout() {
  // Timer reached the deadline; tear the session down like an explicit logout.
  clear();
}

}  // namespace fmgr::qt
