#include "MessengerServer.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <limits>
#include <set>
#include <random>
#include <sstream>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace {
    constexpr int kBacklog = 16;
    constexpr std::size_t kMaxLineLength = 8192;
    constexpr int kDefaultHistoryLimit = 20;
    constexpr int kMaxHistoryLimit = 100;
    constexpr int kMaxUserSearchResults = 20;
    constexpr const char* kServerCertificatePath = "certs/server.crt";
    constexpr const char* kServerPrivateKeyPath = "certs/server.key";

    // Преобразует sockaddr_in из accept() в читаемый вид для логов.
    std::string sockaddrToString(const sockaddr_in& address) {
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));

        std::ostringstream out;
        out << ip << ':' << ntohs(address.sin_port);
        return out.str();
    }

    // Лог тоже строковый, поэтому пользовательский текст экранируется перед
    // записью. Так перенос строки внутри сообщения не сломает формат лог-файла.
    std::string escapeLogField(const std::string& value) {
        std::string escaped;
        escaped.reserve(value.size());

        for (char ch : value) {
            switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\t':
                escaped += "\\t";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            default:
                escaped += ch;
                break;
            }
        }

        return escaped;
    }

    bool parsePositiveInt(const std::string& value, int& output) {
        if (value.empty()) {
            return false;
        }

        int result = 0;
        for (char ch : value) {
            if (ch < '0' || ch > '9') {
                return false;
            }

            const int digit = ch - '0';
            if (result > (std::numeric_limits<int>::max() - digit) / 10) {
                return false;
            }

            result = result * 10 + digit;
        }

        if (result <= 0) {
            return false;
        }

        output = result;
        return true;
    }

    bool parsePositiveInt64(const std::string& value, long long& output) {
        if (value.empty()) {
            return false;
        }

        long long result = 0;
        for (char ch : value) {
            if (ch < '0' || ch > '9') {
                return false;
            }

            const int digit = ch - '0';
            if (result > (std::numeric_limits<long long>::max() - digit) / 10) {
                return false;
            }

            result = result * 10 + digit;
        }

        if (result <= 0) {
            return false;
        }

        output = result;
        return true;
    }

    std::vector<std::string> splitCsv(const std::string& value) {
        std::vector<std::string> items;
        std::stringstream input(value);
        std::string token;
        while (std::getline(input, token, ',')) {
            if (!token.empty()) {
                items.push_back(token);
            }
        }
        return items;
    }

    std::string makeGroupChatId(long long groupId) {
        return "group:" + std::to_string(groupId);
    }

    bool parseGroupChatIdValue(const std::string& chatId, long long& groupId) {
        if (chatId.rfind("group:", 0) != 0) {
            return false;
        }
        return parsePositiveInt64(chatId.substr(6), groupId);
    }

    std::string lastOpenSslError() {
        const unsigned long errorCode = ERR_get_error();
        if (errorCode == 0) {
            return "unknown OpenSSL error";
        }

        char buffer[256] = {};
        ERR_error_string_n(errorCode, buffer, sizeof(buffer));
        return buffer;
    }

    std::vector<std::string> buildStoredMessageFields(const StoredMessage& storedMessage,
                                                      const std::string& chatId,
                                                      const std::string& recipientLabel) {
        return {
            std::to_string(storedMessage.id),
            chatId,
            storedMessage.createdAt,
            storedMessage.sender,
            recipientLabel,
            storedMessage.text,
            storedMessage.replyToMessageId > 0 ? std::to_string(storedMessage.replyToMessageId) : "",
            storedMessage.replyToSender,
            storedMessage.replyToText,
            storedMessage.forwardFromMessageId > 0 ? std::to_string(storedMessage.forwardFromMessageId) : "",
            storedMessage.forwardFromSender,
            storedMessage.forwardFromText,
            storedMessage.deletedAt,
            storedMessage.deletedBy
        };
    }
}

MessengerServer::MessengerServer(int port,
                                 std::string bindAddress,
                                 std::string logPath,
                                 std::string dbPath)
    : port_(port),
      bindAddress_(std::move(bindAddress)),
      logPath_(std::move(logPath)),
      dbPath_(std::move(dbPath)) {
}

MessengerServer::~MessengerServer() {
    stop();
}

bool MessengerServer::start() {
    auto failStart = [&](const std::string& message) {
        std::cerr << message << '\n';
        if (listenSocket_ >= 0) {
            ::close(listenSocket_);
            listenSocket_ = -1;
        }
        if (sslContext_) {
            SSL_CTX_free(sslContext_);
            sslContext_ = nullptr;
        }
        messageStore_.close();
        if (logFile_.is_open()) {
            logFile_.close();
        }
        return false;
    };

    // Лог открывается в режиме append: события сохраняются между запусками
    // сервера, что требуется в задании преподавателя.
    logFile_.open(logPath_, std::ios::app);
    if (!logFile_) {
        return failStart("Cannot open log file: " + logPath_);
    }

    std::string storageError;
    if (!messageStore_.open(dbPath_, storageError)) {
        return failStart("Cannot open database: " + dbPath_ + ": " + storageError);
    }

    OPENSSL_init_ssl(0, nullptr);
    sslContext_ = SSL_CTX_new(TLS_server_method());
    if (!sslContext_) {
        return failStart("Cannot create TLS context: " + lastOpenSslError());
    }

    SSL_CTX_set_min_proto_version(sslContext_, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_file(sslContext_, kServerCertificatePath, SSL_FILETYPE_PEM) != 1) {
        return failStart(
            "Cannot load TLS certificate " + std::string(kServerCertificatePath) + ": " + lastOpenSslError());
    }

    if (SSL_CTX_use_PrivateKey_file(sslContext_, kServerPrivateKeyPath, SSL_FILETYPE_PEM) != 1) {
        return failStart(
            "Cannot load TLS private key " + std::string(kServerPrivateKeyPath) + ": " + lastOpenSslError());
    }

    if (SSL_CTX_check_private_key(sslContext_) != 1) {
        return failStart("TLS certificate/private key mismatch: " + lastOpenSslError());
    }

    // Создаем TCP/IPv4 socket. Это низкоуровневый дескриптор, через который
    // ОС будет принимать входящие сетевые подключения.
    listenSocket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket_ < 0) {
        return failStart("socket failed: " + std::string(std::strerror(errno)));
    }

    // SO_REUSEADDR позволяет быстро перезапустить сервер на том же порту после
    // остановки, не дожидаясь, пока ОС полностью освободит старое соединение.
    int enabled = 1;
    if (::setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        return failStart("setsockopt failed: " + std::string(std::strerror(errno)));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port_));

    if (::inet_pton(AF_INET, bindAddress_.c_str(), &address.sin_addr) != 1) {
        return failStart("Invalid bind address: " + bindAddress_);
    }

    // bind связывает socket с конкретным IP-адресом и портом.
    // 127.0.0.1 подходит для локального режима, 0.0.0.0 - для сетевого.
    if (::bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        return failStart("bind failed: " + std::string(std::strerror(errno)));
    }

    // После listen socket становится "слушающим": ОС начинает ставить входящие
    // подключения в очередь, откуда сервер забирает их через accept().
    if (::listen(listenSocket_, kBacklog) < 0) {
        return failStart("listen failed: " + std::string(std::strerror(errno)));
    }

    running_ = true;
    logEvent("server_started port=" + std::to_string(port_) +
             " bind=" + bindAddress_ +
             " db=" + dbPath_ +
             " tls=enabled");
    std::cout << "NexTalk TLS server listening on " << bindAddress_ << ':' << port_ << '\n';
    return true;
}

