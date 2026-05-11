#pragma once

#include <memory>
#include <string>

#include <QMainWindow>

#include "client/ClientConnection.hpp"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTextEdit;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void buildUi();
    void wireUi();
    void installConnectionCallbacks();

    void connectToServer();
    void sendMessage();
    void requestHistory();
    void appendSystemMessage(const QString& text);
    void appendChatMessage(const QString& sender, const QString& text);
    void appendHistoryMessage(const client::HistoryItem& item);
    QString currentPeer() const;
    void setConnectedState(bool connected);

    QLineEdit* hostEdit_{nullptr};
    QSpinBox* portSpin_{nullptr};
    QLineEdit* loginEdit_{nullptr};
    QPushButton* connectButton_{nullptr};
    QLabel* statusLabel_{nullptr};

    QListWidget* chatList_{nullptr};
    QLineEdit* peerEdit_{nullptr};
    QPushButton* historyButton_{nullptr};
    QTextEdit* transcript_{nullptr};
    QLineEdit* messageEdit_{nullptr};
    QPushButton* sendButton_{nullptr};

    std::unique_ptr<client::ClientConnection> connection_;
    bool connected_{false};
};
