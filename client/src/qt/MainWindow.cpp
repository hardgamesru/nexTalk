#include "MainWindow.hpp"

#include "AuthDialog.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QSpinBox>
#include <QTextEdit>
#include <QToolButton>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace {
    constexpr int kItemKindRole = Qt::UserRole;
    constexpr int kItemPeerRole = Qt::UserRole + 1;
    constexpr int kItemUserIdRole = Qt::UserRole + 2;
    constexpr int kItemPreviewRole = Qt::UserRole + 3;
    constexpr int kChatItem = 1;
    constexpr int kUserSearchItem = 2;
    constexpr int kStatusItem = 3;

    bool isValidSearchQuery(const QString& query) {
        if (query.isEmpty() || query.size() > 32) {
            return false;
        }

        for (const QChar ch : query) {
            const ushort code = ch.unicode();
            const bool allowed = (code >= 'a' && code <= 'z') ||
                                 (code >= 'A' && code <= 'Z') ||
                                 (code >= '0' && code <= '9') ||
                                 code == '_' ||
                                 code == '-';
            if (!allowed) {
                return false;
            }
        }

        return true;
    }

    QListWidgetItem* makeStatusItem(const QString& text) {
        auto* item = new QListWidgetItem(text);
        item->setData(kItemKindRole, kStatusItem);
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        return item;
    }

    QString htmlText(const QString& text) {
        QString escaped = text.toHtmlEscaped();
        escaped.replace('\n', "<br>");
        return escaped;
    }

    QString avatarColor(const QString& name) {
        static const QStringList colors = {
            "#2da7ff", "#1fbf75", "#f59e0b", "#ef4444", "#8b5cf6", "#14b8a6"
        };
        return colors.at(static_cast<int>(qHash(name) % colors.size()));
    }

    QString timePart(const QString& createdAt) {
        if (createdAt.isEmpty()) {
            return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm");
        }

        const QDateTime parsed = QDateTime::fromString(createdAt, "yyyy-MM-dd HH:mm:ss");
        if (parsed.isValid()) {
            return parsed.toString("yyyy-MM-dd HH:mm");
        }

        return createdAt;
    }
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      connection_(std::make_unique<client::ClientConnection>()) {
    buildUi();
    wireUi();
    installConnectionCallbacks();
    setConnectedState(false);

    QTimer::singleShot(0, this, [this] {
        showAuthDialog();
    });
}

MainWindow::~MainWindow() {
    connection_->stop();
}