void MessengerServer::run(const std::function<bool()>& shouldStop) {
    // Главный поток сервера занимается только приемом новых клиентов. Работа с
    // уже подключенными клиентами выполняется в отдельных потоках handleClient.
    while (running_ && !shouldStop()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket_, &readSet);

        // select с таймаутом нужен, чтобы цикл не завис навсегда в accept().
        // Раз в секунду сервер просыпается и проверяет флаг завершения.
        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        const int ready = ::select(listenSocket_ + 1, &readSet, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "select failed: " << std::strerror(errno) << '\n';
            break;
        }

        if (ready == 0 || !FD_ISSET(listenSocket_, &readSet)) {
            continue;
        }

        sockaddr_in clientAddress{};
        socklen_t clientLength = sizeof(clientAddress);

        // accept создает новый socket уже для конкретного клиента. Listening
        // socket продолжает ждать следующие подключения.
        const int clientSocket = ::accept(
            listenSocket_, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);

        if (clientSocket < 0) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "accept failed: " << std::strerror(errno) << '\n';
            continue;
        }

        auto session = std::make_shared<ClientSession>();
        session->socket = clientSocket;
        session->peer = sockaddrToString(clientAddress);
        session->ssl = SSL_new(sslContext_);
        if (!session->ssl) {
            std::cerr << "SSL_new failed for " << session->peer << ": " << lastOpenSslError() << '\n';
            ::close(clientSocket);
            continue;
        }

        SSL_set_fd(session->ssl, clientSocket);
        if (SSL_accept(session->ssl) != 1) {
            std::cerr << "TLS handshake failed for " << session->peer << ": " << lastOpenSslError() << '\n';
            SSL_free(session->ssl);
            session->ssl = nullptr;
            ::close(clientSocket);
            continue;
        }

        logEvent("client_connected peer=" + session->peer);

        {
            // sessions_ используется главным потоком остановки сервера, поэтому
            // добавление новой сессии защищено mutex-ом.
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            sessions_.push_back(session);
        }

        // Для каждого клиента создается отдельный поток обработки. Это простая
        // и понятная модель многопоточности: один клиент - один
        // worker thread.
        std::lock_guard<std::mutex> lock(threadsMutex_);
        clientThreads_.emplace_back(&MessengerServer::handleClient, this, std::move(session));
    }

    // После выхода из основного цикла закрываем сокеты клиентов, чтобы их
    // потоки вышли из blocking recv(), а затем дожидаемся завершения потоков.
    stop();

    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        threads.swap(clientThreads_);
    }

    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    logEvent("server_stopped");
}

void MessengerServer::stop() {
    // running_ атомарный, потому что его читают клиентские потоки.
    running_.exchange(false);

    if (listenSocket_ >= 0) {
        ::shutdown(listenSocket_, SHUT_RDWR);
        ::close(listenSocket_);
        listenSocket_ = -1;
    }

    std::vector<std::shared_ptr<ClientSession>> sessions;
    {
        // Копируем shared_ptr под mutex-ом, а закрываем сокеты уже без удержания
        // sessionsMutex_, чтобы не блокировать новые операции дольше нужного.
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions = sessions_;
    }

    {
        // После начала остановки сервер больше не доставляет сообщения онлайн-
        // пользователям, поэтому таблица активных пользователей очищается.
        std::lock_guard<std::mutex> lock(clientsMutex_);
        onlineUsers_.clear();
    }

    for (const auto& session : sessions) {
        session->active = false;
        sendToSession(session, {common::CommandType::Info, {"server is shutting down"}});
        closeSessionSocket(session);
    }

    if (sslContext_) {
        SSL_CTX_free(sslContext_);
        sslContext_ = nullptr;
    }
}

bool MessengerServer::sendToSession(const std::shared_ptr<ClientSession>& session,
                                    const common::ProtocolMessage& message) {
    if (!session) {
        return false;
    }

    const std::string line = common::serializeMessage(message);

    // Запись в один и тот же socket защищена mutex-ом конкретной сессии. Это
    // важно, когда разные клиентские потоки одновременно отправляют события
    // одному и тому же пользователю.
    std::lock_guard<std::mutex> lock(session->writeMutex);
    if (session->socket < 0 || !session->ssl) {
        return false;
    }

    std::size_t sent = 0;
    while (sent < line.size()) {
        const int result = SSL_write(
            session->ssl,
            line.data() + sent,
            static_cast<int>(line.size() - sent));
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }

        const int sslError = SSL_get_error(session->ssl, result);
        if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE) {
            continue;
        }

        return false;
    }

    return true;
}

bool MessengerServer::readLine(const std::shared_ptr<ClientSession>& session, std::string& line) {
    line.clear();
    char ch = '\0';

    // TCP - поток байтов, он не хранит границы сообщений. Мы читаем по одному
    // байту до '\n', потому что наш протокол задает "одно сообщение = одна строка".
    while (line.size() < kMaxLineLength) {
        if (!session || !session->ssl) {
            return false;
        }

        const int result = SSL_read(session->ssl, &ch, 1);
        if (result > 0) {
            line += ch;
            if (ch == '\n') {
                return true;
            }
            continue;
        }

        const int sslError = SSL_get_error(session->ssl, result);
        if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE) {
            continue;
        }

        if (sslError == SSL_ERROR_ZERO_RETURN) {
            return false;
        }

        return false;
    }

    return false;
}

