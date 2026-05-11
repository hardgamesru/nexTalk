#include "MainWindow.hpp"

#include <QApplication>
#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTextEdit>
#include <QTime>
#include <QVBoxLayout>

#include <utility>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      connection_(std::make_unique<client::ClientConnection>()) {
    buildUi();
    wireUi();
    installConnectionCallbacks();
    setConnectedState(false);
}

MainWindow::~MainWindow() {
    connection_->stop();
}

void MainWindow::buildUi() {
    auto* root = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

    auto* connectionBox = new QGroupBox("Connection", root);
    auto* connectionLayout = new QGridLayout(connectionBox);

    hostEdit_ = new QLineEdit("127.0.0.1", connectionBox);
    portSpin_ = new QSpinBox(connectionBox);
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(5555);
    loginEdit_ = new QLineEdit(connectionBox);
    loginEdit_->setPlaceholderText("alice");
    connectButton_ = new QPushButton("Connect", connectionBox);
    statusLabel_ = new QLabel("Disconnected", connectionBox);

    connectionLayout->addWidget(new QLabel("Host", connectionBox), 0, 0);
    connectionLayout->addWidget(hostEdit_, 0, 1);
    connectionLayout->addWidget(new QLabel("Port", connectionBox), 0, 2);
    connectionLayout->addWidget(portSpin_, 0, 3);
    connectionLayout->addWidget(new QLabel("Login", connectionBox), 1, 0);
    connectionLayout->addWidget(loginEdit_, 1, 1);
    connectionLayout->addWidget(connectButton_, 1, 2);
    connectionLayout->addWidget(statusLabel_, 1, 3);

    auto* splitter = new QSplitter(root);

    auto* sidePanel = new QWidget(splitter);
    auto* sideLayout = new QVBoxLayout(sidePanel);
    sideLayout->setContentsMargins(0, 0, 8, 0);
    sideLayout->setSpacing(8);

    peerEdit_ = new QLineEdit(sidePanel);
    peerEdit_->setPlaceholderText("Peer username");
    historyButton_ = new QPushButton("Load history", sidePanel);
    chatList_ = new QListWidget(sidePanel);
    chatList_->addItem("alice");
    chatList_->addItem("bob");

    sideLayout->addWidget(new QLabel("Chat", sidePanel));
    sideLayout->addWidget(peerEdit_);
    sideLayout->addWidget(historyButton_);
    sideLayout->addWidget(chatList_, 1);

    auto* chatPanel = new QWidget(splitter);
    auto* chatLayout = new QVBoxLayout(chatPanel);
    chatLayout->setContentsMargins(8, 0, 0, 0);
    chatLayout->setSpacing(8);

    transcript_ = new QTextEdit(chatPanel);
    transcript_->setReadOnly(true);
    transcript_->setAcceptRichText(false);

    auto* composeLayout = new QHBoxLayout();
    messageEdit_ = new QLineEdit(chatPanel);
    messageEdit_->setPlaceholderText("Message");
    sendButton_ = new QPushButton("Send", chatPanel);
    composeLayout->addWidget(messageEdit_, 1);
    composeLayout->addWidget(sendButton_);

    chatLayout->addWidget(transcript_, 1);
    chatLayout->addLayout(composeLayout);

    splitter->addWidget(sidePanel);
    splitter->addWidget(chatPanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    rootLayout->addWidget(connectionBox);
    rootLayout->addWidget(splitter, 1);

    setCentralWidget(root);
    setWindowTitle("NexTalk");
    resize(920, 620);
}

void MainWindow::wireUi() {
    connect(connectButton_, &QPushButton::clicked, this, [this] {
        connectToServer();
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

    connect(chatList_, &QListWidget::currentTextChanged, this, [this](const QString& value) {
        peerEdit_->setText(value);
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
    callbacks.onLoginResult = [this](const std::string& status, const std::string& text) {
        QMetaObject::invokeMethod(this, [this, status, text] {
            appendSystemMessage("Login " + QString::fromStdString(status) +
                                ": " + QString::fromStdString(text));
            setConnectedState(status == "ok");
        }, Qt::QueuedConnection);
    };
    callbacks.onIncomingMessage = [this](const std::string& sender, const std::string& text) {
        QMetaObject::invokeMethod(this, [this, sender, text] {
            appendChatMessage(QString::fromStdString(sender), QString::fromStdString(text));
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
    callbacks.onProtocolError = [this](const std::string& text) {
        QMetaObject::invokeMethod(this, [this, text] {
            appendSystemMessage("Protocol: " + QString::fromStdString(text));
        }, Qt::QueuedConnection);
    };
    callbacks.onDisconnected = [this] {
        QMetaObject::invokeMethod(this, [this] {
            appendSystemMessage("Disconnected from server");
            setConnectedState(false);
        }, Qt::QueuedConnection);
    };

    connection_->setCallbacks(std::move(callbacks));
}

void MainWindow::connectToServer() {
    if (connected_) {
        connection_->quit();
        connection_->stop();
        setConnectedState(false);
        appendSystemMessage("Disconnected");
        return;
    }

    const QString login = loginEdit_->text().trimmed();
    if (login.isEmpty()) {
        appendSystemMessage("Enter login first");
        return;
    }

    std::string error;
    if (!connection_->connectToServer(hostEdit_->text().trimmed().toStdString(),
                                      portSpin_->value(),
                                      error)) {
        appendSystemMessage("Cannot connect: " + QString::fromStdString(error));
        return;
    }

    appendSystemMessage("Connected, logging in");
    connection_->login(login.toStdString());
}

void MainWindow::sendMessage() {
    const QString peer = currentPeer();
    const QString text = messageEdit_->text();
    if (!connected_ || peer.isEmpty() || text.trimmed().isEmpty()) {
        return;
    }

    connection_->sendPrivateMessage(peer.toStdString(), text.toStdString());
    appendChatMessage(loginEdit_->text().trimmed(), text);
    messageEdit_->clear();
}

void MainWindow::requestHistory() {
    const QString peer = currentPeer();
    if (!connected_ || peer.isEmpty()) {
        return;
    }

    transcript_->clear();
    appendSystemMessage("Loading history with " + peer);
    connection_->fetchHistory(peer.toStdString(), "50");
}

void MainWindow::appendSystemMessage(const QString& text) {
    transcript_->append("[" + QTime::currentTime().toString("HH:mm:ss") + "] " + text);
}

void MainWindow::appendChatMessage(const QString& sender, const QString& text) {
    transcript_->append(sender + ": " + text);
}

void MainWindow::appendHistoryMessage(const client::HistoryItem& item) {
    transcript_->append(QString::fromStdString(item.createdAt) +
                        " " + QString::fromStdString(item.sender) +
                        " -> " + QString::fromStdString(item.recipient) +
                        ": " + QString::fromStdString(item.text));
}

QString MainWindow::currentPeer() const {
    return peerEdit_->text().trimmed();
}

void MainWindow::setConnectedState(bool connected) {
    connected_ = connected;
    connectButton_->setText(connected ? "Disconnect" : "Connect");
    statusLabel_->setText(connected ? "Connected" : "Disconnected");
    sendButton_->setEnabled(connected);
    historyButton_->setEnabled(connected);
}
