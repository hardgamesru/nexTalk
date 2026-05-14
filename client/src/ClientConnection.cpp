#include "client/ClientConnection.hpp"

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace client {
namespace {
    constexpr std::size_t kMaxLineLength = 8192;

    int openConnection(const std::string& host, int port, std::string& error) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        // getaddrinfo поддерживает и localhost, и IP-адреса. Это удобнее, чем
        // вручную разбирать IPv4/IPv6 в клиентском коде.
        addrinfo* results = nullptr;
        const std::string service = std::to_string(port);
        const int lookup = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
        if (lookup != 0) {
            error = gai_strerror(lookup);
            return -1;
        }

        int socket = -1;
        // У hostname может быть несколько адресов. Пробуем каждый, пока один
        // socket успешно не подключится.
        for (addrinfo* current = results; current != nullptr; current = current->ai_next) {
            socket = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (socket < 0) {
                continue;
            }

            if (::connect(socket, current->ai_addr, current->ai_addrlen) == 0) {
                break;
            }

            ::close(socket);
            socket = -1;
        }

        ::freeaddrinfo(results);

        if (socket < 0 && error.empty()) {
            error = "cannot connect to " + host + ':' + std::to_string(port);
        }

        return socket;
    }
}

ClientConnection::ClientConnection() = default;

ClientConnection::~ClientConnection() {
    stop();
}

void ClientConnection::setCallbacks(Callbacks callbacks) {
    std::lock_guard<std::mutex> lock(callbacksMutex_);
    callbacks_ = std::move(callbacks);
}

bool ClientConnection::connectToServer(const std::string& host, int port, std::string& error) {
    if (running_) {
        error = "connection is already active";
        return false;
    }

    // Если объект переиспользуется после stop(), старый receiverThread уже
    // должен быть завершен до открытия нового socket.
    if (receiverThread_.joinable() &&
        receiverThread_.get_id() != std::this_thread::get_id()) {
        receiverThread_.join();
    }

    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }

    socket_ = openConnection(host, port, error);
    if (socket_ < 0) {
        return false;
    }

    running_ = true;
    receiverThread_ = std::thread(&ClientConnection::receiverLoop, this);
    return true;
}

void ClientConnection::stop() {
    const bool wasRunning = running_.exchange(false);
    if (socket_ >= 0) {
        // shutdown будит receiverLoop, если он сейчас заблокирован в recv().
        ::shutdown(socket_, SHUT_RDWR);
    }

    if (receiverThread_.joinable() &&
        receiverThread_.get_id() != std::this_thread::get_id()) {
        receiverThread_.join();
    }

    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }

    (void)wasRunning;
}

bool ClientConnection::isRunning() const {
    return running_;
}

bool ClientConnection::registerAccount(const std::string& username, const std::string& password) {
    return sendMessage({common::CommandType::Register, {username, password}});
}

bool ClientConnection::login(const std::string& username, const std::string& password) {
    return sendMessage({common::CommandType::Login, {username, password}});
}

bool ClientConnection::sendPrivateMessage(const std::string& recipient,
                                          const std::string& text,
                                          const std::string& replyToMessageId) {
    if (replyToMessageId.empty()) {
        return sendMessage({common::CommandType::SendMessage, {recipient, text}});
    }

    return sendMessage({common::CommandType::SendMessage, {recipient, text, replyToMessageId}});
}

bool ClientConnection::forwardMessage(const std::string& recipient,
                                      const std::string& sourceMessageId) {
    return sendMessage({common::CommandType::ForwardMessage, {recipient, sourceMessageId}});
}

bool ClientConnection::fetchHistory(const std::string& peer, const std::string& limit) {
    if (limit.empty()) {
        return sendMessage({common::CommandType::FetchHistory, {peer}});
    }

    return sendMessage({common::CommandType::FetchHistory, {peer, limit}});
}

bool ClientConnection::fetchChats() {
    return sendMessage({common::CommandType::FetchChats, {}});
}