void MessengerServer::handleClient(std::shared_ptr<ClientSession> session) {
    sendToSession(session, {common::CommandType::Info, {"welcome to NexTalk"}});

    std::string line;
    while (running_ && session->active && readLine(session, line)) {
        common::ProtocolMessage message;

        // Все входящие строки проходят через общий парсер протокола из common.
        // Благодаря этому сервер и клиент используют один и тот же формат.
        if (!common::parseMessage(line, message)) {
            sendToSession(session, {common::CommandType::Error, {"invalid protocol message"}});
            continue;
        }

        switch (message.type) {
        case common::CommandType::Register:
            handleRegister(session, message);
            break;
        case common::CommandType::Login:
            handleLogin(session, message);
            break;
        case common::CommandType::SendMessage:
            handleSendMessage(session, message);
            break;
        case common::CommandType::DeleteMessage:
            handleDeleteMessage(session, message);
            break;
        case common::CommandType::ForwardMessage:
            handleForwardMessage(session, message);
            break;
        case common::CommandType::FetchHistory:
            handleFetchHistory(session, message);
            break;
        case common::CommandType::FetchHistoryBefore:
            handleFetchHistoryBefore(session, message);
            break;
        case common::CommandType::FetchChats:
            handleFetchChats(session, message);
            break;
        case common::CommandType::SearchUsers:
            handleSearchUsers(session, message);
            break;
        case common::CommandType::CreateChat:
            handleCreateChat(session, message);
            break;
        case common::CommandType::CreateGroup:
            handleCreateGroup(session, message);
            break;
        case common::CommandType::GetGroupInfo:
            handleGetGroupInfo(session, message);
            break;
        case common::CommandType::AddGroupMembers:
            handleAddGroupMembers(session, message);
            break;
        case common::CommandType::RemoveGroupMember:
            handleRemoveGroupMember(session, message);
            break;
        case common::CommandType::TransferGroupAdmin:
            handleTransferGroupAdmin(session, message);
            break;
        case common::CommandType::LeaveGroup:
            handleLeaveGroup(session, message);
            break;
        case common::CommandType::DeleteGroup:
            handleDeleteGroup(session, message);
            break;
        case common::CommandType::MarkRead:
            handleMarkRead(session, message);
            break;
        case common::CommandType::DeleteChat:
            handleDeleteChat(session, message);
            break;
        case common::CommandType::Quit:
            sendToSession(session, {common::CommandType::Info, {"bye"}});
            session->active = false;
            break;
        default:
            sendToSession(session, {common::CommandType::Error, {"unsupported command"}});
            break;
        }
    }

    removeSession(session);
}

void MessengerServer::handleLogin(const std::shared_ptr<ClientSession>& session,
                                  const common::ProtocolMessage& message) {
    if (message.fields.size() != 2 || !isValidUsername(message.fields[0])) {
        sendToSession(session, {common::CommandType::LoginResult, {"error", "invalid username or password"}});
        return;
    }

    const std::string username = message.fields[0];
    const std::string password = message.fields[1];

    std::string storageError;
    std::string salt;
    if (!messageStore_.loadPasswordSalt(username, salt, storageError)) {
        logEvent("user_login_storage_failed username=" + username +
                 " error=\"" + escapeLogField(storageError) + "\"");
        sendToSession(session, {common::CommandType::LoginResult, {"error", "storage error"}});
        return;
    }

    if (salt.empty()) {
        sendToSession(session, {common::CommandType::LoginResult, {"error", "unknown user"}});
        return;
    }

    bool authenticated = false;
    if (!messageStore_.authenticateUser(username, passwordHash(password, salt), authenticated, storageError)) {
        logEvent("user_login_storage_failed username=" + username +
                 " error=\"" + escapeLogField(storageError) + "\"");
        sendToSession(session, {common::CommandType::LoginResult, {"error", "storage error"}});
        return;
    }

    if (!authenticated) {
        sendToSession(session, {common::CommandType::LoginResult, {"error", "invalid password"}});
        logEvent("user_login_failed username=" + username + " peer=" + session->peer);
        return;
    }

    {
        // onlineUsers_ общий для всех клиентских потоков. Например, один поток
        // может логинить alice, пока другой поток ищет bob для доставки сообщения.
        std::lock_guard<std::mutex> lock(clientsMutex_);
        const auto existing = onlineUsers_.find(username);
        if (existing != onlineUsers_.end() && existing->second != session) {
            sendToSession(session, {common::CommandType::LoginResult, {"error", "user already online"}});
            return;
        }

        if (!session->username.empty() && session->username != username) {
            onlineUsers_.erase(session->username);
        }

        session->username = username;
        onlineUsers_[username] = session;
    }

    logEvent("user_login username=" + username + " peer=" + session->peer);
    sendToSession(session, {common::CommandType::LoginResult, {"ok", "logged in as " + username}});
    handleFetchChats(session, {common::CommandType::FetchChats, {}});
}

void MessengerServer::handleRegister(const std::shared_ptr<ClientSession>& session,
                                     const common::ProtocolMessage& message) {
    if (message.fields.size() != 2 || !isValidUsername(message.fields[0]) ||
        !isValidPassword(message.fields[1])) {
        sendToSession(session, {common::CommandType::RegisterResult,
                                {"error", "username must be 1-32 chars; password must be at least 4 chars"}});
        return;
    }

    const std::string username = message.fields[0];
    const std::string salt = generatePasswordSalt();
    const std::string hash = passwordHash(message.fields[1], salt);

    std::string storageError;
    if (!messageStore_.registerUser(username, salt, hash, storageError)) {
        sendToSession(session, {common::CommandType::RegisterResult, {"error", storageError}});
        logEvent("user_register_failed username=" + username +
                 " error=\"" + escapeLogField(storageError) + "\"");
        return;
    }

    logEvent("user_registered username=" + username + " peer=" + session->peer);
    sendToSession(session, {common::CommandType::RegisterResult,
                            {"ok", "account created for " + username}});
}