void MainWindow::buildUi() {
    auto* root = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* sidebar = new QFrame(root);
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(340);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(14, 14, 14, 14);
    sidebarLayout->setSpacing(10);

    peerEdit_ = new QLineEdit(sidebar);
    peerEdit_->setPlaceholderText("Search chats");
    peerEdit_->setObjectName("search");

    chatList_ = new QListWidget(sidebar);
    chatList_->setObjectName("chatList");

    historyLimitSpin_ = new QSpinBox(sidebar);
    historyLimitSpin_->setRange(1, 100);
    historyLimitSpin_->setValue(50);

    historyButton_ = new QPushButton("Load history", sidebar);
    addChatButton_ = new QPushButton("New chat", sidebar);
    addChatButton_->setObjectName("addChatButton");
    chatStatusLabel_ = new QLabel("Sign in to start", sidebar);
    chatStatusLabel_->setObjectName("muted");

    sidebarLayout->addWidget(peerEdit_);
    sidebarLayout->addWidget(chatList_, 1);
    sidebarLayout->addWidget(new QLabel("History limit", sidebar));
    sidebarLayout->addWidget(historyLimitSpin_);
    auto* chatActionLayout = new QHBoxLayout();
    chatActionLayout->addWidget(historyButton_, 1);
    chatActionLayout->addWidget(addChatButton_);
    sidebarLayout->addLayout(chatActionLayout);
    sidebarLayout->addWidget(chatStatusLabel_);

    auto* bottomNav = new QHBoxLayout();
    profileLabel_ = new QLabel("Not signed in", sidebar);
    profileLabel_->setObjectName("accountStatus");
    accountButton_ = new QPushButton("Profile", sidebar);
    chatsButton_ = new QPushButton("Chats", sidebar);
    settingsButton_ = new QPushButton("Settings", sidebar);
    sidebarLayout->addWidget(profileLabel_);
    bottomNav->addWidget(accountButton_);
    bottomNav->addWidget(chatsButton_);
    bottomNav->addWidget(settingsButton_);
    sidebarLayout->addLayout(bottomNav);

    auto* content = new QWidget(root);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    auto* header = new QFrame(content);
    header->setObjectName("chatHeader");
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(18, 14, 18, 14);
    chatTitleLabel_ = new QLabel("NexTalk", header);
    chatTitleLabel_->setObjectName("chatTitle");
    headerLayout->addWidget(chatTitleLabel_);

    systemLog_ = new QTextEdit(content);
    systemLog_->setObjectName("systemLog");
    systemLog_->setReadOnly(true);
    systemLog_->setAcceptRichText(false);
    systemLog_->setFixedHeight(110);

    transcript_ = new QTextEdit(content);
    transcript_->setObjectName("transcript");
    transcript_->setReadOnly(true);
    transcript_->setAcceptRichText(true);

    auto* compose = new QFrame(content);
    compose->setObjectName("compose");
    auto* composeLayout = new QHBoxLayout(compose);
    composeLayout->setContentsMargins(14, 12, 14, 12);
    messageEdit_ = new QLineEdit(compose);
    messageEdit_->setPlaceholderText("Write a message");
    sendButton_ = new QPushButton("Send", compose);
    composeLayout->addWidget(messageEdit_, 1);
    composeLayout->addWidget(sendButton_);

    contentLayout->addWidget(header);
    contentLayout->addWidget(systemLog_);
    contentLayout->addWidget(transcript_, 1);
    contentLayout->addWidget(compose);

    rootLayout->addWidget(sidebar);
    rootLayout->addWidget(content, 1);

    setCentralWidget(root);
    setWindowTitle("NexTalk");
    resize(1120, 720);

    addChatSearchTimer_ = new QTimer(this);
    addChatSearchTimer_->setSingleShot(true);

    setStyleSheet(
        "QWidget { font-size: 14px; }"
        "#sidebar { background: #172331; }"
        "#search { background: #243447; color: #edf4ff; border: 0; border-radius: 16px; padding: 10px 14px; }"
        "#addChatButton { background: #1fbf75; }"
        "#chatList { background: #172331; color: #edf4ff; border: 0; outline: 0; }"
        "#chatList::item { border-bottom: 1px solid #243447; }"
        "#chatList::item:selected { background: #243447; border-radius: 8px; }"
        "#chatHeader { background: #ffffff; border-bottom: 1px solid #d9e0e8; }"
        "#chatTitle { font-size: 20px; font-weight: 700; }"
        "#muted { color: #8ca0b3; }"
        "#accountStatus { color: #c8d6e4; padding: 6px 2px; }"
        "#systemLog { background: #f8fafc; color: #52677a; border: 0; border-bottom: 1px solid #d9e0e8; padding: 8px; }"
        "#transcript { background: #e7eef5; border: 0; padding: 16px; }"
        "#compose { background: #ffffff; border-top: 1px solid #d9e0e8; }"
        "QPushButton { border: 0; border-radius: 8px; background: #2da7ff; color: white; padding: 8px 12px; }"
        "QPushButton:disabled { background: #93a4b5; }"
        "QLineEdit, QSpinBox { padding: 8px; }"
    );
}

void MainWindow::wireUi() {
    connect(accountButton_, &QPushButton::clicked, this, [this] {
        if (connected_ || connecting_) {
            disconnectFromServer();
        } else {
            showAuthDialog();
        }
    });

    connect(chatsButton_, &QPushButton::clicked, this, [this] {
        if (connected_) {
            peerEdit_->clear();
            connection_->fetchChats();
        }
    });

    connect(settingsButton_, &QPushButton::clicked, this, [this] {
        appendSystemMessage("Settings will be added later");
    });

    connect(sendButton_, &QPushButton::clicked, this, [this] {
        sendMessage();
    });

    connect(messageEdit_, &QLineEdit::returnPressed, this, [this] {
        sendMessage();
    });

    connect(historyButton_, &QPushButton::clicked, this, [this] {
        requestHistory();
    });

    connect(addChatButton_, &QPushButton::clicked, this, [this] {
        openAddChatDialog();
    });

    connect(chatList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current, QListWidgetItem*) {
        if (!current) {
            updateChatActions();
            return;
        }

        const int kind = current->data(kItemKindRole).toInt();
        const QString peer = current->data(kItemPeerRole).toString().trimmed();
        if (kind == kChatItem) {
            selectPeer(peer);
        }
    });

    connect(peerEdit_, &QLineEdit::returnPressed, this, [this] {
        if (auto* item = firstVisibleChatItem()) {
            chatList_->setCurrentItem(item);
            selectPeer(item->data(kItemPeerRole).toString());
        }
    });

    connect(peerEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        filterChatList();
        updateChatActions();
    });

    connect(messageEdit_, &QLineEdit::textChanged, this, [this] {
        updateChatActions();
    });

    connect(addChatSearchTimer_, &QTimer::timeout, this, [this] {
        runAddChatSearch();
    });
}

