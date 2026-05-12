#include "MessageStore.hpp"

#include <sqlite3.h>
#include <utility>

namespace {
    class Statement {
    public:
        Statement(sqlite3* db, const char* sql, std::string& error) {
            const int result = sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr);
            if (result != SQLITE_OK) {
                error = sqlite3_errmsg(db);
                statement_ = nullptr;
            }
        }

        ~Statement() {
            if (statement_) {
                sqlite3_finalize(statement_);
            }
        }

        Statement(const Statement&) = delete;
        Statement& operator=(const Statement&) = delete;

        sqlite3_stmt* get() const {
            return statement_;
        }

        explicit operator bool() const {
            return statement_ != nullptr;
        }

    private:
        sqlite3_stmt* statement_{nullptr};
    };

    std::pair<std::string, std::string> orderedUsers(const std::string& user,
                                                     const std::string& peer) {
        if (user <= peer) {
            return {user, peer};
        }

        return {peer, user};
    }

    std::string lastError(sqlite3* db) {
        return sqlite3_errmsg(db) ? sqlite3_errmsg(db) : "sqlite error";
    }

    std::string escapeLikePattern(const std::string& value) {
        std::string escaped;
        escaped.reserve(value.size());

        for (char ch : value) {
            if (ch == '\\' || ch == '%' || ch == '_') {
                escaped += '\\';
            }
            escaped += ch;
        }

        return escaped;
    }
}

MessageStore::~MessageStore() {
    close();
}

bool MessageStore::open(const std::string& path, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (db_) {
        return true;
    }

    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        error = db_ ? lastError(db_) : "cannot open sqlite database";
        closeLocked();
        return false;
    }

    sqlite3_busy_timeout(db_, 3000);

    if (!executeLocked("PRAGMA foreign_keys = ON;", error)) {
        closeLocked();
        return false;
    }

    if (!ensureSchemaLocked(error)) {
        closeLocked();
        return false;
    }

    return true;
}

void MessageStore::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
}

void MessageStore::closeLocked() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool MessageStore::registerUser(const std::string& username,
                                const std::string& passwordSalt,
                                const std::string& passwordHash,
                                std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    bool exists = false;
    if (!userExistsLocked(username, exists, error)) {
        return false;
    }

    if (exists) {
        Statement existing(db_,
                           "SELECT password_hash FROM users WHERE username = ?;",
                           error);
        if (!existing) {
            return false;
        }

        sqlite3_bind_text(existing.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);
        const int existingResult = sqlite3_step(existing.get());
        if (existingResult != SQLITE_ROW) {
            error = lastError(db_);
            return false;
        }

        const auto* rawHash = sqlite3_column_text(existing.get(), 0);
        const std::string existingHash = rawHash ? reinterpret_cast<const char*>(rawHash) : "";
        if (!existingHash.empty()) {
            error = "username already exists";
            return false;
        }

        Statement update(db_,
                         "UPDATE users SET password_salt = ?, password_hash = ? WHERE username = ?;",
                         error);
        if (!update) {
            return false;
        }

        sqlite3_bind_text(update.get(), 1, passwordSalt.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update.get(), 2, passwordHash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update.get(), 3, username.c_str(), -1, SQLITE_TRANSIENT);

        const int updateResult = sqlite3_step(update.get());
        if (updateResult != SQLITE_DONE) {
            error = lastError(db_);
            return false;
        }

        return true;
    }

    Statement statement(db_,
                        "INSERT INTO users(username, password_salt, password_hash) "
                        "VALUES (?, ?, ?);",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, passwordSalt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 3, passwordHash.c_str(), -1, SQLITE_TRANSIENT);

    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }

    return true;
}

