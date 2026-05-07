#include "MessageStore.hpp"

#include <sqlite3.h>

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

bool MessageStore::ensureUser(const std::string& username, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ensureUserLocked(username, error);
}

bool MessageStore::saveMessage(const std::string& sender,
                               const std::string& recipient,
                               const std::string& text,
                               std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!ensureUserLocked(sender, error) || !ensureUserLocked(recipient, error)) {
        return false;
    }

    long long conversationId = 0;
    if (!getOrCreateConversationLocked(sender, recipient, conversationId, error)) {
        return false;
    }

    Statement statement(db_,
                        "INSERT INTO messages(conversation_id, sender, recipient, body) "
                        "VALUES (?, ?, ?, ?);",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_int64(statement.get(), 1, conversationId);
    sqlite3_bind_text(statement.get(), 2, sender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 3, recipient.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 4, text.c_str(), -1, SQLITE_TRANSIENT);

    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }

    return true;
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
                        "SELECT created_at, sender, recipient, body "
                        "FROM ("
                        "  SELECT id, created_at, sender, recipient, body "
                        "  FROM messages "
                        "  WHERE conversation_id = ? "
                        "  ORDER BY id DESC "
                        "  LIMIT ?"
                        ") "
                        "ORDER BY id ASC;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_int64(statement.get(), 1, conversationId);
    sqlite3_bind_int(statement.get(), 2, limit);

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

        messages.push_back({
            columnText(0),
            columnText(1),
            columnText(2),
            columnText(3)
        });
    }
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
    return executeLocked(
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT NOT NULL UNIQUE,"
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
        "  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY(conversation_id) REFERENCES conversations(id)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_messages_conversation_id "
        "ON messages(conversation_id, id);",
        error);
}

bool MessageStore::ensureUserLocked(const std::string& username, std::string& error) {
    Statement statement(db_,
                        "INSERT OR IGNORE INTO users(username) VALUES (?);",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);

    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }

    return true;
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
