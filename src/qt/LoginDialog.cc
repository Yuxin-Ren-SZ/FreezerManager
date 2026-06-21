// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/LoginDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace fmgr::qt {

LoginDialog::Validation LoginDialog::validate(const QString& email,
                                              const QString& password,
                                              const QString& totp,
                                              bool totp_required) {
  // A plausible email: non-empty and contains '@' with text on both sides. Full
  // RFC validation lives on the server; this only catches obvious mistakes.
  const int at = email.indexOf(QLatin1Char('@'));
  if (email.isEmpty() || at <= 0 || at == email.size() - 1) {
    return {false, QStringLiteral("Enter a valid email address.")};
  }
  if (password.isEmpty()) {
    return {false, QStringLiteral("Password is required.")};
  }
  if (totp_required) {
    static const QRegularExpression kSixDigits(QStringLiteral("^[0-9]{6}$"));
    if (!kSixDigits.match(totp).hasMatch()) {
      return {false, QStringLiteral("Enter the 6-digit authenticator code.")};
    }
  }
  return {true, QString()};
}

LoginDialog::LoginDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("Sign in — FreezerManager"));

  auto* form = new QFormLayout;
  email_edit_ = new QLineEdit(this);
  email_edit_->setPlaceholderText(QStringLiteral("you@lab.example"));
  password_edit_ = new QLineEdit(this);
  password_edit_->setEchoMode(QLineEdit::Password);

  totp_label_ = new QLabel(QStringLiteral("Authenticator code"), this);
  totp_edit_ = new QLineEdit(this);
  totp_edit_->setPlaceholderText(QStringLiteral("123456"));
  totp_edit_->setMaxLength(6);
  // Hidden until the server reports MFA is required.
  totp_label_->setVisible(false);
  totp_edit_->setVisible(false);

  form->addRow(QStringLiteral("Email"), email_edit_);
  form->addRow(QStringLiteral("Password"), password_edit_);
  form->addRow(totp_label_, totp_edit_);

  error_label_ = new QLabel(this);
  error_label_->setWordWrap(true);
  error_label_->setStyleSheet(QStringLiteral("color: #b00020;"));
  error_label_->setVisible(false);

  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                           this);
  submit_button_ = buttons->button(QDialogButtonBox::Ok);
  submit_button_->setText(QStringLiteral("Sign in"));
  submit_button_->setDefault(true);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto* layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(error_label_);
  layout->addWidget(buttons);

  connect(email_edit_, &QLineEdit::textChanged, this,
          &LoginDialog::revalidate);
  connect(password_edit_, &QLineEdit::textChanged, this,
          &LoginDialog::revalidate);
  connect(totp_edit_, &QLineEdit::textChanged, this,
          &LoginDialog::revalidate);

  revalidate();
}

QString LoginDialog::email() const { return email_edit_->text().trimmed(); }

QString LoginDialog::password() const { return password_edit_->text(); }

QString LoginDialog::totpCode() const { return totp_edit_->text().trimmed(); }

void LoginDialog::showTotpField(const QString& message) {
  totp_visible_ = true;
  totp_label_->setVisible(true);
  totp_edit_->setVisible(true);
  if (!message.isEmpty()) {
    setError(message);
  }
  totp_edit_->setFocus();
  revalidate();
}

void LoginDialog::setError(const QString& message) {
  error_label_->setText(message);
  error_label_->setVisible(!message.isEmpty());
}

void LoginDialog::revalidate() {
  const Validation v = validate(email_edit_->text().trimmed(),
                                password_edit_->text(),
                                totp_edit_->text().trimmed(), totp_visible_);
  if (submit_button_) {
    submit_button_->setEnabled(v.valid);
  }
}

}  // namespace fmgr::qt