bool MessageStore::authenticateUser(const std::string& username,
                                    const std::string& passwordHash,
                                    bool& ok,
                                    std::string& error) {
    ok = false;
    std::lock_guard<std::mutex> lock(mutex_);

    Statement statement(db_,
                        "SELECT password_hash FROM users WHERE username = ?;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);

    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return true;
    }

    if (result != SQLITE_ROW) {
        error = lastError(db_);
        return false;
    }

    const auto* rawHash = sqlite3_column_text(statement.get(), 0);
    const std::string storedHash = rawHash ? reinterpret_cast<const char*>(rawHash) : "";
    ok = !storedHash.empty() && storedHash == passwordHash;
    return true;
}

bool MessageStore::loadPasswordSalt(const std::string& username,
                                    std::string& passwordSalt,
                                    std::string& error) {
    passwordSalt.clear();
    std::lock_guard<std::mutex> lock(mutex_);

    Statement statement(db_,
                        "SELECT password_salt FROM users WHERE username = ?;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);

    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return true;
    }

    if (result != SQLITE_ROW) {
        error = lastError(db_);
        return false;
    }

    const auto* rawSalt = sqlite3_column_text(statement.get(), 0);
    passwordSalt = rawSalt ? reinterpret_cast<const char*>(rawSalt) : "";
    return true;
}

bool MessageStore::userExists(const std::string& username, bool& exists, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return userExistsLocked(username, exists, error);
}

bool MessageStore::saveMessage(const std::string& sender,
                               const std::string& recipient,
                               const std::string& text,
                               long long replyToMessageId,
                               StoredMessage& storedMessage,
                               std::string& error) {
    storedMessage = StoredMessage{};
    std::lock_guard<std::mutex> lock(mutex_);

    bool senderExists = false;
    bool recipientExists = false;
    if (!userExistsLocked(sender, senderExists, error) ||
        !userExistsLocked(recipient, recipientExists, error)) {
        return false;
    }

    if (!senderExists || !recipientExists) {
        error = "unknown user";
        return false;
    }

    long long conversationId = 0;
    if (!getOrCreateConversationLocked(sender, recipient, conversationId, error)) {
        return false;
    }

    std::string replyToSender;
    std::string replyToText;
    if (replyToMessageId > 0) {
        Statement replyStatement(db_,
                                 "SELECT sender, body "
                                 "FROM messages "
                                 "WHERE id = ? AND conversation_id = ?;",
                                 error);
        if (!replyStatement) {
            return false;
        }

        sqlite3_bind_int64(replyStatement.get(), 1, replyToMessageId);
        sqlite3_bind_int64(replyStatement.get(), 2, conversationId);

        const int replyResult = sqlite3_step(replyStatement.get());
        if (replyResult == SQLITE_DONE) {
            error = "reply target not found";
            return false;
        }

        if (replyResult != SQLITE_ROW) {
            error = lastError(db_);
            return false;
        }

        const auto* rawReplySender = sqlite3_column_text(replyStatement.get(), 0);
        const auto* rawReplyText = sqlite3_column_text(replyStatement.get(), 1);
        replyToSender = rawReplySender ? reinterpret_cast<const char*>(rawReplySender) : "";
        replyToText = rawReplyText ? reinterpret_cast<const char*>(rawReplyText) : "";
    }

    Statement statement(db_,
                        "INSERT INTO messages("
                        "  conversation_id,"
                        "  sender,"
                        "  recipient,"
                        "  body,"
                        "  reply_to_message_id"
                        ") VALUES (?, ?, ?, ?, NULLIF(?, 0));",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_int64(statement.get(), 1, conversationId);
    sqlite3_bind_text(statement.get(), 2, sender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 3, recipient.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 4, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.get(), 5, replyToMessageId);

    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }

    storedMessage.id = sqlite3_last_insert_rowid(db_);
    storedMessage.sender = sender;
    storedMessage.recipient = recipient;
    storedMessage.text = text;
    storedMessage.replyToMessageId = replyToMessageId;
    storedMessage.replyToSender = replyToSender;
    storedMessage.replyToText = replyToText;

    Statement createdAtStatement(db_,
                                 "SELECT created_at FROM messages WHERE id = ?;",
                                 error);
    if (!createdAtStatement) {
        return false;
    }

    sqlite3_bind_int64(createdAtStatement.get(), 1, storedMessage.id);
    const int createdAtResult = sqlite3_step(createdAtStatement.get());
    if (createdAtResult != SQLITE_ROW) {
        error = createdAtResult == SQLITE_DONE ? "saved message not found" : lastError(db_);
        return false;
    }

    const auto* rawCreatedAt = sqlite3_column_text(createdAtStatement.get(), 0);
    if (rawCreatedAt) {
        storedMessage.createdAt = reinterpret_cast<const char*>(rawCreatedAt);
    }

    return true;
}

