#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common/Protocol.hpp"

namespace client {

struct HistoryItem {
    std::string createdAt;
    std::string sender;
    std::string recipient;
    std::string text;
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
        std::function<void(const std::string& status, const std::string& text)> onLoginResult;
        std::function<void(const std::string& sender, const std::string& text)> onIncomingMessage;
        std::function<void(const HistoryItem& item)> onHistoryMessage;
        std::function<void(const std::string& status, const std::string& text)> onHistoryResult;
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
    bool login(const std::string& username);
    bool sendPrivateMessage(const std::string& recipient, const std::string& text);
    bool fetchHistory(const std::string& peer, const std::string& limit = "");
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
