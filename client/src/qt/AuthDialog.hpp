#pragma once

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QSpinBox;

struct AuthData {
    QString host;
    int port{5555};
    QString username;
    QString password;
    bool createAccount{false};
};

class AuthDialog : public QDialog {
public:
    explicit AuthDialog(QWidget* parent = nullptr);

    AuthData authData() const;

private:
    QLineEdit* hostEdit_{nullptr};
    QSpinBox* portSpin_{nullptr};
    QLineEdit* usernameEdit_{nullptr};
    QLineEdit* passwordEdit_{nullptr};
    QCheckBox* createAccountCheck_{nullptr};
};