void MessengerServer::handleSendMessage(const std::shared_ptr<ClientSession>& session,
                                        const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }

    if ((message.fields.size() != 2 && message.fields.size() != 3) ||
        message.fields[0].empty() ||
        message.fields[1].empty()) {
        sendToSession(session, {common::CommandType::Error, {"usage: /msg <chat_id> <text> [reply_id]"}});
        return;
    }

    const std::string targetChatId = message.fields[0];
    const std::string text = message.fields[1];
    long long replyToMessageId = 0;
    if (message.fields.size() == 3 && !message.fields[2].empty() &&
        !parsePositiveInt64(message.fields[2], replyToMessageId)) {
        sendToSession(session, {common::CommandType::SendMessageResult, {"error", "invalid reply target"}});
        return;
    }

    long long groupId = 0;
    if (parseGroupChatIdValue(targetChatId, groupId)) {
        std::string storageError;
        StoredMessage storedMessage;
        std::string groupName;
        std::vector<std::string> memberUsernames;
        if (!messageStore_.saveGroupMessage(session->username,
                                            groupId,
                                            text,
                                            replyToMessageId,
                                            0,
                                            storedMessage,
                                            groupName,
                                            memberUsernames,
                                            storageError)) {
            sendToSession(session, {common::CommandType::SendMessageResult, {"error", storageError}});
            return;
        }

        std::vector<std::string> selfFields = {"ok"};
        const std::vector<std::string> baseFields = buildStoredMessageFields(storedMessage, targetChatId, groupName);
        selfFields.insert(selfFields.end(), baseFields.begin(), baseFields.end());
        sendToSession(session, {common::CommandType::SendMessageResult, selfFields});

        const std::vector<std::string> incomingFields = buildStoredMessageFields(storedMessage, targetChatId, groupName);

        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (const auto& username : memberUsernames) {
            if (username == session->username) {
                continue;
            }
            const auto it = onlineUsers_.find(username);
            if (it != onlineUsers_.end()) {
                sendToSession(it->second, {common::CommandType::IncomingMessage, incomingFields});
                handleFetchChats(it->second, {common::CommandType::FetchChats, {}});
            }
        }
        handleFetchChats(session, {common::CommandType::FetchChats, {}});
        return;
    }

    if (!isValidUsername(targetChatId)) {
        sendToSession(session, {common::CommandType::Error, {"invalid user"}}); 
        return;
    }

    const std::string recipientName = targetChatId;
    std::shared_ptr<ClientSession> recipient;

    {
        // Берем shared_ptr на сессию получателя под mutex-ом. После выхода из
        // блока объект сессии не исчезнет, даже если пользователь отключится,
        // потому что у нас остается своя копия shared_ptr.
        std::lock_guard<std::mutex> lock(clientsMutex_);
        const auto it = onlineUsers_.find(recipientName);
        if (it != onlineUsers_.end()) {
            recipient = it->second;
        }
    }

    logEvent("message_received from=" + session->username +
             " to=" + recipientName +
             (replyToMessageId > 0 ? " reply_to=" + std::to_string(replyToMessageId) : "") +
             " text=\"" + escapeLogField(text) + "\"");

    std::string storageError;
    StoredMessage storedMessage;
    if (!messageStore_.saveMessage(session->username,
                                   recipientName,
                                   text,
                                   replyToMessageId,
                                   0,
                                   storedMessage,
                                   storageError)) {
        logEvent("message_store_failed from=" + session->username +
                 " to=" + recipientName +
                 " error=\"" + escapeLogField(storageError) + "\"");
        sendToSession(session, {common::CommandType::SendMessageResult, {"error", storageError}});
        return;
    }

    const std::vector<std::string> messageFields = buildStoredMessageFields(storedMessage, recipientName, storedMessage.recipient);

    std::vector<std::string> selfFields = {"ok"};
    selfFields.insert(selfFields.end(), messageFields.begin(), messageFields.end());
    sendToSession(session, {common::CommandType::SendMessageResult, selfFields});

    if (!recipient) {
        sendToSession(session, {common::CommandType::Info, {"saved for offline user " + recipientName}});
        logEvent("message_saved_offline from=" + session->username + " to=" + recipientName);
        handleFetchChats(session, {common::CommandType::FetchChats, {}});
        return;
    }

    std::vector<std::string> recipientFields = messageFields;
    recipientFields[1] = session->username;

    if (!sendToSession(recipient, {common::CommandType::IncomingMessage, recipientFields})) {
        sendToSession(session, {common::CommandType::Info, {"saved; delivery will be available in history"}});
        logEvent("message_delivery_failed_saved from=" + session->username + " to=" + recipientName);
        return;
    }

    sendToSession(session, {common::CommandType::Info, {"delivered to " + recipientName}});
    logEvent("message_delivered from=" + session->username + " to=" + recipientName);
    handleFetchChats(session, {common::CommandType::FetchChats, {}});
    handleFetchChats(recipient, {common::CommandType::FetchChats, {}});
}

void MessengerServer::handleDeleteMessage(const std::shared_ptr<ClientSession>& session,
                                          const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }

    if (message.fields.size() != 1) {
        sendToSession(session, {common::CommandType::DeleteMessageResult, {"error", "usage: delete_message <message_id>"}});
        return;
    }

    long long messageId = 0;
    if (!parsePositiveInt64(message.fields[0], messageId)) {
        sendToSession(session, {common::CommandType::DeleteMessageResult, {"error", "invalid_message_id"}});
        return;
    }

    StoredMessage storedMessage;
    std::string resolvedChatId;
    std::vector<std::string> audienceUsernames;
    std::string storageError;
    if (!messageStore_.deleteMessage(session->username,
                                     messageId,
                                     storedMessage,
                                     resolvedChatId,
                                     audienceUsernames,
                                     storageError)) {
        sendToSession(session, {common::CommandType::DeleteMessageResult,
                                {"error", storageError.empty() ? "message_not_found" : storageError}});
        return;
    }

    sendToSession(session, {common::CommandType::DeleteMessageResult, {"ok", std::to_string(messageId)}});

    long long resolvedGroupId = 0;
    const bool isGroupMessage = !resolvedChatId.empty() && parseGroupChatIdValue(resolvedChatId, resolvedGroupId);
    std::set<std::string> delivered;
    for (const auto& username : audienceUsernames) {
        if (username.empty() || !delivered.insert(username).second) {
            continue;
        }

        std::shared_ptr<ClientSession> targetSession;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            const auto it = onlineUsers_.find(username);
            if (it != onlineUsers_.end()) {
                targetSession = it->second;
            }
        }

        if (!targetSession) {
            continue;
        }

        const std::string chatId = isGroupMessage ? resolvedChatId
                                  : (username == storedMessage.sender ? storedMessage.recipient : storedMessage.sender);
        const std::string recipientLabel = storedMessage.recipient;
        sendToSession(targetSession,
                      {common::CommandType::MessageDeleted,
                       buildStoredMessageFields(storedMessage, chatId, recipientLabel)});
        handleFetchChats(targetSession, {common::CommandType::FetchChats, {}});
    }

    logEvent("message_deleted user=" + session->username +
             " message_id=" + std::to_string(storedMessage.id));
}

