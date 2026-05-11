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

struct ChatSummary {
    std::string peerId;
    std::string peer;
    std::string lastAt;
    std::string lastSender;
    std::string lastText;
};

struct UserSearchResult {
    std::string id;
    std::string username;
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

    bool registerUser(const std::string& username,
                      const std::string& passwordSalt,
                      const std::string& passwordHash,
                      std::string& error);
    bool authenticateUser(const std::string& username,
                          const std::string& passwordHash,
                          bool& ok,
                          std::string& error);
    bool loadPasswordSalt(const std::string& username,
                          std::string& passwordSalt,
                          std::string& error);
    bool userExists(const std::string& username, bool& exists, std::string& error);
    bool saveMessage(const std::string& sender,
                     const std::string& recipient,
                     const std::string& text,
                     std::string& error);
    bool fetchHistory(const std::string& user,
                      const std::string& peer,
                      int limit,
                      std::vector<StoredMessage>& messages,
                      std::string& error);
    bool fetchChats(const std::string& user,
                    std::vector<ChatSummary>& chats,
                    std::string& error);
    bool searchUsers(const std::string& query,
                     const std::string& excludeUser,
                     int limit,
                     std::vector<UserSearchResult>& users,
                     std::string& error);
    bool createConversation(const std::string& user,
                            long long peerId,
                            std::string& peerUsername,
                            std::string& error);

private:
    bool executeLocked(const char* sql, std::string& error);
    bool ensureSchemaLocked(std::string& error);
    bool ensureColumnLocked(const std::string& table,
                            const std::string& column,
                            const std::string& definition,
                            std::string& error);
    bool userExistsLocked(const std::string& username, bool& exists, std::string& error);
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
