// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_QT_LOGINDIALOG_H
#define FMGR_QT_LOGINDIALOG_H

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;

namespace fmgr::qt {

// Modal credential prompt: email + password, plus a TOTP field that is hidden
// until the server reports MFA is required (showTotpField()). It only gathers
// and validates input; the caller drives AuthServiceClient and decides whether
// to reveal the TOTP step.
class LoginDialog : public QDialog {
  Q_OBJECT

 public:
  struct Validation {
    bool valid = false;
    QString error;  // empty when valid
  };

  // Pure form-validation rule, separated from the widgets so it is unit-testable
  // without a QApplication. Requires a plausible email (non-empty, contains '@')
  // and a non-empty password; when totp_required, totp must be exactly 6 digits.
  static Validation validate(const QString& email, const QString& password,
                             const QString& totp, bool totp_required);

  explicit LoginDialog(QWidget* parent = nullptr);

  QString email() const;
  QString password() const;
  QString totpCode() const;

  bool totpVisible() const { return totp_visible_; }

  // Reveal the TOTP field (after Login returns mfa_required) and surface an
  // optional message, e.g. "Enter the 6-digit code from your authenticator".
  void showTotpField(const QString& message = QString());

  // Display a server-side error (e.g. failed login) above the buttons.
  void setError(const QString& message);

 private slots:
  // Re-run validation and enable/disable the submit button accordingly.
  void revalidate();

 private:
  QLineEdit* email_edit_ = nullptr;
  QLineEdit* password_edit_ = nullptr;
  QLineEdit* totp_edit_ = nullptr;
  QLabel* totp_label_ = nullptr;
  QLabel* error_label_ = nullptr;
  QPushButton* submit_button_ = nullptr;
  bool totp_visible_ = false;
};

}  // namespace fmgr::qt

#endif  // FMGR_QT_LOGINDIALOG_H