void MessengerServer::handleForwardMessage(const std::shared_ptr<ClientSession>& session,
                                           const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }

    if (message.fields.size() < 3 || message.fields.size() > 4 || message.fields[0].empty() || message.fields[1].empty()) {
        sendToSession(session, {common::CommandType::SendMessageResult, {"error", "usage: /forward <target_chat> <source_chat> <message_id> [text]"}});
        return;
    }

    long long sourceMessageId = 0;
    if (!parsePositiveInt64(message.fields[2], sourceMessageId)) {
        sendToSession(session, {common::CommandType::SendMessageResult, {"error", "invalid forward target"}});
        return;
    }

    StoredMessage sourceMessage;
    std::string storageError;
    if (!messageStore_.loadAccessibleMessage(session->username, sourceMessageId, sourceMessage, storageError)) {
        sendToSession(session, {common::CommandType::SendMessageResult, {"error", storageError.empty() ? "cannot load source message" : storageError}});
        return;
    }

    if (sourceMessage.id == 0) {
        sendToSession(session, {common::CommandType::SendMessageResult, {"error", "forward target not found"}});
        return;
    }

    const std::string targetChatId = message.fields[0];
    const std::string sourceChatId = message.fields[1];
    const std::string commentText = message.fields.size() == 4 ? message.fields[3] : "";

    long long sourceGroupId = 0;
    if (parseGroupChatIdValue(sourceChatId, sourceGroupId) && sourceMessage.recipient.empty()) {
        sendToSession(session, {common::CommandType::SendMessageResult, {"error", "invalid source chat"}});
        return;
    }

    if (targetChatId == session->username) {
        sendToSession(session, {common::CommandType::SendMessageResult, {"error", "cannot forward messages to yourself"}});
        return;
    }

    long long targetGroupId = 0;
    if (parseGroupChatIdValue(targetChatId, targetGroupId)) {
        std::string groupName;
        std::vector<std::string> memberUsernames;
        StoredMessage storedMessage;
        if (!messageStore_.saveGroupMessage(session->username,
                                            targetGroupId,
                                            commentText,
                                            0,
                                            sourceMessage.id,
                                            storedMessage,
                                            groupName,
                                            memberUsernames,
                                            storageError)) {
            sendToSession(session, {common::CommandType::SendMessageResult, {"error", storageError}});
            return;
        }

        const std::vector<std::string> fields = buildStoredMessageFields(storedMessage, targetChatId, groupName);
        std::vector<std::string> senderFields = {"ok"};
        senderFields.insert(senderFields.end(), fields.begin(), fields.end());
        sendToSession(session, {common::CommandType::SendMessageResult, senderFields});
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (const auto& username : memberUsernames) {
            if (username == session->username) {
                continue;
            }
            const auto it = onlineUsers_.find(username);
            if (it != onlineUsers_.end()) {
                sendToSession(it->second, {common::CommandType::IncomingMessage, fields});
                handleFetchChats(it->second, {common::CommandType::FetchChats, {}});
            }
        }
        handleFetchChats(session, {common::CommandType::FetchChats, {}});
        return;
    }

    if (!isValidUsername(targetChatId)) {
        sendToSession(session, {common::CommandType::SendMessageResult, {"error", "invalid target chat"}});
        return;
    }

    const std::string recipientName = targetChatId;
    if (recipientName == session->username) {
        sendToSession(session, {common::CommandType::SendMessageResult, {"error", "cannot forward messages to yourself"}});
        return;
    }

    std::shared_ptr<ClientSession> recipient;
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        const auto it = onlineUsers_.find(recipientName);
        if (it != onlineUsers_.end()) {
            recipient = it->second;
        }
    }

    StoredMessage storedMessage;
    if (!messageStore_.saveMessage(session->username,
                                   recipientName,
                                   commentText,
                                   0,
                                   sourceMessage.id,
                                   storedMessage,
                                   storageError)) {
        sendToSession(session, {common::CommandType::SendMessageResult, {"error", storageError}});
        return;
    }

    std::vector<std::string> senderFields = {"ok"};
    const std::vector<std::string> forwardFields = buildStoredMessageFields(storedMessage, recipientName, storedMessage.recipient);
    senderFields.insert(senderFields.end(), forwardFields.begin(), forwardFields.end());
    sendToSession(session, {common::CommandType::SendMessageResult, senderFields});

    std::vector<std::string> incomingFields = forwardFields;
    incomingFields[1] = session->username;
    if (recipient) {
        sendToSession(recipient, {common::CommandType::IncomingMessage, incomingFields});
        handleFetchChats(recipient, {common::CommandType::FetchChats, {}});
    }
    handleFetchChats(session, {common::CommandType::FetchChats, {}});
}

void MessengerServer::handleFetchHistory(const std::shared_ptr<ClientSession>& session,
                                         const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }

    if (message.fields.empty() || message.fields.size() > 2 || message.fields[0].empty()) {
        sendToSession(session, {common::CommandType::Error, {"usage: /history <chat_id> [limit]"}});
        return;
    }

    int limit = kDefaultHistoryLimit;
    if (message.fields.size() == 2) {
        if (!parsePositiveInt(message.fields[1], limit)) {
            sendToSession(session, {common::CommandType::Error, {"history limit must be a positive number"}});
            return;
        }
    }

    if (limit > kMaxHistoryLimit) {
        limit = kMaxHistoryLimit;
    }

    std::vector<StoredMessage> history;
    std::string storageError;
    if (!messageStore_.fetchHistory(session->username, message.fields[0], limit, history, storageError)) {
        logEvent("history_fetch_failed user=" + session->username +
                 " peer=" + message.fields[0] +
                 " error=\"" + escapeLogField(storageError) + "\"");
        sendToSession(session, {common::CommandType::HistoryResult,
                                {"error", "storage error", message.fields[0], "latest"}});
        return;
    }

    for (const auto& item : history) {
        long long historyGroupId = 0;
        const std::string chatId = parseGroupChatIdValue(message.fields[0], historyGroupId) ? message.fields[0]
                                   : (item.sender == session->username ? item.recipient : item.sender);
        sendToSession(session, {common::CommandType::HistoryMessage,
                                buildStoredMessageFields(item, chatId, item.recipient)});
    }

    sendToSession(session, {common::CommandType::HistoryResult,
                            {"ok",
                             "history with " + message.fields[0] + ": " +
                                 std::to_string(history.size()) + " message(s)",
                             message.fields[0],
                             "latest"}});

    logEvent("history_fetched user=" + session->username +
             " peer=" + message.fields[0] +
             " count=" + std::to_string(history.size()));
}

void MessengerServer::handleFetchHistoryBefore(const std::shared_ptr<ClientSession>& session,
                                               const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }

    if (message.fields.size() != 3 || message.fields[0].empty()) {
        sendToSession(session, {common::CommandType::Error,
                                {"usage: fetch_history_before <chat_id> <before_message_id> <limit>"}});
        return;
    }

    long long beforeMessageId = 0;
    if (!parsePositiveInt64(message.fields[1], beforeMessageId)) {
        sendToSession(session, {common::CommandType::Error, {"before_message_id must be a positive number"}});
        return;
    }

    int limit = kDefaultHistoryLimit;
    if (!parsePositiveInt(message.fields[2], limit)) {
        sendToSession(session, {common::CommandType::Error, {"history limit must be a positive number"}});
        return;
    }

    if (limit > kMaxHistoryLimit) {
        limit = kMaxHistoryLimit;
    }

    std::vector<StoredMessage> history;
    std::string storageError;
    if (!messageStore_.fetchHistoryBefore(session->username,
                                          message.fields[0],
                                          beforeMessageId,
                                          limit,
                                          history,
                                          storageError)) {
        logEvent("history_before_fetch_failed user=" + session->username +
                 " peer=" + message.fields[0] +
                 " before=" + message.fields[1] +
                 " error=\"" + escapeLogField(storageError) + "\"");
        sendToSession(session, {common::CommandType::HistoryResult,
                                {"error", "storage error", message.fields[0], "older"}});
        return;
    }

    for (const auto& item : history) {
        long long historyGroupId = 0;
        const std::string chatId = parseGroupChatIdValue(message.fields[0], historyGroupId) ? message.fields[0]
                                   : (item.sender == session->username ? item.recipient : item.sender);
        sendToSession(session, {common::CommandType::HistoryMessage,
                                buildStoredMessageFields(item, chatId, item.recipient)});
    }

    sendToSession(session, {common::CommandType::HistoryResult,
                            {"ok",
                             "history before " + message.fields[0] + ": " +
                                 std::to_string(history.size()) + " message(s)",
                             message.fields[0],
                             "older"}});

    logEvent("history_before_fetched user=" + session->username +
             " peer=" + message.fields[0] +
             " before=" + message.fields[1] +
             " count=" + std::to_string(history.size()));
}