bool ClientConnection::searchUsers(const std::string& query) {
    return sendMessage({common::CommandType::SearchUsers, {query}});
}

bool ClientConnection::createChat(const std::string& peerId) {
    return sendMessage({common::CommandType::CreateChat, {peerId}});
}

bool ClientConnection::deleteChat(const std::string& peer) {
    return sendMessage({common::CommandType::DeleteChat, {peer}});
}

bool ClientConnection::quit() {
    return sendMessage({common::CommandType::Quit, {}});
}

bool ClientConnection::sendMessage(const common::ProtocolMessage& message) {
    if (!running_) {
        return false;
    }

    // common::serializeMessage добавляет '\n', поэтому sendAll отправляет уже
    // готовую строку протокола.
    return sendAll(common::serializeMessage(message));
}

bool ClientConnection::sendAll(const std::string& data) {
    std::lock_guard<std::mutex> lock(writeMutex_);

    if (socket_ < 0) {
        return false;
    }

    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t result = ::send(socket_, data.data() + sent, data.size() - sent, 0);
        if (result < 0 && errno == EINTR) {
            continue;
        }

        if (result <= 0) {
            return false;
        }

        sent += static_cast<std::size_t>(result);
    }

    return true;
}

bool ClientConnection::readLine(std::string& line) {
    line.clear();
    char ch = '\0';

    // Протокол построчный, а TCP потоковый. Читаем до '\n' и защищаемся от
    // слишком длинной строки, чтобы поврежденный peer не раздувал память.
    while (line.size() < kMaxLineLength) {
        const ssize_t result = ::recv(socket_, &ch, 1, 0);
        if (result == 0) {
            return false;
        }

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        line += ch;
        if (ch == '\n') {
            return true;
        }
    }

    return false;
}

void ClientConnection::receiverLoop() {
    std::string line;

    // Receiver thread только читает серверные события и раздает callbacks.
    // Отправка команд может идти из UI-потока параллельно через writeMutex_.
    while (running_ && readLine(line)) {
        common::ProtocolMessage message;
        if (!common::parseMessage(line, message)) {
            auto callbacks = callbacksSnapshot();
            if (callbacks.onProtocolError) {
                callbacks.onProtocolError("invalid protocol message");
            }
            continue;
        }

        handleServerMessage(message);
    }

    running_ = false;
    auto callbacks = callbacksSnapshot();
    if (callbacks.onDisconnected) {
        callbacks.onDisconnected();
    }
}