void MainWindow::installConnectionCallbacks() {
    client::ClientConnection::Callbacks callbacks;

    callbacks.onInfo = [this](const std::string& text) {
        QMetaObject::invokeMethod(this, [this, text] {
            appendSystemMessage(QString::fromStdString(text));
        }, Qt::QueuedConnection);
    };
    callbacks.onError = [this](const std::string& text) {
        QMetaObject::invokeMethod(this, [this, text] {
            appendSystemMessage("Error: " + QString::fromStdString(text));
        }, Qt::QueuedConnection);
    };
    callbacks.onRegisterResult = [this](const std::string& status, const std::string& text) {
        QMetaObject::invokeMethod(this, [this, status, text] {
            appendSystemMessage("Register " + QString::fromStdString(status) +
                                ": " + QString::fromStdString(text));
            if (status != "ok" && pendingCreateAccount_) {
                manualDisconnect_ = true;
                connection_->stop();
                setConnectingState(false);
                setConnectedState(false);
            }
        }, Qt::QueuedConnection);
    };
    callbacks.onLoginResult = [this](const std::string& status, const std::string& text) {
        QMetaObject::invokeMethod(this, [this, status, text] {
            setConnectingState(false);
            const bool loggedIn = status == "ok";
            setConnectedState(loggedIn);
            appendSystemMessage("Login " + QString::fromStdString(status) +
                                ": " + QString::fromStdString(text));
            if (loggedIn) {
                transcript_->clear();
                chatList_->clear();
                selectedPeer_.clear();
                selectedPeerId_.clear();
                connection_->fetchChats();
            } else {
                manualDisconnect_ = true;
                connection_->stop();
                resetSessionUi();
            }
        }, Qt::QueuedConnection);
    };
    callbacks.onIncomingMessage = [this](const std::string& sender, const std::string& text) {
        QMetaObject::invokeMethod(this, [this, sender, text] {
            const QString peer = QString::fromStdString(sender);
            rememberPeer(peer);
            if (currentPeer().isEmpty()) {
                selectPeer(peer);
            }
            appendChatMessage(peer, QString::fromStdString(text));
            connection_->fetchChats();
        }, Qt::QueuedConnection);
    };
    callbacks.onHistoryMessage = [this](const client::HistoryItem& item) {
        QMetaObject::invokeMethod(this, [this, item] {
            appendHistoryMessage(item);
        }, Qt::QueuedConnection);
    };
    callbacks.onHistoryResult = [this](const std::string& status, const std::string& text) {
        QMetaObject::invokeMethod(this, [this, status, text] {
            appendSystemMessage("History " + QString::fromStdString(status) +
                                ": " + QString::fromStdString(text));
        }, Qt::QueuedConnection);
    };
    callbacks.onChatItem = [this](const client::ChatSummary& chat) {
        QMetaObject::invokeMethod(this, [this, chat] {
            updateChatPreview(chat);
        }, Qt::QueuedConnection);
    };
    callbacks.onChatListResult = [this](const std::string&, const std::string&) {
        QMetaObject::invokeMethod(this, [this] {
            filterChatList();
            if (chatList_->count() > 0 && currentPeer().isEmpty()) {
                if (auto* item = firstVisibleChatItem()) {
                    chatList_->setCurrentItem(item);
                }
            }
        }, Qt::QueuedConnection);
    };
    callbacks.onUserSearchItem = [this](const std::string& query, const client::UserSearchItem& user) {
        QMetaObject::invokeMethod(this, [this, query, user] {
            addUserSearchResult(QString::fromStdString(query), user);
        }, Qt::QueuedConnection);
    };
    callbacks.onUserSearchResult = [this](const std::string& status,
                                          const std::string& text,
                                          const std::string& query) {
        QMetaObject::invokeMethod(this, [this, status, text, query] {
            finishUserSearch(QString::fromStdString(status),
                             QString::fromStdString(text),
                             QString::fromStdString(query));
        }, Qt::QueuedConnection);
    };
    callbacks.onCreateChatResult = [this](const std::string& status,
                                          const std::string& peerId,
                                          const std::string& text) {
        QMetaObject::invokeMethod(this, [this, status, peerId, text] {
            const QString qtStatus = QString::fromStdString(status);
            const QString qtPeerId = QString::fromStdString(peerId);
            const QString qtText = QString::fromStdString(text);
            if (qtStatus != "ok") {
                appendSystemMessage("Create chat " + qtStatus + ": " + qtText);
                if (addChatCreateButton_) {
                    addChatCreateButton_->setText("Create chat");
                    addChatCreateButton_->setEnabled(!selectedAddChatUserId().isEmpty());
                }
                updateChatActions();
                return;
            }

            if (addChatDialog_) {
                addChatDialog_->close();
            }
            chatList_->clear();
            selectedPeerId_ = qtPeerId;
            selectPeer(qtText);
            connection_->fetchChats();
        }, Qt::QueuedConnection);
    };
    callbacks.onDeleteChatResult = [this](const std::string& status,
                                          const std::string& text,
                                          const std::string& peer) {
        QMetaObject::invokeMethod(this, [this, status, text, peer] {
            const QString qtStatus = QString::fromStdString(status);
            const QString qtText = QString::fromStdString(text);
            const QString qtPeer = QString::fromStdString(peer);
            if (qtStatus != "ok") {
                appendSystemMessage("Delete chat " + qtStatus + ": " + qtText);
                return;
            }

            removeChatItem(qtPeer);
            appendSystemMessage("Chat deleted");
            connection_->fetchChats();
        }, Qt::QueuedConnection);
    };
    callbacks.onProtocolError = [this](const std::string& text) {
        QMetaObject::invokeMethod(this, [this, text] {
            appendSystemMessage("Protocol: " + QString::fromStdString(text));
        }, Qt::QueuedConnection);
    };
    callbacks.onDisconnected = [this] {
        QMetaObject::invokeMethod(this, [this] {
            if (!manualDisconnect_) {
                appendSystemMessage("Disconnected from server");
            }
            manualDisconnect_ = false;
            setConnectingState(false);
            setConnectedState(false);
        }, Qt::QueuedConnection);
    };

    connection_->setCallbacks(std::move(callbacks));
}