void MessengerServer::handleFetchChats(const std::shared_ptr<ClientSession>& session,
                                       const common::ProtocolMessage&) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }

    std::vector<ChatSummary> chats;
    std::string storageError;
    if (!messageStore_.fetchChats(session->username, chats, storageError)) {
        logEvent("chat_list_failed user=" + session->username +
                 " error=\"" + escapeLogField(storageError) + "\"");
        sendToSession(session, {common::CommandType::ChatListResult, {"error", "storage error"}});
        return;
    }

    for (const auto& chat : chats) {
        sendToSession(session, {common::CommandType::ChatItem,
                                {chat.peerId,
                                 chat.peer,
                                 chat.isGroup ? "group" : "dm",
                                 chat.lastAt,
                                 chat.lastSender,
                                 chat.lastText,
                                 std::to_string(chat.unreadCount),
                                 chat.canManage ? "1" : "0"}});
    }

    sendToSession(session, {common::CommandType::ChatListResult,
                            {"ok", std::to_string(chats.size()) + " chat(s)"}});
}

void MessengerServer::handleSearchUsers(const std::shared_ptr<ClientSession>& session,
                                        const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }

    if (message.fields.empty() || message.fields.size() > 3 || !isValidUsername(message.fields[0])) {
        const std::string query = message.fields.empty() ? "" : message.fields[0];
        sendToSession(session, {common::CommandType::UserSearchResult, {"error", "invalid search query", query}});
        return;
    }

    const std::string scope = message.fields.size() >= 2 ? message.fields[1] : "dm";
    const std::string scopeTarget = message.fields.size() >= 3 ? message.fields[2] : "";
    if (scope != "dm" && scope != "group_create" && scope != "group_add") {
        sendToSession(session, {common::CommandType::UserSearchResult, {"error", "invalid search scope", message.fields[0]}});
        return;
    }

    std::vector<UserSearchResult> users;
    std::string storageError;
    if (!messageStore_.searchUsers(message.fields[0],
                                   session->username,
                                   scope,
                                   scopeTarget,
                                   kMaxUserSearchResults,
                                   users,
                                   storageError)) {
        logEvent("user_search_failed user=" + session->username +
                 " query=\"" + escapeLogField(message.fields[0]) +
                 "\" error=\"" + escapeLogField(storageError) + "\"");
        sendToSession(session, {common::CommandType::UserSearchResult, {"error", "storage error", message.fields[0]}});
        return;
    }

    for (const auto& user : users) {
        sendToSession(session, {common::CommandType::UserSearchItem,
                                {message.fields[0], user.id, user.username}});
    }

    sendToSession(session, {common::CommandType::UserSearchResult,
                            {"ok", std::to_string(users.size()) + " user(s)", message.fields[0]}});
}

void MessengerServer::handleCreateChat(const std::shared_ptr<ClientSession>& session,
                                       const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }

    long long peerId = 0;
    if (message.fields.size() != 1 || !parsePositiveInt64(message.fields[0], peerId)) {
        sendToSession(session, {common::CommandType::CreateChatResult, {"error", "invalid user id"}});
        return;
    }

    std::string peer;
    std::string storageError;
    if (!messageStore_.createConversation(session->username, peerId, peer, storageError)) {
        logEvent("chat_create_failed user=" + session->username +
                 " peer_id=" + message.fields[0] +
                 " error=\"" + escapeLogField(storageError) + "\"");
        sendToSession(session, {common::CommandType::CreateChatResult, {"error", storageError}});
        return;
    }

    sendToSession(session, {common::CommandType::CreateChatResult, {"ok", peer, peer}});
    handleFetchChats(session, {common::CommandType::FetchChats, {}});
    logEvent("chat_created user=" + session->username + " peer=" + peer);
}

void MessengerServer::handleCreateGroup(const std::shared_ptr<ClientSession>& session,
                                        const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }
    if (message.fields.size() != 3 || message.fields[0].empty() || !isValidUsername(message.fields[1])) {
        sendToSession(session, {common::CommandType::CreateGroupResult, {"error", "invalid group payload"}});
        return;
    }

    std::set<std::string> uniqueMembers;
    uniqueMembers.insert(session->username);
    for (const auto& username : splitCsv(message.fields[2])) {
        if (!isValidUsername(username)) {
            sendToSession(session, {common::CommandType::CreateGroupResult, {"error", "invalid member username"}});
            return;
        }
        uniqueMembers.insert(username);
    }

    if (uniqueMembers.find(message.fields[1]) == uniqueMembers.end()) {
        sendToSession(session, {common::CommandType::CreateGroupResult, {"error", "admin must be one of selected members"}});
        return;
    }

    const std::vector<std::string> members(uniqueMembers.begin(), uniqueMembers.end());
    long long groupId = 0;
    std::string storageError;
    if (!messageStore_.createGroup(session->username, message.fields[0], message.fields[1], members, groupId, storageError)) {
        sendToSession(session, {common::CommandType::CreateGroupResult, {"error", storageError}});
        return;
    }

    StoredMessage createdMessage;
    std::string groupName;
    std::vector<std::string> memberUsernames;
    messageStore_.saveGroupSystemMessage(groupId,
                                         session->username + " created the group",
                                         createdMessage,
                                         groupName,
                                         memberUsernames,
                                         storageError);

    sendToSession(session, {common::CommandType::CreateGroupResult, {"ok", makeGroupChatId(groupId), message.fields[0]}});
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (const auto& username : members) {
        const auto it = onlineUsers_.find(username);
        if (it != onlineUsers_.end()) {
            handleFetchChats(it->second, {common::CommandType::FetchChats, {}});
        }
    }
}

void MessengerServer::handleGetGroupInfo(const std::shared_ptr<ClientSession>& session,
                                         const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }
    long long groupId = 0;
    if (message.fields.size() != 1 || !parseGroupChatIdValue(message.fields[0], groupId)) {
        sendToSession(session, {common::CommandType::GroupInfoResult, {"error", "invalid group"}});
        return;
    }

    GroupInfo info;
    std::vector<GroupMemberInfo> members;
    std::string storageError;
    if (!messageStore_.fetchGroupInfo(session->username, groupId, info, members, storageError)) {
        sendToSession(session, {common::CommandType::GroupInfoResult, {"error", storageError}});
        return;
    }

    for (const auto& member : members) {
        sendToSession(session, {common::CommandType::GroupMemberItem,
                                {info.chatId, member.username, member.isAdmin ? "1" : "0"}});
    }
    sendToSession(session, {common::CommandType::GroupInfoResult,
                            {"ok", info.chatId, info.name, info.adminUsername, info.canManage ? "1" : "0"}});
}

