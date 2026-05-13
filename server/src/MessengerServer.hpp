#pragma once

#include <atomic>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <openssl/ssl.h>

#include "common/Protocol.hpp"
#include "MessageStore.hpp"

/**
 * TCP-сервер мессенджера.
 *
 * Обязанности класса:
 * - открыть listening socket и принимать подключения клиентов;
 * - создать отдельный поток обработки для каждого клиента;
 * - хранить активные сессии и таблицу пользователей онлайн;
 * - пересылать личные сообщения между пользователями;
 * - писать события сервера и сообщений в лог-файл.
 */
class MessengerServer {
public:
    MessengerServer(int port, std::string bindAddress, std::string logPath, std::string dbPath);
    ~MessengerServer();

    MessengerServer(const MessengerServer&) = delete;
    MessengerServer& operator=(const MessengerServer&) = delete;

    bool start();
    void run(const std::function<bool()>& shouldStop);
    void stop();

private:
    /**
     * Состояние одного подключенного клиента.
     *
     * Это объект прикладного уровня: он не заменяет сокет и не является потоком,
     * а связывает их с данными пользователя. После accept() сервер создает
     * ClientSession, кладет в нее socket и запускает поток handleClient(session).
     */
    struct ClientSession {
        // Файловый дескриптор TCP-соединения с клиентом.
        int socket{-1};

        // Адрес клиента в виде "ip:port"; используется для логов.
        std::string peer;

        // Имя пользователя после login. До логина строка пустая.
        std::string username;

        // Флаг активности читается и меняется из разных потоков.
        std::atomic<bool> active{true};

        // Защищает запись в socket этой сессии, чтобы два потока не смешали
        // свои сообщения в одном TCP-потоке байтов.
        std::mutex writeMutex;

        // TLS-состояние соединения поверх обычного TCP socket.
        SSL* ssl{nullptr};
    };

    /**
     * Отправляет одно протокольное сообщение конкретной сессии.
     */
    bool sendToSession(const std::shared_ptr<ClientSession>& session,
                       const common::ProtocolMessage& message);

    /**
     * Читает из TCP-сокета одну строку до '\n'.
     */
    bool readLine(const std::shared_ptr<ClientSession>& session, std::string& line);

    /**
     * Основной цикл обработки одного клиента. Выполняется в отдельном потоке.
     */
    void handleClient(std::shared_ptr<ClientSession> session);

    /**
     * Обрабатывает команду login и регистрирует пользователя в onlineUsers_.
     */
    void handleLogin(const std::shared_ptr<ClientSession>& session,
                     const common::ProtocolMessage& message);

    /**
     * Создает новый аккаунт пользователя.
     */
    void handleRegister(const std::shared_ptr<ClientSession>& session,
                        const common::ProtocolMessage& message);

    /**
     * Обрабатывает команду send_message и доставляет личное сообщение адресату.
     */
    void handleSendMessage(const std::shared_ptr<ClientSession>& session,
                           const common::ProtocolMessage& message);

    /**
     * Пересылает уже существующее сообщение в другой личный диалог.
     */
    void handleForwardMessage(const std::shared_ptr<ClientSession>& session,
                              const common::ProtocolMessage& message);

    /**
     * Обрабатывает команду fetch_history и отправляет клиенту последние
     * сообщения из SQLite.
     */
    void handleFetchHistory(const std::shared_ptr<ClientSession>& session,
                            const common::ProtocolMessage& message);

    /**
     * Обрабатывает команду fetch_history_before и подгружает более старые
     * сообщения относительно указанного message id.
     */
    void handleFetchHistoryBefore(const std::shared_ptr<ClientSession>& session,
                                  const common::ProtocolMessage& message);

    /**
     * Отправляет список диалогов пользователя с последними сообщениями.
     */
    void handleFetchChats(const std::shared_ptr<ClientSession>& session,
                          const common::ProtocolMessage& message);

    /**
     * Ищет пользователей по части имени для создания нового диалога.
     */
    void handleSearchUsers(const std::shared_ptr<ClientSession>& session,
                           const common::ProtocolMessage& message);

    /**
     * Создает пустой диалог с существующим пользователем.
     */
    void handleCreateChat(const std::shared_ptr<ClientSession>& session,
                          const common::ProtocolMessage& message);
    void handleCreateGroup(const std::shared_ptr<ClientSession>& session,
                           const common::ProtocolMessage& message);
    void handleGetGroupInfo(const std::shared_ptr<ClientSession>& session,
                            const common::ProtocolMessage& message);
    void handleAddGroupMembers(const std::shared_ptr<ClientSession>& session,
                               const common::ProtocolMessage& message);
    void handleRemoveGroupMember(const std::shared_ptr<ClientSession>& session,
                                 const common::ProtocolMessage& message);
    void handleTransferGroupAdmin(const std::shared_ptr<ClientSession>& session,
                                  const common::ProtocolMessage& message);
    void handleLeaveGroup(const std::shared_ptr<ClientSession>& session,
                          const common::ProtocolMessage& message);
    void handleDeleteGroup(const std::shared_ptr<ClientSession>& session,
                           const common::ProtocolMessage& message);

    /**
     * Помечает диалог как прочитанный для текущего пользователя.
     */
    void handleMarkRead(const std::shared_ptr<ClientSession>& session,
                        const common::ProtocolMessage& message);

    /**
     * Удаляет личный диалог и его историю.
     */
    void handleDeleteChat(const std::shared_ptr<ClientSession>& session,
                          const common::ProtocolMessage& message);

    /**
     * Удаляет сессию из таблицы онлайн-пользователей и закрывает ее сокет.
     */
    void removeSession(const std::shared_ptr<ClientSession>& session);

    /**
     * Аккуратно закрывает TCP-сокет сессии.
     */
    void closeSessionSocket(const std::shared_ptr<ClientSession>& session);

    /**
     * Пишет одну строку в лог-файл.
     */
    void logEvent(const std::string& event);
    static bool isValidUsername(const std::string& username);
    static bool isValidPassword(const std::string& password);
    static std::string generatePasswordSalt();
    static std::string passwordHash(const std::string& password, const std::string& salt);
    static std::string currentTimestamp();

    int port_;
    std::string bindAddress_;
    std::string logPath_;
    std::string dbPath_;
    int listenSocket_{-1};
    std::atomic<bool> running_{false};

    std::ofstream logFile_;
    std::mutex logMutex_;
    MessageStore messageStore_;
    SSL_CTX* sslContext_{nullptr};

    // Таблица "имя пользователя -> активная сессия". По ней сервер ищет, куда
    // доставлять личные сообщения. Доступ защищен clientsMutex_.
    std::mutex clientsMutex_;
    std::unordered_map<std::string, std::shared_ptr<ClientSession>> onlineUsers_;

    // Список всех сессий нужен для штатного завершения сервера: при Ctrl+C
    // сервер проходит по списку, уведомляет клиентов и закрывает сокеты.
    std::mutex sessionsMutex_;
    std::vector<std::shared_ptr<ClientSession>> sessions_;

    // Потоки обработки клиентов хранятся, чтобы при остановке сервера сделать
    // join и не оставить незавершенные std::thread.
    std::mutex threadsMutex_;
    std::vector<std::thread> clientThreads_;
};