void MainWindow::showAuthDialog() {
    if (connection_->isRunning()) {
        manualDisconnect_ = true;
        connection_->stop();
    }

    AuthDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const AuthData data = dialog.authData();
    connectWithAuth(data.host, data.port, data.username, data.password, data.createAccount);
}

void MainWindow::connectWithAuth(const QString& host,
                                 int port,
                                 const QString& username,
                                 const QString& password,
                                 bool createAccount) {
    if (username.isEmpty() || password.isEmpty()) {
        appendSystemMessage("Username and password are required");
        return;
    }

    currentUsername_ = username;
    pendingPassword_ = password;
    pendingCreateAccount_ = createAccount;

    std::string error;
    if (connection_->isRunning()) {
        manualDisconnect_ = true;
        connection_->stop();
    }

    setConnectingState(true);
    if (!connection_->connectToServer(host.toStdString(), port, error)) {
        setConnectingState(false);
        appendSystemMessage("Cannot connect: " + QString::fromStdString(error));
        return;
    }

    if (pendingCreateAccount_) {
        connection_->registerAccount(currentUsername_.toStdString(), pendingPassword_.toStdString());
    }
    connection_->login(currentUsername_.toStdString(), pendingPassword_.toStdString());
}

void MainWindow::disconnectFromServer() {
    manualDisconnect_ = true;
    connection_->quit();
    connection_->stop();
    setConnectingState(false);
    setConnectedState(false);
    resetSessionUi();
    appendSystemMessage("Disconnected");
}

void MainWindow::sendMessage() {
    const QString peer = currentPeer();
    const QString text = messageEdit_->text();
    if (!connected_ || peer.isEmpty() || text.trimmed().isEmpty()) {
        updateChatActions();
        return;
    }

    rememberPeer(peer);
    connection_->sendPrivateMessage(peer.toStdString(), text.toStdString());
    appendChatMessage(currentUsername_, text);
    messageEdit_->clear();
    updateChatActions();
    connection_->fetchChats();
}

void MainWindow::requestHistory() {
    const QString peer = currentPeer();
    if (!connected_ || peer.isEmpty()) {
        updateChatActions();
        return;
    }

    rememberPeer(peer);
    clearTranscriptForPeer(peer);
    connection_->fetchHistory(peer.toStdString(), std::to_string(historyLimitSpin_->value()));
}

