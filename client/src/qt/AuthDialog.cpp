#include "AuthDialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

AuthDialog::AuthDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("NexTalk sign in");
    setModal(true);
    resize(360, 240);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    hostEdit_ = new QLineEdit("127.0.0.1", this);
    portSpin_ = new QSpinBox(this);
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(5555);
    usernameEdit_ = new QLineEdit(this);
    usernameEdit_->setPlaceholderText("alice");
    passwordEdit_ = new QLineEdit(this);
    passwordEdit_->setEchoMode(QLineEdit::Password);
    passwordEdit_->setPlaceholderText("at least 4 characters");
    createAccountCheck_ = new QCheckBox("Create new account", this);

    form->addRow("Host", hostEdit_);
    form->addRow("Port", portSpin_);
    form->addRow("Username", usernameEdit_);
    form->addRow("Password", passwordEdit_);
    form->addRow("", createAccountCheck_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText("Continue");

    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (usernameEdit_->text().trimmed().isEmpty() ||
            passwordEdit_->text().isEmpty()) {
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    layout->addLayout(form);
    layout->addWidget(buttons);
}

AuthData AuthDialog::authData() const {
    return {
        hostEdit_->text().trimmed(),
        portSpin_->value(),
        usernameEdit_->text().trimmed(),
        passwordEdit_->text(),
        createAccountCheck_->isChecked()
    };
}
