#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "common/Protocol.hpp"

namespace {
    constexpr std::size_t kMaxLineLength = 8192;

    // Консоль общая для двух потоков клиента: главный поток печатает подсказку
    // и ошибки ввода, receiver thread печатает входящие сообщения от сервера.
    std::mutex g_outputMutex;

    int parsePort(const char* value) {
        const int port = std::atoi(value);
        if (port <= 0 || port > 65535) {
            return -1;
        }

        return port;
    }

    bool sendAll(int socket, const std::string& data) {
        std::size_t sent = 0;

        // send() может отправить только часть буфера. Цикл гарантирует, что
        // вся строка протокола будет записана в TCP-соединение.
        while (sent < data.size()) {
            const ssize_t result = ::send(socket, data.data() + sent, data.size() - sent, 0);
            if (result <= 0) {
                return false;
            }

            sent += static_cast<std::size_t>(result);
        }

        return true;
    }

    bool sendMessage(int socket, const common::ProtocolMessage& message) {
        return sendAll(socket, common::serializeMessage(message));
    }

    bool readLine(int socket, std::string& line) {
        line.clear();
        char ch = '\0';

        // Как и на сервере, TCP не знает границ сообщений. Клиент читает поток
        // до '\n', потому что протокол NexTalk передает одно сообщение в строке.
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

    int connectToServer(const std::string& host, int port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        // getaddrinfo позволяет подключаться и по IP, и по hostname. AF_UNSPEC
        // оставляет возможность использовать IPv4 или IPv6, если ОС их вернет.
        addrinfo* results = nullptr;
        const std::string service = std::to_string(port);
        const int lookup = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
        if (lookup != 0) {
            std::cerr << "getaddrinfo failed: " << gai_strerror(lookup) << '\n';
            return -1;
        }

        int socket = -1;
        for (addrinfo* current = results; current != nullptr; current = current->ai_next) {
            // Перебираем все адреса, которые вернула ОС, пока один из вариантов
            // не позволит создать socket и подключиться к серверу.
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
        return socket;
    }

    void printHelp() {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout
            << "Commands:\n"
            << "  /msg <user> <text>  send private message\n"
            << "  /login <user>       change login\n"
            << "  /help               show commands\n"
            << "  /quit               exit\n";
    }

    void printPrompt() {
        std::cout << "> " << std::flush;
    }

    void receiverLoop(int socket, std::atomic<bool>& running) {
        std::string line;

        // Этот цикл работает во втором потоке клиента. Благодаря ему клиент
        // получает входящие сообщения сразу, даже если пользователь ничего не
        // вводит в консоль.
        while (running && readLine(socket, line)) {
            common::ProtocolMessage message;
            if (!common::parseMessage(line, message)) {
                std::lock_guard<std::mutex> lock(g_outputMutex);
                std::cout << "\n[protocol] invalid message\n";
                printPrompt();
                continue;
            }

            std::lock_guard<std::mutex> lock(g_outputMutex);
            switch (message.type) {
            case common::CommandType::Info:
                if (!message.fields.empty()) {
                    std::cout << "\n[info] " << message.fields[0] << '\n';
                }
                break;
            case common::CommandType::LoginResult:
                if (message.fields.size() >= 2) {
                    std::cout << "\n[login] " << message.fields[0] << ": " << message.fields[1] << '\n';
                }
                break;
            case common::CommandType::IncomingMessage:
                if (message.fields.size() >= 2) {
                    std::cout << "\n[from " << message.fields[0] << "] " << message.fields[1] << '\n';
                }
                break;
            case common::CommandType::Error:
                if (!message.fields.empty()) {
                    std::cout << "\n[error] " << message.fields[0] << '\n';
                }
                break;
            default:
                std::cout << "\n[server] unsupported message\n";
                break;
            }

            // После асинхронного входящего сообщения возвращаем приглашение
            // ввода, чтобы пользователю было понятно, что клиент ждет команду.
            printPrompt();
        }

        // Если сервер закрыл соединение или произошла ошибка чтения, поток
        // сообщает главному циклу, что клиент должен завершиться.
        running = false;
        ::shutdown(socket, SHUT_RDWR);

        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "\nDisconnected from server\n";
    }

    bool startsWith(const std::string& value, const std::string& prefix) {
        return value.size() >= prefix.size() &&
               value.compare(0, prefix.size(), prefix) == 0;
    }
}

int main(int argc, char* argv[]) {
#if defined(SIGPIPE)
    // Защита от аварийного завершения процесса при записи в закрытый socket.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // Аргументы запуска:
    //   client [host] [port]
    //
    // Для локального режима:
    //   client 127.0.0.1 5555
    //
    // Для сетевого режима:
    //   client <ip-сервера> 5555
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const int port = argc > 2 ? parsePort(argv[2]) : 5555;
    if (port < 0) {
        std::cerr << "Usage: client [host] [port]\n";
        return 1;
    }

    const int socket = connectToServer(host, port);
    if (socket < 0) {
        std::cerr << "Cannot connect to " << host << ':' << port << '\n';
        return 1;
    }

    std::atomic<bool> running{true};

    // Главный поток дальше читает ввод пользователя, а receiver thread в фоне
    // читает сообщения от сервера. Это закрывает требование о многопоточности
    // клиента: отправка и прием не блокируют друг друга.
    std::thread receiver(receiverLoop, socket, std::ref(running));

    {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "Connected to " << host << ':' << port << '\n';
        std::cout << "Login: " << std::flush;
    }

    std::string login;
    while (running && std::getline(std::cin, login)) {
        if (!login.empty()) {
            break;
        }

        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "Login cannot be empty. Login: " << std::flush;
    }

    if (!running || login.empty()) {
        running = false;
        ::shutdown(socket, SHUT_RDWR);
        if (receiver.joinable()) {
            receiver.join();
        }
        ::close(socket);
        return 0;
    }

    // Первое введенное пользователем значение считаем логином и отправляем
    // серверу команду login.
    sendMessage(socket, {common::CommandType::Login, {login}});
    printHelp();

    std::string input;
    {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        printPrompt();
    }

    while (running && std::getline(std::cin, input)) {
        if (input.empty()) {
            std::lock_guard<std::mutex> lock(g_outputMutex);
            printPrompt();
            continue;
        }

        if (input == "/quit") {
            // Сообщаем серверу о корректном завершении сессии и выходим из
            // главного цикла клиента.
            sendMessage(socket, {common::CommandType::Quit, {}});
            running = false;
            break;
        }

        if (input == "/help") {
            printHelp();
            std::lock_guard<std::mutex> lock(g_outputMutex);
            printPrompt();
            continue;
        }

        if (startsWith(input, "/login ")) {
            // Повторный login позволяет сменить имя пользователя в текущей
            // TCP-сессии, если новое имя свободно.
            const std::string username = input.substr(7);
            sendMessage(socket, {common::CommandType::Login, {username}});
            std::lock_guard<std::mutex> lock(g_outputMutex);
            printPrompt();
            continue;
        }

        if (startsWith(input, "/msg ")) {
            // Команда пользователя "/msg bob hello" превращается в протокольную
            // команду send_message с двумя полями: получатель и текст.
            const std::string rest = input.substr(5);
            const std::size_t separator = rest.find(' ');
            if (separator == std::string::npos || separator + 1 >= rest.size()) {
                std::lock_guard<std::mutex> lock(g_outputMutex);
                std::cout << "Usage: /msg <user> <text>\n";
                printPrompt();
                continue;
            }

            const std::string recipient = rest.substr(0, separator);
            const std::string text = rest.substr(separator + 1);
            sendMessage(socket, {common::CommandType::SendMessage, {recipient, text}});

            std::lock_guard<std::mutex> lock(g_outputMutex);
            printPrompt();
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_outputMutex);
            std::cout << "Unknown command. Type /help\n";
            printPrompt();
        }
    }

    running = false;

    // shutdown разбудит receiver thread, если тот сейчас заблокирован в recv().
    // После этого можно безопасно дождаться завершения потока через join().
    ::shutdown(socket, SHUT_RDWR);
    if (receiver.joinable()) {
        receiver.join();
    }

    ::close(socket);
    return 0;
}