void MainWindow::selectPeer(const QString& peer) {
    const QString normalizedPeer = peer.trimmed();
    if (normalizedPeer.isEmpty()) {
        return;
    }

    selectedPeer_ = normalizedPeer;
    for (int index = 0; index < chatList_->count(); ++index) {
        auto* item = chatList_->item(index);
        if (item->data(kItemPeerRole).toString() == normalizedPeer) {
            selectedPeerId_ = item->data(kItemUserIdRole).toString();
            if (chatList_->currentItem() != item) {
                chatList_->setCurrentItem(item);
            }
            break;
        }
    }

    chatTitleLabel_->setText(normalizedPeer);
    updateChatActions();
    if (connected_) {
        requestHistory();
    }
}

void MainWindow::rememberPeer(const QString& peer) {
    const QString normalizedPeer = peer.trimmed();
    if (normalizedPeer.isEmpty()) {
        return;
    }

    for (int index = 0; index < chatList_->count(); ++index) {
        if (chatList_->item(index)->data(kItemPeerRole).toString() == normalizedPeer) {
            return;
        }
    }

    auto* item = new QListWidgetItem();
    item->setData(kItemKindRole, kChatItem);
    item->setData(kItemPeerRole, normalizedPeer);
    item->setData(kItemPreviewRole, "No messages yet");
    chatList_->addItem(item);
    installChatItemWidget(item);
    filterChatList();
}

void MainWindow::updateChatPreview(const client::ChatSummary& chat) {
    const QString peerId = QString::fromStdString(chat.peerId);
    const QString peer = QString::fromStdString(chat.peer);
    const QString lastSender = QString::fromStdString(chat.lastSender);
    const QString lastText = QString::fromStdString(chat.lastText);
    const QString preview = lastText.isEmpty() ? "No messages yet" : (lastSender + ": " + lastText);

    for (int index = 0; index < chatList_->count(); ++index) {
        if (chatList_->item(index)->data(kItemPeerRole).toString() == peer) {
            chatList_->item(index)->setText("");
            chatList_->item(index)->setData(kItemKindRole, kChatItem);
            chatList_->item(index)->setData(kItemPeerRole, peer);
            chatList_->item(index)->setData(kItemUserIdRole, peerId);
            chatList_->item(index)->setData(kItemPreviewRole, preview);
            installChatItemWidget(chatList_->item(index));
            filterChatList();
            return;
        }
    }

    auto* item = new QListWidgetItem();
    item->setData(kItemKindRole, kChatItem);
    item->setData(kItemPeerRole, peer);
    item->setData(kItemUserIdRole, peerId);
    item->setData(kItemPreviewRole, preview);
    chatList_->addItem(item);
    installChatItemWidget(item);
    filterChatList();
}

void MainWindow::installChatItemWidget(QListWidgetItem* item) {
    if (!item || item->data(kItemKindRole).toInt() != kChatItem) {
        return;
    }

    if (auto* oldWidget = chatList_->itemWidget(item)) {
        chatList_->removeItemWidget(item);
        oldWidget->deleteLater();
    }

    const QString peer = item->data(kItemPeerRole).toString();
    QString preview = item->data(kItemPreviewRole).toString().trimmed().isEmpty()
        ? "No messages yet"
        : item->data(kItemPreviewRole).toString().trimmed();
    if (preview.size() > 42) {
        preview = preview.left(39) + "...";
    }

    auto* row = new QWidget(chatList_);
    row->setObjectName("chatRow");
    row->setMinimumHeight(72);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 9, 8, 9);
    layout->setSpacing(8);

    auto* textBlock = new QWidget(row);
    textBlock->setMinimumWidth(0);
    auto* textLayout = new QVBoxLayout(textBlock);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(5);

    auto* nameLabel = new QLabel(peer, textBlock);
    nameLabel->setStyleSheet("color:#edf4ff;font-weight:700;");
    auto* previewLabel = new QLabel(preview, textBlock);
    previewLabel->setStyleSheet("color:#9fb3c8;");
    previewLabel->setWordWrap(false);
    previewLabel->setMinimumWidth(0);
    previewLabel->setTextInteractionFlags(Qt::NoTextInteraction);

    textLayout->addWidget(nameLabel);
    textLayout->addWidget(previewLabel);

    auto* deleteButton = new QToolButton(row);
    deleteButton->setText("x");
    deleteButton->setToolTip("Delete chat");
    deleteButton->setCursor(Qt::PointingHandCursor);
    deleteButton->setFixedSize(24, 24);
    deleteButton->setStyleSheet(
        "QToolButton { border:0;border-radius:12px;background:#2b3d51;color:#c8d6e4;font-weight:700; }"
        "QToolButton:hover { background:#ef4444;color:white; }"
    );

    layout->addWidget(textBlock, 1);
    layout->addWidget(deleteButton, 0, Qt::AlignTop);

    connect(deleteButton, &QToolButton::clicked, this, [this, peer] {
        confirmDeleteChat(peer);
    });

    item->setSizeHint(QSize(chatList_->viewport()->width(), 72));
    chatList_->setItemWidget(item, row);
}

