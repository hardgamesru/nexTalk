#pragma once

#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

/**
 * One persisted chat message loaded from SQLite.
 */
struct StoredMessage {
    std::string createdAt;
    std::string sender;
    std::string recipient;
    std::string text;
};

/**
 * Small SQLite wrapper for NexTalk server persistence.
 *
 * The server has one MessageStore shared by all client threads, so every public
 * operation takes a mutex before touching sqlite3.
 */
class MessageStore {
public:
    MessageStore() = default;
    ~MessageStore();

    MessageStore(const MessageStore&) = delete;
    MessageStore& operator=(const MessageStore&) = delete;

    bool open(const std::string& path, std::string& error);
    void close();

    bool ensureUser(const std::string& username, std::string& error);
    bool saveMessage(const std::string& sender,
                     const std::string& recipient,
                     const std::string& text,
                     std::string& error);
    bool fetchHistory(const std::string& user,
                      const std::string& peer,
                      int limit,
                      std::vector<StoredMessage>& messages,
                      std::string& error);

private:
    bool executeLocked(const char* sql, std::string& error);
    bool ensureSchemaLocked(std::string& error);
    bool ensureUserLocked(const std::string& username, std::string& error);
    bool findConversationLocked(const std::string& user,
                                const std::string& peer,
                                long long& conversationId,
                                std::string& error);
    bool getOrCreateConversationLocked(const std::string& user,
                                       const std::string& peer,
                                       long long& conversationId,
                                       std::string& error);
    void closeLocked();

    sqlite3* db_{nullptr};
    std::mutex mutex_;
};
