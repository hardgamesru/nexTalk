#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include "MessengerServer.hpp"

namespace {
    volatile std::sig_atomic_t g_stopRequested = 0;

    void handleSignal(int) {
        // В обработчике сигнала нельзя безопасно выполнять сложную логику:
        // закрывать mutex-и, писать в файл или работать с std::cout. Поэтому
        // только выставляем флаг, а реальная остановка произойдет в main loop.
        g_stopRequested = 1;
    }

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
}

int main(int argc, char* argv[]) {
    // Аргументы запуска:
    //   server [port] [bind_address] [log_path] [db_path]
    //
    // Пример локального режима:
    //   server 5555 127.0.0.1 server.log nextalk.db
    //
    // Пример сетевого режима:
    //   server 5555 0.0.0.0 server.log nextalk.db
    const int port = argc > 1 ? parsePort(argv[1]) : 5555;
    if (port < 0) {
        std::cerr << "Usage: server [port] [bind_address] [log_path] [db_path]\n";
        return 1;
    }

    const std::string bindAddress = argc > 2 ? argv[2] : "0.0.0.0";
    const std::string logPath = argc > 3 ? argv[3] : "server.log";
    const std::string dbPath = argc > 4 ? argv[4] : "nextalk.db";

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
#if defined(SIGPIPE)
    // Если клиент резко отключился, запись в его socket может породить SIGPIPE.
    // Игнорируем этот сигнал и обрабатываем ошибку через возвращаемое значение send().
    std::signal(SIGPIPE, SIG_IGN);
#endif

    MessengerServer server(port, bindAddress, logPath, dbPath);
    if (!server.start()) {
        return 1;
    }

    server.run([] {
        return g_stopRequested != 0;
    });

    return 0;
}
