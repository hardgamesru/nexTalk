#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common/Protocol.hpp"

namespace client {

/**
 * Сообщение, которое клиент показывает в истории.
 *
 * Поля повторяют серверный StoredMessage, но без деталей SQLite. Такой DTO
 * удобно передавать из сетевого слоя в консольный или GUI-интерфейс.
 */
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

/**
 * Одна строка списка чатов на клиенте.
 */
struct ChatSummary {
    // peerId используется в командах протокола; peer можно показывать человеку.
    std::string peerId;
    std::string peer;
    std::string lastAt;
    std::string lastSender;
    std::string lastText;
};

/**
 * Пользователь, найденный командой search_users.
 */
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
        // Все callbacks вызываются из receiverThread_. UI-слой должен сам
        // перекинуть событие в главный поток, если его toolkit этого требует.
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

    // Открывает TCP/TLS-соединение и запускает receiverLoop в отдельном потоке.
    bool connectToServer(const std::string& host, int port, std::string& error);
    // Останавливает receiverLoop, закрывает socket и join-ит поток.
    void stop();

    bool isRunning() const;
    // Ниже идут тонкие wrappers над протокольными командами. Они возвращают
    // false только если команда не ушла в сеть; результат операции придет callback-ом.
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
    // Сериализует ProtocolMessage и отправляет его целиком.
    bool sendMessage(const common::ProtocolMessage& message);
    // send() может записать только часть буфера, поэтому sendAll крутится,
    // пока не отправит всю строку протокола.
    bool sendAll(const std::string& data);
    // Читает до '\n', потому что общий Protocol работает построчно.
    bool readLine(std::string& line);
    void receiverLoop();
    // Разбирает входящую команду сервера и вызывает подходящий callback.
    void handleServerMessage(const common::ProtocolMessage& message);
    // Берем снимок callbacks под mutex, а вызываем уже без lock, чтобы callback
    // мог безопасно вызвать методы ClientConnection.
    Callbacks callbacksSnapshot();

    int socket_{-1};
    std::atomic<bool> running_{false};
    std::thread receiverThread_;
    mutable std::mutex writeMutex_;
    mutable std::mutex callbacksMutex_;
    Callbacks callbacks_;
};

}