bool MessageStore::fetchChats(const std::string& user,
                              std::vector<ChatSummary>& chats,
                              std::string& error) {
    chats.clear();
    std::lock_guard<std::mutex> lock(mutex_);

    Statement statement(db_,
                        "SELECT "
                        "  peer.id,"
                        "  peer.username,"
                        "  m.created_at,"
                        "  m.sender,"
                        "  m.body,"
                        "  COALESCE(("
                        "    SELECT COUNT(1) "
                        "    FROM messages unread "
                        "    WHERE unread.conversation_id = c.id "
                        "      AND unread.sender != ? "
                        "      AND unread.id > COALESCE(cr.last_read_message_id, 0)"
                        "  ), 0) "
                        "FROM conversations c "
                        "JOIN users peer ON peer.username = "
                        "  CASE WHEN c.user_low = ? THEN c.user_high ELSE c.user_low END "
                        "LEFT JOIN messages m ON m.id = ("
                        "  SELECT id FROM messages "
                        "  WHERE conversation_id = c.id "
                        "  ORDER BY id DESC "
                        "  LIMIT 1"
                        ") "
                        "LEFT JOIN conversation_reads cr "
                        "  ON cr.conversation_id = c.id AND cr.username = ? "
                        "WHERE c.user_low = ? OR c.user_high = ? "
                        "ORDER BY CASE WHEN m.id IS NULL THEN c.id ELSE m.id END DESC;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 3, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 4, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 5, user.c_str(), -1, SQLITE_TRANSIENT);

    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            return true;
        }

        if (result != SQLITE_ROW) {
            error = lastError(db_);
            return false;
        }

        auto columnText = [&](int column) -> std::string {
            const auto* value = sqlite3_column_text(statement.get(), column);
            return value ? reinterpret_cast<const char*>(value) : "";
        };

        chats.push_back({
            columnText(0),
            columnText(1),
            columnText(2),
            columnText(3),
            columnText(4),
            sqlite3_column_int(statement.get(), 5)
        });
    }
}

bool MessageStore::searchUsers(const std::string& query,
                               const std::string& excludeUser,
                               int limit,
                               std::vector<UserSearchResult>& users,
                               std::string& error) {
    users.clear();
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string escapedQuery = escapeLikePattern(query);
    const std::string containsPattern = "%" + escapedQuery + "%";
    const std::string prefixPattern = escapedQuery + "%";

    Statement statement(db_,
                        "SELECT candidate.id, candidate.username "
                        "FROM users candidate "
                        "JOIN users current_user ON current_user.username = ? "
                        "WHERE candidate.id != current_user.id "
                        "  AND candidate.username LIKE ? ESCAPE '\\' "
                        "  AND NOT EXISTS ("
                        "    SELECT 1 "
                        "    FROM conversations c "
                        "    JOIN users low_user ON low_user.username = c.user_low "
                        "    JOIN users high_user ON high_user.username = c.user_high "
                        "    WHERE (low_user.id = current_user.id AND high_user.id = candidate.id) "
                        "       OR (low_user.id = candidate.id AND high_user.id = current_user.id)"
                        "  ) "
                        "ORDER BY "
                        "  CASE "
                        "    WHEN candidate.username = ? THEN 0 "
                        "    WHEN candidate.username LIKE ? ESCAPE '\\' THEN 1 "
                        "    ELSE 2 "
                        "  END,"
                        "  candidate.username ASC "
                        "LIMIT ?;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, excludeUser.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, containsPattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 3, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 4, prefixPattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement.get(), 5, limit);

    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            return true;
        }

        if (result != SQLITE_ROW) {
            error = lastError(db_);
            return false;
        }

        const auto* rawUsername = sqlite3_column_text(statement.get(), 1);
        users.push_back({
            std::to_string(sqlite3_column_int64(statement.get(), 0)),
            rawUsername ? reinterpret_cast<const char*>(rawUsername) : ""
        });
    }
}