void MainWindow::confirmDeleteChat(const QString& peer) {
    if (!connected_ || peer.trimmed().isEmpty()) {
        return;
    }

    QMessageBox confirm(this);
    confirm.setWindowTitle("Delete chat");
    confirm.setText("Delete chat with " + peer + "?");
    confirm.setInformativeText("The chat history will be removed from the server.");
    confirm.setStandardButtons(QMessageBox::Cancel | QMessageBox::Yes);
    confirm.button(QMessageBox::Yes)->setText("Delete");
    confirm.setDefaultButton(QMessageBox::Cancel);

    if (confirm.exec() != QMessageBox::Yes) {
        return;
    }

    connection_->deleteChat(peer.toStdString());
}

void MainWindow::removeChatItem(const QString& peer) {
    for (int index = 0; index < chatList_->count(); ++index) {
        auto* item = chatList_->item(index);
        if (item->data(kItemPeerRole).toString() != peer) {
            continue;
        }

        delete chatList_->takeItem(index);
        break;
    }

    if (selectedPeer_ == peer) {
        selectedPeer_.clear();
        selectedPeerId_.clear();
        transcript_->clear();
        chatTitleLabel_->setText("NexTalk");
        if (auto* next = firstVisibleChatItem()) {
            chatList_->setCurrentItem(next);
            selectPeer(next->data(kItemPeerRole).toString());
        }
    }

    updateChatActions();
}

void MainWindow::filterChatList() {
    const QString query = peerEdit_->text().trimmed();
    for (int index = 0; index < chatList_->count(); ++index) {
        auto* item = chatList_->item(index);
        if (item->data(kItemKindRole).toInt() != kChatItem) {
            continue;
        }

        const bool matches = query.isEmpty() ||
            item->data(kItemPeerRole).toString().contains(query, Qt::CaseInsensitive) ||
            item->data(kItemPreviewRole).toString().contains(query, Qt::CaseInsensitive);
        item->setHidden(!matches);
    }
}

QListWidgetItem* MainWindow::firstVisibleChatItem() const {
    for (int index = 0; index < chatList_->count(); ++index) {
        auto* item = chatList_->item(index);
        if (!item->isHidden() && item->data(kItemKindRole).toInt() == kChatItem) {
            return item;
        }
    }

    return nullptr;
}