void MessengerServer::handleAddGroupMembers(const std::shared_ptr<ClientSession>& session,
                                            const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }
    long long groupId = 0;
    if (message.fields.size() != 2 || !parseGroupChatIdValue(message.fields[0], groupId)) {
        sendToSession(session, {common::CommandType::GroupUpdateResult, {"error", "invalid group"}});
        return;
    }

    std::set<std::string> uniqueUsers;
    for (const auto& username : splitCsv(message.fields[1])) {
        if (!isValidUsername(username)) {
            sendToSession(session, {common::CommandType::GroupUpdateResult, {"error", "invalid member username"}});
            return;
        }
        uniqueUsers.insert(username);
    }

    std::vector<std::string> addedUsers;
    std::string storageError;
    std::vector<std::string> usernames(uniqueUsers.begin(), uniqueUsers.end());
    if (!messageStore_.addGroupMembers(session->username, groupId, usernames, addedUsers, storageError)) {
        sendToSession(session, {common::CommandType::GroupUpdateResult, {"error", storageError, message.fields[0]}});
        return;
    }

    for (const auto& username : addedUsers) {
        StoredMessage systemMessage;
        std::string groupName;
        std::vector<std::string> memberUsernames;
        messageStore_.saveGroupSystemMessage(groupId,
                                             session->username + " added " + username,
                                             systemMessage,
                                             groupName,
                                             memberUsernames,
                                             storageError);
        const std::vector<std::string> incomingFields = {
            std::to_string(systemMessage.id), message.fields[0], systemMessage.createdAt, systemMessage.sender,
            groupName, systemMessage.text, "", "", "", "", "", ""
        };
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (const auto& member : memberUsernames) {
            const auto it = onlineUsers_.find(member);
            if (it != onlineUsers_.end()) {
                if (member != session->username) {
                    sendToSession(it->second, {common::CommandType::IncomingMessage, incomingFields});
                }
                handleFetchChats(it->second, {common::CommandType::FetchChats, {}});
            }
        }
    }

    sendToSession(session, {common::CommandType::GroupUpdateResult, {"ok", "members added", message.fields[0]}});
}

void MessengerServer::handleRemoveGroupMember(const std::shared_ptr<ClientSession>& session,
                                              const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }
    long long groupId = 0;
    if (message.fields.size() != 2 || !parseGroupChatIdValue(message.fields[0], groupId) || !isValidUsername(message.fields[1])) {
        sendToSession(session, {common::CommandType::GroupUpdateResult, {"error", "invalid request"}});
        return;
    }
    std::string storageError;
    if (!messageStore_.removeGroupMember(session->username, groupId, message.fields[1], storageError)) {
        sendToSession(session, {common::CommandType::GroupUpdateResult, {"error", storageError, message.fields[0]}});
        return;
    }

    StoredMessage systemMessage;
    std::string groupName;
    std::vector<std::string> memberUsernames;
    messageStore_.saveGroupSystemMessage(groupId,
                                         session->username + " removed " + message.fields[1],
                                         systemMessage,
                                         groupName,
                                         memberUsernames,
                                         storageError);
    const std::vector<std::string> incomingFields = {
        std::to_string(systemMessage.id), message.fields[0], systemMessage.createdAt, systemMessage.sender,
        groupName, systemMessage.text, "", "", "", "", "", ""
    };
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (const auto& member : memberUsernames) {
        const auto it = onlineUsers_.find(member);
        if (it != onlineUsers_.end()) {
            sendToSession(it->second, {common::CommandType::IncomingMessage, incomingFields});
            handleFetchChats(it->second, {common::CommandType::FetchChats, {}});
        }
    }
    const auto removedIt = onlineUsers_.find(message.fields[1]);
    if (removedIt != onlineUsers_.end()) {
        sendToSession(removedIt->second, {common::CommandType::ChatRemoved, {message.fields[0], "removed from group"}});
        handleFetchChats(removedIt->second, {common::CommandType::FetchChats, {}});
    }
    sendToSession(session, {common::CommandType::GroupUpdateResult, {"ok", "member removed", message.fields[0]}});
}

void MessengerServer::handleTransferGroupAdmin(const std::shared_ptr<ClientSession>& session,
                                               const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }
    long long groupId = 0;
    if (message.fields.size() != 2 || !parseGroupChatIdValue(message.fields[0], groupId) || !isValidUsername(message.fields[1])) {
        sendToSession(session, {common::CommandType::GroupUpdateResult, {"error", "invalid request"}});
        return;
    }
    std::string storageError;
    if (!messageStore_.transferGroupAdmin(session->username, groupId, message.fields[1], storageError)) {
        sendToSession(session, {common::CommandType::GroupUpdateResult, {"error", storageError, message.fields[0]}});
        return;
    }
    StoredMessage systemMessage;
    std::string groupName;
    std::vector<std::string> memberUsernames;
    messageStore_.saveGroupSystemMessage(groupId,
                                         session->username + " transferred admin to " + message.fields[1],
                                         systemMessage,
                                         groupName,
                                         memberUsernames,
                                         storageError);
    const std::vector<std::string> incomingFields = {
        std::to_string(systemMessage.id), message.fields[0], systemMessage.createdAt, systemMessage.sender,
        groupName, systemMessage.text, "", "", "", "", "", ""
    };
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (const auto& member : memberUsernames) {
        const auto it = onlineUsers_.find(member);
        if (it != onlineUsers_.end()) {
            sendToSession(it->second, {common::CommandType::IncomingMessage, incomingFields});
            handleFetchChats(it->second, {common::CommandType::FetchChats, {}});
        }
    }
    sendToSession(session, {common::CommandType::GroupUpdateResult, {"ok", "admin transferred", message.fields[0]}});
}

void MessengerServer::handleLeaveGroup(const std::shared_ptr<ClientSession>& session,
                                       const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }
    long long groupId = 0;
    if (message.fields.size() != 1 || !parseGroupChatIdValue(message.fields[0], groupId)) {
        sendToSession(session, {common::CommandType::GroupUpdateResult, {"error", "invalid group"}});
        return;
    }
    std::string storageError;
    if (!messageStore_.leaveGroup(session->username, groupId, storageError)) {
        sendToSession(session, {common::CommandType::GroupUpdateResult, {"error", storageError, message.fields[0]}});
        return;
    }
    StoredMessage systemMessage;
    std::string groupName;
    std::vector<std::string> memberUsernames;
    messageStore_.saveGroupSystemMessage(groupId,
                                         session->username + " left the group",
                                         systemMessage,
                                         groupName,
                                         memberUsernames,
                                         storageError);
    const std::vector<std::string> incomingFields = {
        std::to_string(systemMessage.id), message.fields[0], systemMessage.createdAt, systemMessage.sender,
        groupName, systemMessage.text, "", "", "", "", "", ""
    };
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (const auto& member : memberUsernames) {
            const auto it = onlineUsers_.find(member);
            if (it != onlineUsers_.end()) {
                sendToSession(it->second, {common::CommandType::IncomingMessage, incomingFields});
                handleFetchChats(it->second, {common::CommandType::FetchChats, {}});
            }
        }
        const auto selfIt = onlineUsers_.find(session->username);
        if (selfIt != onlineUsers_.end()) {
            sendToSession(selfIt->second, {common::CommandType::ChatRemoved, {message.fields[0], "left group"}});
            handleFetchChats(selfIt->second, {common::CommandType::FetchChats, {}});
        }
    }
    sendToSession(session, {common::CommandType::GroupUpdateResult, {"ok", "left group", message.fields[0]}});
}