bool MessageStore::createConversation(const std::string& user,
                                      long long peerId,
                                      std::string& peerUsername,
                                      std::string& error) {
    peerUsername.clear();
    std::lock_guard<std::mutex> lock(mutex_);

    bool userExists = false;
    if (!userExistsLocked(user, userExists, error)) {
        return false;
    }

    if (!userExists) {
        error = "unknown user";
        return false;
    }

    Statement peerStatement(db_,
                            "SELECT username FROM users WHERE id = ?;",
                            error);
    if (!peerStatement) {
        return false;
    }

    sqlite3_bind_int64(peerStatement.get(), 1, peerId);
    const int peerResult = sqlite3_step(peerStatement.get());
    if (peerResult == SQLITE_DONE) {
        error = "unknown user";
        return false;
    }

    if (peerResult != SQLITE_ROW) {
        error = lastError(db_);
        return false;
    }

    const auto* rawPeer = sqlite3_column_text(peerStatement.get(), 0);
    peerUsername = rawPeer ? reinterpret_cast<const char*>(rawPeer) : "";
    if (peerUsername.empty()) {
        error = "unknown user";
        return false;
    }

    if (peerUsername == user) {
        error = "cannot create chat with yourself";
        return false;
    }

    long long conversationId = 0;
    return getOrCreateConversationLocked(user, peerUsername, conversationId, error);
}

bool MessageStore::fetchHistory(const std::string& user,
                                const std::string& peer,
                                int limit,
                                std::vector<StoredMessage>& messages,
                                std::string& error) {
    messages.clear();

    std::lock_guard<std::mutex> lock(mutex_);

    long long conversationId = 0;
    if (!findConversationLocked(user, peer, conversationId, error)) {
        return false;
    }

    if (conversationId == 0) {
        return true;
    }

    Statement statement(db_,
                        "SELECT "
                        "  m.id,"
                        "  m.created_at,"
                        "  m.sender,"
                        "  m.recipient,"
                        "  m.body,"
                        "  COALESCE(m.reply_to_message_id, 0),"
                        "  COALESCE(reply.sender, ''),"
                        "  COALESCE(reply.body, '') "
                        "FROM ("
                        "  SELECT id, created_at, sender, recipient, body, reply_to_message_id "
                        "  FROM messages "
                        "  WHERE conversation_id = ? "
                        "  ORDER BY id DESC "
                        "  LIMIT ?"
                        ") m "
                        "LEFT JOIN messages reply ON reply.id = m.reply_to_message_id "
                        "ORDER BY m.id ASC;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_int64(statement.get(), 1, conversationId);
    sqlite3_bind_int(statement.get(), 2, limit);

    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            return markConversationReadLocked(conversationId, user, error);
        }

        if (result != SQLITE_ROW) {
            error = lastError(db_);
            return false;
        }

        auto columnText = [&](int column) -> std::string {
            const auto* value = sqlite3_column_text(statement.get(), column);
            return value ? reinterpret_cast<const char*>(value) : "";
        };

        messages.push_back({
            sqlite3_column_int64(statement.get(), 0),
            columnText(1),
            columnText(2),
            columnText(3),
            columnText(4),
            sqlite3_column_int64(statement.get(), 5),
            columnText(6),
            columnText(7)
        });
    }
}