void MainWindow::openAddChatDialog() {
    if (!connected_) {
        return;
    }

    if (addChatDialog_) {
        addChatDialog_->raise();
        addChatDialog_->activateWindow();
        return;
    }

    addChatDialog_ = new QDialog(this);
    addChatDialog_->setAttribute(Qt::WA_DeleteOnClose);
    addChatDialog_->setWindowTitle("New chat");
    addChatDialog_->resize(360, 440);

    auto* layout = new QVBoxLayout(addChatDialog_);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto* title = new QLabel("Find user by username", addChatDialog_);
    title->setObjectName("chatTitle");
    addChatSearchEdit_ = new QLineEdit(addChatDialog_);
    addChatSearchEdit_->setPlaceholderText("Username");
    addChatResults_ = new QListWidget(addChatDialog_);
    addChatCreateButton_ = new QPushButton("Create chat", addChatDialog_);
    addChatCreateButton_->setEnabled(false);
    auto* cancelButton = new QPushButton("Cancel", addChatDialog_);

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(cancelButton);
    buttons->addWidget(addChatCreateButton_);

    layout->addWidget(title);
    layout->addWidget(addChatSearchEdit_);
    layout->addWidget(addChatResults_, 1);
    layout->addLayout(buttons);

    addChatDialog_->setStyleSheet(
        "QDialog { background: #f8fafc; }"
        "QLineEdit { padding: 9px; border: 1px solid #cbd5e1; border-radius: 8px; }"
        "QListWidget { background: white; border: 1px solid #d9e0e8; border-radius: 8px; outline: 0; }"
        "QListWidget::item { padding: 10px; border-bottom: 1px solid #edf2f7; }"
        "QListWidget::item:selected { background: #dceeff; color: #0f172a; }"
        "QPushButton { border: 0; border-radius: 8px; background: #2da7ff; color: white; padding: 8px 12px; }"
        "QPushButton:disabled { background: #93a4b5; }"
    );

    connect(addChatSearchEdit_, &QLineEdit::textChanged, this, [this] {
        scheduleAddChatSearch();
    });
    connect(addChatResults_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current, QListWidgetItem*) {
        addChatCreateButton_->setEnabled(current && current->data(kItemKindRole).toInt() == kUserSearchItem);
    });
    connect(addChatCreateButton_, &QPushButton::clicked, this, [this] {
        createSelectedChat();
    });
    connect(cancelButton, &QPushButton::clicked, addChatDialog_, &QDialog::reject);
    connect(addChatDialog_, &QDialog::destroyed, this, [this] {
        addChatDialog_ = nullptr;
        addChatSearchEdit_ = nullptr;
        addChatResults_ = nullptr;
        addChatCreateButton_ = nullptr;
        pendingAddChatSearchQuery_.clear();
        addChatHasResults_ = false;
        addChatSearchTimer_->stop();
    });

    addChatResults_->addItem(makeStatusItem("Type a username to search"));
    addChatDialog_->show();
    addChatSearchEdit_->setFocus();
}

void MainWindow::scheduleAddChatSearch() {
    if (!addChatSearchEdit_) {
        return;
    }

    const QString query = addChatSearchEdit_->text().trimmed();
    pendingAddChatSearchQuery_ = query;
    addChatSearchTimer_->start(250);
}

void MainWindow::runAddChatSearch() {
    if (!addChatDialog_ || !addChatSearchEdit_ || !addChatResults_) {
        return;
    }

    const QString query = addChatSearchEdit_->text().trimmed();
    pendingAddChatSearchQuery_ = query;
    addChatHasResults_ = false;
    addChatCreateButton_->setEnabled(false);
    addChatResults_->clear();

    if (query.isEmpty()) {
        addChatResults_->addItem(makeStatusItem("Type a username to search"));
        return;
    }

    if (!isValidSearchQuery(query)) {
        addChatResults_->addItem(makeStatusItem("Use letters, digits, _ or -"));
        return;
    }

    addChatResults_->addItem(makeStatusItem("Searching..."));
    connection_->searchUsers(query.toStdString());
}

void MainWindow::addUserSearchResult(const QString& query, const client::UserSearchItem& user) {
    if (!addChatDialog_ || !addChatResults_ || query != pendingAddChatSearchQuery_) {
        return;
    }

    const bool firstResult = !addChatHasResults_;
    if (firstResult) {
        addChatResults_->clear();
        addChatHasResults_ = true;
    }

    const QString username = QString::fromStdString(user.username);
    auto* item = new QListWidgetItem(username);
    item->setData(kItemKindRole, kUserSearchItem);
    item->setData(kItemUserIdRole, QString::fromStdString(user.id));
    item->setData(kItemPeerRole, username);
    addChatResults_->addItem(item);
    if (firstResult) {
        addChatResults_->setCurrentItem(item);
    }
}

void MainWindow::finishUserSearch(const QString& status, const QString& text, const QString& query) {
    if (!addChatDialog_ || !addChatResults_ || query != pendingAddChatSearchQuery_) {
        return;
    }

    if (status != "ok") {
        addChatResults_->clear();
        addChatResults_->addItem(makeStatusItem(text));
        addChatCreateButton_->setEnabled(false);
        return;
    }

    if (!addChatHasResults_) {
        addChatResults_->clear();
        addChatResults_->addItem(makeStatusItem("No users found"));
        addChatCreateButton_->setEnabled(false);
    }
}

void MainWindow::createSelectedChat() {
    const QString userId = selectedAddChatUserId();
    if (!connected_ || userId.isEmpty()) {
        return;
    }

    addChatCreateButton_->setEnabled(false);
    addChatCreateButton_->setText("Creating...");
    connection_->createChat(userId.toStdString());
}

