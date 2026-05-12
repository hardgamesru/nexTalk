#pragma once

#include <memory>

#include <QMainWindow>

#include "client/ClientConnection.hpp"

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;
class QTextEdit;
class QTimer;
class QDialog;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void buildUi();
    void wireUi();
    void installConnectionCallbacks();

    void showAuthDialog();
    void connectWithAuth(const QString& host,
                         int port,
                         const QString& username,
                         const QString& password,
                         bool createAccount);
    void disconnectFromServer();
    void sendMessage();
    void requestHistory();
    void selectPeer(const QString& peer);
    void rememberPeer(const QString& peer);
    void updateChatPreview(const client::ChatSummary& chat);
    void installChatItemWidget(QListWidgetItem* item);
    void confirmDeleteChat(const QString& peer);
    void removeChatItem(const QString& peer);
    void filterChatList();
    QListWidgetItem* firstVisibleChatItem() const;
    void openAddChatDialog();
    void scheduleAddChatSearch();
    void runAddChatSearch();
    void addUserSearchResult(const QString& query, const client::UserSearchItem& user);
    void finishUserSearch(const QString& status, const QString& text, const QString& query);
    void createSelectedChat();
    QString selectedAddChatUserId() const;
    void clearTranscriptForPeer(const QString& peer);
    void resetSessionUi();
    void appendSystemMessage(const QString& text);
    void appendChatMessage(const QString& sender, const QString& text, const QString& createdAt = {});
    void appendHistoryMessage(const client::HistoryItem& item);
    QString renderMessageHtml(const QString& sender,
                              const QString& text,
                              const QString& createdAt) const;
    QString currentPeer() const;
    void setConnectedState(bool connected);
    void setConnectingState(bool connecting);
    void updateChatActions();

    QListWidget* chatList_{nullptr};
    QLineEdit* peerEdit_{nullptr};
    QPushButton* addChatButton_{nullptr};
    QSpinBox* historyLimitSpin_{nullptr};
    QPushButton* historyButton_{nullptr};
    QLabel* chatStatusLabel_{nullptr};
    QLabel* profileLabel_{nullptr};
    QPushButton* accountButton_{nullptr};
    QPushButton* chatsButton_{nullptr};
    QPushButton* settingsButton_{nullptr};
    QLabel* chatTitleLabel_{nullptr};
    QTextEdit* systemLog_{nullptr};
    QTextEdit* transcript_{nullptr};
    QLineEdit* messageEdit_{nullptr};
    QPushButton* sendButton_{nullptr};
    QDialog* addChatDialog_{nullptr};
    QLineEdit* addChatSearchEdit_{nullptr};
    QListWidget* addChatResults_{nullptr};
    QPushButton* addChatCreateButton_{nullptr};
    QTimer* addChatSearchTimer_{nullptr};

    std::unique_ptr<client::ClientConnection> connection_;
    QString currentUsername_;
    QString pendingPassword_;
    QString selectedPeer_;
    QString selectedPeerId_;
    QString pendingAddChatSearchQuery_;
    bool pendingCreateAccount_{false};
    bool connected_{false};
    bool connecting_{false};
    bool manualDisconnect_{false};
    bool addChatHasResults_{false};
};