bool MessageStore::markConversationRead(const std::string& user,
                                        const std::string& peer,
                                        std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    long long conversationId = 0;
    if (!findConversationLocked(user, peer, conversationId, error)) {
        return false;
    }

    if (conversationId == 0) {
        return true;
    }

    return markConversationReadLocked(conversationId, user, error);
}

bool MessageStore::markConversationReadLocked(long long conversationId,
                                              const std::string& username,
                                              std::string& error) {
    Statement maxMessageStatement(db_,
                                  "SELECT COALESCE(MAX(id), 0) "
                                  "FROM messages "
                                  "WHERE conversation_id = ?;",
                                  error);
    if (!maxMessageStatement) {
        return false;
    }

    sqlite3_bind_int64(maxMessageStatement.get(), 1, conversationId);
    const int maxResult = sqlite3_step(maxMessageStatement.get());
    if (maxResult != SQLITE_ROW) {
        if (maxResult == SQLITE_DONE) {
            return true;
        }
        error = lastError(db_);
        return false;
    }

    const long long maxMessageId = sqlite3_column_int64(maxMessageStatement.get(), 0);

    Statement upsertStatement(db_,
                              "INSERT INTO conversation_reads("
                              "  conversation_id,"
                              "  username,"
                              "  last_read_message_id,"
                              "  updated_at"
                              ") VALUES (?, ?, ?, CURRENT_TIMESTAMP) "
                              "ON CONFLICT(conversation_id, username) "
                              "DO UPDATE SET "
                              "  last_read_message_id = excluded.last_read_message_id,"
                              "  updated_at = CURRENT_TIMESTAMP;",
                              error);
    if (!upsertStatement) {
        return false;
    }

    sqlite3_bind_int64(upsertStatement.get(), 1, conversationId);
    sqlite3_bind_text(upsertStatement.get(), 2, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upsertStatement.get(), 3, maxMessageId);

    const int upsertResult = sqlite3_step(upsertStatement.get());
    if (upsertResult != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }

    return true;
}

bool MessageStore::deleteConversation(const std::string& user,
                                      const std::string& peer,
                                      std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    bool userKnown = false;
    bool peerKnown = false;
    if (!userExistsLocked(user, userKnown, error) ||
        !userExistsLocked(peer, peerKnown, error)) {
        return false;
    }

    if (!userKnown || !peerKnown) {
        error = "unknown user";
        return false;
    }

    long long conversationId = 0;
    if (!findConversationLocked(user, peer, conversationId, error)) {
        return false;
    }

    if (conversationId == 0) {
        error = "chat not found";
        return false;
    }

    if (!executeLocked("BEGIN IMMEDIATE TRANSACTION;", error)) {
        return false;
    }

    {
        Statement deleteMessages(db_,
                                 "DELETE FROM messages WHERE conversation_id = ?;",
                                 error);
        if (!deleteMessages) {
            executeLocked("ROLLBACK;", error);
            return false;
        }

        sqlite3_bind_int64(deleteMessages.get(), 1, conversationId);
        if (sqlite3_step(deleteMessages.get()) != SQLITE_DONE) {
            error = lastError(db_);
            executeLocked("ROLLBACK;", error);
            return false;
        }
    }

    {
        Statement deleteConversation(db_,
                                     "DELETE FROM conversations WHERE id = ?;",
                                     error);
        if (!deleteConversation) {
            executeLocked("ROLLBACK;", error);
            return false;
        }

        sqlite3_bind_int64(deleteConversation.get(), 1, conversationId);
        if (sqlite3_step(deleteConversation.get()) != SQLITE_DONE) {
            error = lastError(db_);
            executeLocked("ROLLBACK;", error);
            return false;
        }
    }

    return executeLocked("COMMIT;", error);
}

bool MessageStore::executeLocked(const char* sql, std::string& error) {
    char* rawError = nullptr;
    const int result = sqlite3_exec(db_, sql, nullptr, nullptr, &rawError);
    if (result != SQLITE_OK) {
        error = rawError ? rawError : lastError(db_);
        sqlite3_free(rawError);
        return false;
    }

    return true;
}

