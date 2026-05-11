#include <atomic>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>

#include "client/ClientConnection.hpp"

namespace {
    std::mutex g_outputMutex;

    int parsePort(const char* value) {
        if (!value || value[0] == '\0') {
            return -1;
        }

        int port = 0;
        for (const char* current = value; *current != '\0'; ++current) {
            if (*current < '0' || *current > '9') {
                return -1;
            }

            port = port * 10 + (*current - '0');
            if (port > 65535) {
                return -1;
            }
        }

        return port > 0 ? port : -1;
    }

    bool startsWith(const std::string& value, const std::string& prefix) {
        return value.size() >= prefix.size() &&
               value.compare(0, prefix.size(), prefix) == 0;
    }

    void printPrompt() {
        std::cout << "> " << std::flush;
    }

    void printHelpUnlocked() {
        std::cout
            << "Commands:\n"
            << "  /msg <user> <text>  send private message\n"
            << "  /history <user> [n] show recent messages with user\n"
            << "  /chats              fetch chat list\n"
            << "  /help               show commands\n"
            << "  /quit               exit\n";
    }

    void printHelp() {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        printHelpUnlocked();
    }
}

int main(int argc, char* argv[]) {
#if defined(SIGPIPE)
    // Защита от аварийного завершения процесса при записи в закрытый socket.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const int port = argc > 2 ? parsePort(argv[2]) : 5555;
    if (port < 0) {
        std::cerr << "Usage: client [host] [port]\n";
        return 1;
    }

    std::atomic<bool> running{true};
    std::atomic<bool> userRequestedQuit{false};
    client::ClientConnection connection;

    client::ClientConnection::Callbacks callbacks;
    callbacks.onInfo = [](const std::string& text) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "\n[info] " << text << '\n';
        printPrompt();
    };
    callbacks.onError = [](const std::string& text) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "\n[error] " << text << '\n';
        printPrompt();
    };
    callbacks.onRegisterResult = [](const std::string& status, const std::string& text) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "\n[register] " << status << ": " << text << '\n';
        printPrompt();
    };
    callbacks.onLoginResult = [](const std::string& status, const std::string& text) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "\n[login] " << status << ": " << text << '\n';
        printPrompt();
    };
    callbacks.onIncomingMessage = [](const std::string& sender, const std::string& text) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "\n[from " << sender << "] " << text << '\n';
        printPrompt();
    };
    callbacks.onHistoryMessage = [](const client::HistoryItem& item) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "\n[history " << item.createdAt << "] "
                  << item.sender << " -> " << item.recipient
                  << ": " << item.text << '\n';
        printPrompt();
    };
    callbacks.onHistoryResult = [](const std::string& status, const std::string& text) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "\n[history] " << status << ": " << text << '\n';
        printPrompt();
    };
    callbacks.onChatItem = [](const client::ChatSummary& chat) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "\n[chat] " << chat.peer << " | "
                  << chat.lastAt << " | " << chat.lastSender
                  << ": " << chat.lastText << '\n';
        printPrompt();
    };
    callbacks.onChatListResult = [](const std::string& status, const std::string& text) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "\n[chats] " << status << ": " << text << '\n';
        printPrompt();
    };
    callbacks.onProtocolError = [](const std::string& text) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "\n[protocol] " << text << '\n';
        printPrompt();
    };
    callbacks.onDisconnected = [&] {
        running = false;
        if (!userRequestedQuit) {
            std::lock_guard<std::mutex> lock(g_outputMutex);
            std::cout << "\nDisconnected from server\n";
        }
    };
    connection.setCallbacks(std::move(callbacks));

    std::string error;
    if (!connection.connectToServer(host, port, error)) {
        std::cerr << "Cannot connect to " << host << ':' << port << ": " << error << '\n';
        return 1;
    }

    {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "Connected to " << host << ':' << port << '\n';
        std::cout << "Use existing account? [y/n]: " << std::flush;
    }

    std::string mode;
    std::getline(std::cin, mode);

    std::string login;
    std::string password;
    {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "Login: " << std::flush;
    }
    while (running && std::getline(std::cin, login)) {
        if (!login.empty()) {
            break;
        }

        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "Login cannot be empty. Login: " << std::flush;
    }

    if (!running || login.empty()) {
        connection.stop();
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        std::cout << "Password: " << std::flush;
    }

    if (!std::getline(std::cin, password) || password.empty()) {
        connection.stop();
        return 0;
    }

    if (!mode.empty() && (mode[0] == 'n' || mode[0] == 'N')) {
        connection.registerAccount(login, password);
    }
    connection.login(login, password);
    printHelp();

    {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        printPrompt();
    }

    std::string input;
    while (running && std::getline(std::cin, input)) {
        if (input.empty()) {
            std::lock_guard<std::mutex> lock(g_outputMutex);
            printPrompt();
            continue;
        }

        if (input == "/quit") {
            userRequestedQuit = true;
            connection.quit();
            running = false;
            break;
        }

        if (input == "/help") {
            std::lock_guard<std::mutex> lock(g_outputMutex);
            printHelpUnlocked();
            printPrompt();
            continue;
        }

        if (input == "/chats") {
            connection.fetchChats();
            std::lock_guard<std::mutex> lock(g_outputMutex);
            printPrompt();
            continue;
        }

        if (startsWith(input, "/msg ")) {
            const std::string rest = input.substr(5);
            const std::size_t separator = rest.find(' ');
            if (separator == std::string::npos || separator + 1 >= rest.size()) {
                std::lock_guard<std::mutex> lock(g_outputMutex);
                std::cout << "Usage: /msg <user> <text>\n";
                printPrompt();
                continue;
            }

            connection.sendPrivateMessage(rest.substr(0, separator), rest.substr(separator + 1));

            std::lock_guard<std::mutex> lock(g_outputMutex);
            printPrompt();
            continue;
        }

        if (startsWith(input, "/history ")) {
            const std::string rest = input.substr(9);
            const std::size_t separator = rest.find(' ');
            if (rest.empty()) {
                std::lock_guard<std::mutex> lock(g_outputMutex);
                std::cout << "Usage: /history <user> [limit]\n";
                printPrompt();
                continue;
            }

            if (separator == std::string::npos) {
                connection.fetchHistory(rest);
            } else {
                const std::string peer = rest.substr(0, separator);
                const std::string limit = rest.substr(separator + 1);
                if (peer.empty() || limit.empty()) {
                    std::lock_guard<std::mutex> lock(g_outputMutex);
                    std::cout << "Usage: /history <user> [limit]\n";
                    printPrompt();
                    continue;
                }

                connection.fetchHistory(peer, limit);
            }

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

    connection.stop();
    return 0;
}
