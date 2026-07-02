// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_SESSIONMANAGER_H
#define FMGR_QT_SESSIONMANAGER_H

#include <chrono>
#include <functional>

#include <QObject>
#include <QString>

class QTimer;

namespace fmgr::qt {

// Holds the authenticated session minted by AuthServiceClient::login: the bearer
// token plus the server-side session/user ids, and tracks the local expiry
// deadline so the UI can auto-logout an idle session.
//
// The server does not return an explicit expiry in LoginResponse, so the TTL is
// supplied by the caller (a client-side policy) and counted from startSession.
// A single-shot QTimer fires sessionEnded() when the deadline passes; expiry can
// also be queried synchronously via isExpired(). Tests inject a clock to drive
// expiry without an event loop.
//
// QObject (signals/auto-logout timer) but usable headless: signal emission and
// isExpired() need no running event loop; only the QTimer-driven auto-logout
// does, which the GUI provides.
class SessionManager : public QObject {
  Q_OBJECT

 public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  static constexpr std::chrono::seconds kDefaultTtl = std::chrono::minutes(30);

  explicit SessionManager(QObject* parent = nullptr);
  ~SessionManager() override;

  // Begin a session. mfa_required mirrors LoginResponse.mfa_required: while true
  // and not yet satisfied, isAuthenticated() stays false. Resets the auto-logout
  // timer to ttl from now.
  void startSession(const QString& token, const QString& session_id,
                    const QString& user_id, bool mfa_required,
                    std::chrono::seconds ttl = kDefaultTtl);

  // Mark the MFA step complete (after AuthServiceClient::submitMfa succeeds).
  void markMfaSatisfied();

  // Drop all session state and stop the timer. Emits sessionEnded() if a token
  // was held. Idempotent.
  void clear();

  bool hasToken() const { return !token_.isEmpty(); }
  bool isExpired() const;
  // True only when a token is held, not expired, and MFA (if required) is done.
  bool isAuthenticated() const;
  bool mfaRequired() const { return mfa_required_; }
  bool mfaSatisfied() const { return mfa_satisfied_; }

  const QString& token() const { return token_; }
  const QString& sessionId() const { return session_id_; }
  const QString& userId() const { return user_id_; }

  // Seconds until expiry (clamped at 0). Zero when no session is held.
  std::chrono::seconds remaining() const;

  // Test seam: override the clock used for expiry checks. The auto-logout QTimer
  // still uses real time.
  void setClock(Clock clock) { clock_ = std::move(clock); }

 signals:
  void sessionStarted();
  void mfaSatisfiedChanged();
  // Emitted on logout (clear) and on expiry.
  void sessionEnded();

 private:
  void handleTimeout();

  Clock clock_;
  QString token_;
  QString session_id_;
  QString user_id_;
  bool mfa_required_ = false;
  bool mfa_satisfied_ = false;
  std::chrono::steady_clock::time_point expiry_{};
  QTimer* logout_timer_ = nullptr;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_SESSIONMANAGER_H