bool MessageStore::ensureSchemaLocked(std::string& error) {
    if (!executeLocked(
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT NOT NULL UNIQUE,"
        "  password_salt TEXT,"
        "  password_hash TEXT,"
        "  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS conversations ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  user_low TEXT NOT NULL,"
        "  user_high TEXT NOT NULL,"
        "  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE(user_low, user_high)"
        ");"
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  conversation_id INTEGER NOT NULL,"
        "  sender TEXT NOT NULL,"
        "  recipient TEXT NOT NULL,"
        "  body TEXT NOT NULL,"
        "  reply_to_message_id INTEGER,"
        "  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY(conversation_id) REFERENCES conversations(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS conversation_reads ("
        "  conversation_id INTEGER NOT NULL,"
        "  username TEXT NOT NULL,"
        "  last_read_message_id INTEGER NOT NULL DEFAULT 0,"
        "  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE(conversation_id, username),"
        "  FOREIGN KEY(conversation_id) REFERENCES conversations(id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_messages_conversation_id "
        "ON messages(conversation_id, id);"
        "CREATE INDEX IF NOT EXISTS idx_conversation_reads_user "
        "ON conversation_reads(username, conversation_id);",
        error)) {
        return false;
    }

    return ensureColumnLocked("users", "password_salt", "TEXT", error) &&
           ensureColumnLocked("users", "password_hash", "TEXT", error) &&
           ensureColumnLocked("messages", "reply_to_message_id", "INTEGER", error);
}

bool MessageStore::ensureColumnLocked(const std::string& table,
                                      const std::string& column,
                                      const std::string& definition,
                                      std::string& error) {
    Statement statement(db_, ("PRAGMA table_info(" + table + ");").c_str(), error);
    if (!statement) {
        return false;
    }

    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            break;
        }

        if (result != SQLITE_ROW) {
            error = lastError(db_);
            return false;
        }

        const auto* rawName = sqlite3_column_text(statement.get(), 1);
        const std::string name = rawName ? reinterpret_cast<const char*>(rawName) : "";
        if (name == column) {
            return true;
        }
    }

    return executeLocked(("ALTER TABLE " + table + " ADD COLUMN " + column + " " + definition + ";").c_str(),
                         error);
}

bool MessageStore::userExistsLocked(const std::string& username, bool& exists, std::string& error) {
    exists = false;
    Statement statement(db_,
                        "SELECT 1 FROM users WHERE username = ? LIMIT 1;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);

    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) {
        exists = true;
        return true;
    }

    if (result == SQLITE_DONE) {
        return true;
    }

    error = lastError(db_);
    return false;
}

bool MessageStore::findConversationLocked(const std::string& user,
                                          const std::string& peer,
                                          long long& conversationId,
                                          std::string& error) {
    conversationId = 0;
    const auto [userLow, userHigh] = orderedUsers(user, peer);

    Statement statement(db_,
                        "SELECT id FROM conversations WHERE user_low = ? AND user_high = ?;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, userLow.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, userHigh.c_str(), -1, SQLITE_TRANSIENT);

    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return true;
    }

    if (result != SQLITE_ROW) {
        error = lastError(db_);
        return false;
    }

    conversationId = sqlite3_column_int64(statement.get(), 0);
    return true;
}

bool MessageStore::getOrCreateConversationLocked(const std::string& user,
                                                 const std::string& peer,
                                                 long long& conversationId,
                                                 std::string& error) {
    const auto [userLow, userHigh] = orderedUsers(user, peer);

    Statement insert(db_,
                     "INSERT OR IGNORE INTO conversations(user_low, user_high) VALUES (?, ?);",
                     error);
    if (!insert) {
        return false;
    }

    sqlite3_bind_text(insert.get(), 1, userLow.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.get(), 2, userHigh.c_str(), -1, SQLITE_TRANSIENT);

    const int insertResult = sqlite3_step(insert.get());
    if (insertResult != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }

    return findConversationLocked(user, peer, conversationId, error);
}