QString MainWindow::selectedAddChatUserId() const {
    if (!addChatResults_) {
        return {};
    }

    const auto* item = addChatResults_->currentItem();
    if (!item || item->data(kItemKindRole).toInt() != kUserSearchItem) {
        return {};
    }

    return item->data(kItemUserIdRole).toString().trimmed();
}

void MainWindow::clearTranscriptForPeer(const QString& peer) {
    transcript_->clear();
    chatStatusLabel_->setText("Chat with " + peer);
    appendSystemMessage("Loading history with " + peer);
}

void MainWindow::resetSessionUi() {
    connected_ = false;
    connecting_ = false;
    currentUsername_.clear();
    pendingPassword_.clear();
    pendingCreateAccount_ = false;
    selectedPeer_.clear();
    selectedPeerId_.clear();
    pendingAddChatSearchQuery_.clear();
    addChatHasResults_ = false;
    addChatSearchTimer_->stop();
    if (addChatDialog_) {
        addChatDialog_->close();
    }
    peerEdit_->clear();
    chatList_->clear();
    transcript_->clear();
    chatTitleLabel_->setText("NexTalk");
    profileLabel_->setText("Not signed in");
    accountButton_->setText("Profile");
    updateChatActions();
}

void MainWindow::appendSystemMessage(const QString& text) {
    systemLog_->append("[" + QTime::currentTime().toString("HH:mm:ss") + "] " + text);
}

void MainWindow::appendChatMessage(const QString& sender, const QString& text, const QString& createdAt) {
    transcript_->append(renderMessageHtml(sender, text, createdAt));
}

void MainWindow::appendHistoryMessage(const client::HistoryItem& item) {
    appendChatMessage(QString::fromStdString(item.sender),
                      QString::fromStdString(item.text),
                      QString::fromStdString(item.createdAt));
}

QString MainWindow::renderMessageHtml(const QString& sender,
                                      const QString& text,
                                      const QString& createdAt) const {
    const QString initial = sender.isEmpty() ? "?" : sender.left(1).toUpper();
    const QString color = avatarColor(sender);
    const QString safeSender = htmlText(sender);
    const QString safeText = htmlText(text);
    const QString safeTime = htmlText(timePart(createdAt));

    return QString(
        "<table width='100%' cellspacing='0' cellpadding='0' style='margin:10px 0;'>"
        "<tr>"
        "<td width='54' valign='top'>"
        "<div style='width:42px;height:42px;border-radius:21px;background:%1;color:white;"
        "font-weight:700;text-align:center;line-height:42px;font-size:16px;'>%2</div>"
        "</td>"
        "<td valign='top'>"
        "<div style='background:white;border-radius:16px;padding:10px 14px;'>"
        "<table width='100%' cellspacing='0' cellpadding='0'>"
        "<tr>"
        "<td style='font-weight:700;color:#172331;font-size:14px;padding-bottom:6px;'>%3</td>"
        "<td align='right' style='color:#7b8da1;font-size:12px;'>%4</td>"
        "</tr>"
        "</table>"
        "<div style='color:#172331;text-align:left;font-size:15px;line-height:1.35;"
        "padding:4px 8px 2px 8px;'>%5</div>"
        "</div>"
        "</td>"
        "</tr>"
        "</table>"
    ).arg(color, htmlText(initial), safeSender, safeTime, safeText);
}

QString MainWindow::currentPeer() const {
    return selectedPeer_;
}

void MainWindow::setConnectedState(bool connected) {
    connected_ = connected;
    profileLabel_->setText(connected ? ("Signed in as " + currentUsername_) : "Not signed in");
    accountButton_->setText(connected ? "Sign out" : "Profile");
    updateChatActions();
}

void MainWindow::setConnectingState(bool connecting) {
    connecting_ = connecting;
    accountButton_->setEnabled(!connecting);
    updateChatActions();
}

void MainWindow::updateChatActions() {
    const bool hasPeer = !currentPeer().isEmpty();
    sendButton_->setEnabled(connected_ && hasPeer && !messageEdit_->text().trimmed().isEmpty());
    historyButton_->setEnabled(connected_ && hasPeer);
    messageEdit_->setEnabled(connected_ && hasPeer);
    historyLimitSpin_->setEnabled(connected_ && hasPeer);
    chatsButton_->setEnabled(connected_);
    addChatButton_->setEnabled(connected_);

    if (!hasPeer) {
        chatStatusLabel_->setText("Choose a chat");
    } else if (!connected_) {
        chatStatusLabel_->setText("Sign in to open chat");
    } else {
        chatStatusLabel_->setText("Chat with " + currentPeer());
    }
}