void MessengerServer::handleDeleteGroup(const std::shared_ptr<ClientSession>& session,
                                        const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }
    long long groupId = 0;
    if (message.fields.size() != 1 || !parseGroupChatIdValue(message.fields[0], groupId)) {
        sendToSession(session, {common::CommandType::GroupUpdateResult, {"error", "invalid group"}});
        return;
    }

    std::vector<std::string> removedUsers;
    std::string storageError;
    if (!messageStore_.deleteGroup(session->username, groupId, removedUsers, storageError)) {
        sendToSession(session, {common::CommandType::GroupUpdateResult, {"error", storageError, message.fields[0]}});
        return;
    }
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (const auto& username : removedUsers) {
        const auto it = onlineUsers_.find(username);
        if (it != onlineUsers_.end()) {
            sendToSession(it->second, {common::CommandType::ChatRemoved, {message.fields[0], "group deleted"}});
            handleFetchChats(it->second, {common::CommandType::FetchChats, {}});
        }
    }
    sendToSession(session, {common::CommandType::GroupUpdateResult, {"ok", "group deleted", message.fields[0]}});
}

void MessengerServer::handleMarkRead(const std::shared_ptr<ClientSession>& session,
                                     const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }

    if (message.fields.size() != 1 || message.fields[0].empty()) {
        sendToSession(session, {common::CommandType::MarkReadResult, {"error", "invalid chat"}});
        return;
    }

    std::string storageError;
    if (!messageStore_.markConversationRead(session->username, message.fields[0], storageError)) {
        logEvent("chat_mark_read_failed user=" + session->username +
                 " peer=" + message.fields[0] +
                 " error=\"" + escapeLogField(storageError) + "\"");
        sendToSession(session, {common::CommandType::MarkReadResult, {"error", "storage error"}});
        return;
    }

    sendToSession(session, {common::CommandType::MarkReadResult, {"ok", message.fields[0]}});
}

void MessengerServer::handleDeleteChat(const std::shared_ptr<ClientSession>& session,
                                       const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }

    if (message.fields.size() != 1 || message.fields[0].empty()) {
        sendToSession(session, {common::CommandType::DeleteChatResult, {"error", "invalid chat"}});
        return;
    }

    const std::string peer = message.fields[0];
    long long groupId = 0;
    if (parseGroupChatIdValue(peer, groupId)) {
        sendToSession(session, {common::CommandType::DeleteChatResult, {"error", "group actions are available in group settings", peer}});
        return;
    }
    std::string storageError;
    if (!messageStore_.deleteConversation(session->username, peer, storageError)) {
        logEvent("chat_delete_failed user=" + session->username +
                 " peer=" + peer +
                 " error=\"" + escapeLogField(storageError) + "\"");
        sendToSession(session, {common::CommandType::DeleteChatResult, {"error", storageError, peer}});
        return;
    }

    logEvent("chat_deleted user=" + session->username + " peer=" + peer);
    sendToSession(session, {common::CommandType::DeleteChatResult, {"ok", "deleted", peer}});
    handleFetchChats(session, {common::CommandType::FetchChats, {}});
}

void MessengerServer::removeSession(const std::shared_ptr<ClientSession>& session) {
    session->active = false;

    if (!session->username.empty()) {
        // Удаляем пользователя из onlineUsers_ только если map все еще указывает
        // именно на эту сессию. Это защищает от редких случаев повторного логина.
        std::lock_guard<std::mutex> lock(clientsMutex_);
        const auto it = onlineUsers_.find(session->username);
        if (it != onlineUsers_.end() && it->second == session) {
            onlineUsers_.erase(it);
        }
    }

    closeSessionSocket(session);

    logEvent("client_disconnected username=" +
             (session->username.empty() ? "<anonymous>" : session->username) +
             " peer=" + session->peer);
}

void MessengerServer::closeSessionSocket(const std::shared_ptr<ClientSession>& session) {
    if (!session) {
        return;
    }

    std::lock_guard<std::mutex> lock(session->writeMutex);
    if (session->ssl) {
        SSL_free(session->ssl);
        session->ssl = nullptr;
    }
    if (session->socket >= 0) {
        // shutdown будит поток, который мог ждать данные в recv(). close
        // освобождает файловый дескриптор в ОС.
        ::shutdown(session->socket, SHUT_RDWR);
        ::close(session->socket);
        session->socket = -1;
    }
}

void MessengerServer::logEvent(const std::string& event) {
    // Лог общий для всех потоков, поэтому каждая запись защищена mutex-ом:
    // строки событий не перемешиваются между собой.
    std::lock_guard<std::mutex> lock(logMutex_);
    logFile_ << currentTimestamp() << ' ' << event << '\n';
    logFile_.flush();
}

bool MessengerServer::isValidUsername(const std::string& username) {
    if (username.empty() || username.size() > 32) {
        return false;
    }

    for (char ch : username) {
        const bool allowed = (ch >= 'a' && ch <= 'z') ||
                             (ch >= 'A' && ch <= 'Z') ||
                             (ch >= '0' && ch <= '9') ||
                             ch == '_' ||
                             ch == '-';
        if (!allowed) {
            return false;
        }
    }

    return true;
}

bool MessengerServer::isValidPassword(const std::string& password) {
    return password.size() >= 4 && password.size() <= 128;
}

std::string MessengerServer::generatePasswordSalt() {
    std::random_device randomDevice;
    std::mt19937_64 generator(randomDevice());
    std::uniform_int_distribution<unsigned long long> distribution;

    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(16) << distribution(generator)
        << std::setw(16) << distribution(generator);
    return out.str();
}

std::string MessengerServer::passwordHash(const std::string& password, const std::string& salt) {
    // This is a salted FNV-1a based hash suitable for avoiding plain-text
    // passwords in this coursework project. For production use, replace it with
    // Argon2, bcrypt, scrypt or PBKDF2 from a vetted crypto library.
    unsigned long long hash = 1469598103934665603ULL;
    const std::string data = salt + '\n' + password;

    for (int round = 0; round < 10000; ++round) {
        for (unsigned char ch : data) {
            hash ^= ch;
            hash *= 1099511628211ULL;
        }

        hash ^= static_cast<unsigned long long>(round);
        hash *= 1099511628211ULL;
    }

    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

std::string MessengerServer::currentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};

#if defined(_WIN32)
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif

    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
    return buffer;
}
