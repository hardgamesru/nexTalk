#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common/Protocol.hpp"

namespace client {

struct HistoryItem {
    long long id{0};
    std::string createdAt;
    std::string sender;
    std::string recipient;
    std::string text;
    long long replyToMessageId{0};
    std::string replyToSender;
    std::string replyToText;
    long long forwardFromMessageId{0};
    std::string forwardFromSender;
    std::string forwardFromText;
};

struct ChatSummary {
    std::string peerId;
    std::string peer;
    std::string lastAt;
    std::string lastSender;
    std::string lastText;
};

struct UserSearchItem {
    std::string id;
    std::string username;
};

/**
 * Reusable TCP client for NexTalk.
 *
 * Console UI and future GUI code should use this class instead of working with
 * sockets directly. It owns the socket, runs a receiver thread and exposes
 * typed callbacks for server events.
 */
class ClientConnection {
public:
    struct Callbacks {
        std::function<void(const std::string&)> onInfo;
        std::function<void(const std::string&)> onError;
        std::function<void(const std::string& status, const std::string& text)> onRegisterResult;
        std::function<void(const std::string& status, const std::string& text)> onLoginResult;
        std::function<void(const HistoryItem& item)> onIncomingMessage;
        std::function<void(const std::string& status, const HistoryItem& item, const std::string& text)> onSendMessageResult;
        std::function<void(const HistoryItem& item)> onHistoryMessage;
        std::function<void(const std::string& status, const std::string& text)> onHistoryResult;
        std::function<void(const ChatSummary& chat)> onChatItem;
        std::function<void(const std::string& status, const std::string& text)> onChatListResult;
        std::function<void(const std::string& query, const UserSearchItem& user)> onUserSearchItem;
        std::function<void(const std::string& status,
                           const std::string& text,
                           const std::string& query)> onUserSearchResult;
        std::function<void(const std::string& status,
                           const std::string& peerId,
                           const std::string& text)> onCreateChatResult;
        std::function<void(const std::string& status,
                           const std::string& text,
                           const std::string& peer)> onDeleteChatResult;
        std::function<void(const std::string& text)> onProtocolError;
        std::function<void()> onDisconnected;
    };

    ClientConnection();
    ~ClientConnection();

    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    void setCallbacks(Callbacks callbacks);

    bool connectToServer(const std::string& host, int port, std::string& error);
    void stop();

    bool isRunning() const;
    bool registerAccount(const std::string& username, const std::string& password);
    bool login(const std::string& username, const std::string& password);
    bool sendPrivateMessage(const std::string& recipient,
                            const std::string& text,
                            const std::string& replyToMessageId = "");
    bool forwardMessage(const std::string& recipient,
                        const std::string& sourceMessageId);
    bool fetchHistory(const std::string& peer, const std::string& limit = "");
    bool fetchChats();
    bool searchUsers(const std::string& query);
    bool createChat(const std::string& peerId);
    bool deleteChat(const std::string& peer);
    bool quit();

private:
    bool sendMessage(const common::ProtocolMessage& message);
    bool sendAll(const std::string& data);
    bool readLine(std::string& line);
    void receiverLoop();
    void handleServerMessage(const common::ProtocolMessage& message);
    Callbacks callbacksSnapshot();

    int socket_{-1};
    std::atomic<bool> running_{false};
    std::thread receiverThread_;
    mutable std::mutex writeMutex_;
    mutable std::mutex callbacksMutex_;
    Callbacks callbacks_;
};

}