void ClientConnection::handleServerMessage(const common::ProtocolMessage& message) {
    auto callbacks = callbacksSnapshot();
    auto parseMessageItem = [](const std::vector<std::string>& fields, std::size_t offset, HistoryItem& item) -> bool {
        // Одно и то же представление сообщения используется в разных командах,
        // но иногда перед ним есть status/text. Поэтому offset задается снаружи.
        if (fields.size() < offset + 5) {
            return false;
        }

        item = HistoryItem{};
        if (!fields[offset].empty()) {
            try {
                item.id = std::stoll(fields[offset]);
            } catch (...) {
                return false;
            }
        }

        item.createdAt = fields[offset + 1];
        item.sender = fields[offset + 2];
        item.recipient = fields[offset + 3];
        item.text = fields[offset + 4];

        if (fields.size() >= offset + 6 && !fields[offset + 5].empty()) {
            try {
                item.replyToMessageId = std::stoll(fields[offset + 5]);
            } catch (...) {
                return false;
            }
        }

        if (fields.size() >= offset + 7) {
            item.replyToSender = fields[offset + 6];
        }

        if (fields.size() >= offset + 8) {
            item.replyToText = fields[offset + 7];
        }

        if (fields.size() >= offset + 9 && !fields[offset + 8].empty()) {
            try {
                item.forwardFromMessageId = std::stoll(fields[offset + 8]);
            } catch (...) {
                return false;
            }
        }

        if (fields.size() >= offset + 10) {
            item.forwardFromSender = fields[offset + 9];
        }

        if (fields.size() >= offset + 11) {
            item.forwardFromText = fields[offset + 10];
        }

        return true;
    };

    switch (message.type) {
    case common::CommandType::Info:
        if (!message.fields.empty() && callbacks.onInfo) {
            callbacks.onInfo(message.fields[0]);
        }
        break;
    case common::CommandType::LoginResult:
        if (message.fields.size() >= 2 && callbacks.onLoginResult) {
            callbacks.onLoginResult(message.fields[0], message.fields[1]);
        }
        break;
    case common::CommandType::RegisterResult:
        if (message.fields.size() >= 2 && callbacks.onRegisterResult) {
            callbacks.onRegisterResult(message.fields[0], message.fields[1]);
        }
        break;
    case common::CommandType::SendMessageResult:
        if (!message.fields.empty() && callbacks.onSendMessageResult) {
            HistoryItem item;
            const std::string text = message.fields.size() >= 2 ? message.fields[1] : "";
            if (message.fields[0] == "ok" && parseMessageItem(message.fields, 1, item)) {
                callbacks.onSendMessageResult(message.fields[0], item, {});
            } else {
                callbacks.onSendMessageResult(message.fields[0], {}, text);
            }
        }
        break;
    case common::CommandType::IncomingMessage:
        if (callbacks.onIncomingMessage) {
            HistoryItem item;
            if (parseMessageItem(message.fields, 0, item)) {
                callbacks.onIncomingMessage(item);
            }
        }
        break;
    case common::CommandType::HistoryMessage:
        if (callbacks.onHistoryMessage) {
            HistoryItem item;
            if (parseMessageItem(message.fields, 0, item)) {
                callbacks.onHistoryMessage(item);
            }
        }
        break;
    case common::CommandType::HistoryResult:
        if (message.fields.size() >= 2 && callbacks.onHistoryResult) {
            callbacks.onHistoryResult(message.fields[0], message.fields[1]);
        }
        break;
    case common::CommandType::ChatItem:
        if (message.fields.size() >= 5 && callbacks.onChatItem) {
            callbacks.onChatItem({
                message.fields[0],
                message.fields[1],
                message.fields[2],
                message.fields[3],
                message.fields[4]
            });
        }
        break;
    case common::CommandType::ChatListResult:
        if (message.fields.size() >= 2 && callbacks.onChatListResult) {
            callbacks.onChatListResult(message.fields[0], message.fields[1]);
        }
        break;
    case common::CommandType::UserSearchItem:
        if (message.fields.size() >= 3 && callbacks.onUserSearchItem) {
            callbacks.onUserSearchItem(message.fields[0], {message.fields[1], message.fields[2]});
        }
        break;
    case common::CommandType::UserSearchResult:
        if (message.fields.size() >= 3 && callbacks.onUserSearchResult) {
            callbacks.onUserSearchResult(message.fields[0], message.fields[1], message.fields[2]);
        }
        break;
    case common::CommandType::CreateChatResult:
        if (message.fields.size() >= 2 && callbacks.onCreateChatResult) {
            const std::string peerId = message.fields.size() >= 3 ? message.fields[1] : "";
            const std::string text = message.fields.size() >= 3 ? message.fields[2] : message.fields[1];
            callbacks.onCreateChatResult(message.fields[0], peerId, text);
        }
        break;
    case common::CommandType::DeleteChatResult:
        if (message.fields.size() >= 2 && callbacks.onDeleteChatResult) {
            const std::string peer = message.fields.size() >= 3 ? message.fields[2] : "";
            callbacks.onDeleteChatResult(message.fields[0], message.fields[1], peer);
        }
        break;
    case common::CommandType::Error:
        if (!message.fields.empty() && callbacks.onError) {
            callbacks.onError(message.fields[0]);
        }
        break;
    default:
        if (callbacks.onProtocolError) {
            callbacks.onProtocolError("unsupported server message");
        }
        break;
    }
}

ClientConnection::Callbacks ClientConnection::callbacksSnapshot() {
    std::lock_guard<std::mutex> lock(callbacksMutex_);
    return callbacks_;
}

}
