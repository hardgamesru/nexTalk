#include "MessengerServer.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
    constexpr int kBacklog = 16;
    constexpr std::size_t kMaxLineLength = 8192;

    // send() не обязан отправить всю строку за один вызов. Поэтому sendAll
    // повторяет отправку, пока все байты протокольного сообщения не уйдут в TCP.
    bool sendAll(int socket, const std::string& data) {
        std::size_t sent = 0;

        while (sent < data.size()) {
            const ssize_t result = ::send(socket, data.data() + sent, data.size() - sent, 0);
            if (result <= 0) {
                return false;
            }

            sent += static_cast<std::size_t>(result);
        }

        return true;
    }

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
}

MessengerServer::MessengerServer(int port, std::string bindAddress, std::string logPath)
    : port_(port),
      bindAddress_(std::move(bindAddress)),
      logPath_(std::move(logPath)) {
}

MessengerServer::~MessengerServer() {
    stop();
}

bool MessengerServer::start() {
    // Лог открывается в режиме append: события сохраняются между запусками
    // сервера, что требуется в задании преподавателя.
    logFile_.open(logPath_, std::ios::app);
    if (!logFile_) {
        std::cerr << "Cannot open log file: " << logPath_ << '\n';
        return false;
    }

    // Создаем TCP/IPv4 socket. Это низкоуровневый дескриптор, через который
    // ОС будет принимать входящие сетевые подключения.
    listenSocket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket_ < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << '\n';
        return false;
    }

    // SO_REUSEADDR позволяет быстро перезапустить сервер на том же порту после
    // остановки, не дожидаясь, пока ОС полностью освободит старое соединение.
    int enabled = 1;
    if (::setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        std::cerr << "setsockopt failed: " << std::strerror(errno) << '\n';
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port_));

    if (::inet_pton(AF_INET, bindAddress_.c_str(), &address.sin_addr) != 1) {
        std::cerr << "Invalid bind address: " << bindAddress_ << '\n';
        return false;
    }

    // bind связывает socket с конкретным IP-адресом и портом.
    // 127.0.0.1 подходит для локального режима, 0.0.0.0 - для сетевого.
    if (::bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::cerr << "bind failed: " << std::strerror(errno) << '\n';
        return false;
    }

    // После listen socket становится "слушающим": ОС начинает ставить входящие
    // подключения в очередь, откуда сервер забирает их через accept().
    if (::listen(listenSocket_, kBacklog) < 0) {
        std::cerr << "listen failed: " << std::strerror(errno) << '\n';
        return false;
    }

    running_ = true;
    logEvent("server_started port=" + std::to_string(port_) + " bind=" + bindAddress_);
    std::cout << "NexTalk server listening on " << bindAddress_ << ':' << port_ << '\n';
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
    if (session->socket < 0) {
        return false;
    }

    return sendAll(session->socket, line);
}

bool MessengerServer::readLine(int socket, std::string& line) {
    line.clear();
    char ch = '\0';

    // TCP - поток байтов, он не хранит границы сообщений. Мы читаем по одному
    // байту до '\n', потому что наш протокол задает "одно сообщение = одна строка".
    while (line.size() < kMaxLineLength) {
        const ssize_t result = ::recv(socket, &ch, 1, 0);
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

void MessengerServer::handleClient(std::shared_ptr<ClientSession> session) {
    sendToSession(session, {common::CommandType::Info, {"welcome to NexTalk"}});

    std::string line;
    while (running_ && session->active && readLine(session->socket, line)) {
        common::ProtocolMessage message;

        // Все входящие строки проходят через общий парсер протокола из common.
        // Благодаря этому сервер и клиент используют один и тот же формат.
        if (!common::parseMessage(line, message)) {
            sendToSession(session, {common::CommandType::Error, {"invalid protocol message"}});
            continue;
        }

        switch (message.type) {
        case common::CommandType::Login:
            handleLogin(session, message);
            break;
        case common::CommandType::SendMessage:
            handleSendMessage(session, message);
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
    if (message.fields.size() != 1 || !isValidUsername(message.fields[0])) {
        sendToSession(session, {common::CommandType::LoginResult, {"error", "invalid username"}});
        return;
    }

    const std::string username = message.fields[0];

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
}

void MessengerServer::handleSendMessage(const std::shared_ptr<ClientSession>& session,
                                        const common::ProtocolMessage& message) {
    if (session->username.empty()) {
        sendToSession(session, {common::CommandType::Error, {"login first"}});
        return;
    }

    if (message.fields.size() != 2 || message.fields[0].empty() || message.fields[1].empty()) {
        sendToSession(session, {common::CommandType::Error, {"usage: /msg <user> <text>"}});
        return;
    }

    const std::string recipientName = message.fields[0];
    const std::string text = message.fields[1];
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
             " text=\"" + escapeLogField(text) + "\"");

    if (!recipient) {
        sendToSession(session, {common::CommandType::Error, {"user is offline: " + recipientName}});
        logEvent("message_not_delivered from=" + session->username + " to=" + recipientName);
        return;
    }

    if (!sendToSession(recipient, {common::CommandType::IncomingMessage, {session->username, text}})) {
        sendToSession(session, {common::CommandType::Error, {"delivery failed"}});
        logEvent("message_delivery_failed from=" + session->username + " to=" + recipientName);
        return;
    }

    sendToSession(session, {common::CommandType::Info, {"delivered to " + recipientName}});
    logEvent("message_delivered from=" + session->username + " to=" + recipientName);
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
