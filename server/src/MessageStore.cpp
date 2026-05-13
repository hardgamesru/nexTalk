#include "MessageStore.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sqlite3.h>
#include <utility>

namespace {
    constexpr const char* kDeletedMessageText = "Сообщение удалено";

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

    std::string groupChatId(long long groupId) {
        return "group:" + std::to_string(groupId);
    }

    bool parseGroupChatId(const std::string& chatId, long long& groupId) {
        constexpr const char* prefix = "group:";
        if (chatId.rfind(prefix, 0) != 0) {
            return false;
        }

        const std::string rawId = chatId.substr(6);
        if (rawId.empty()) {
            return false;
        }

        long long result = 0;
        for (char ch : rawId) {
            if (ch < '0' || ch > '9') {
                return false;
            }
            result = result * 10 + (ch - '0');
        }

        if (result <= 0) {
            return false;
        }

        groupId = result;
        return true;
    }

    std::string defaultAvatarColorForUsername(const std::string& username) {
        static constexpr std::array<const char*, 10> kPalette = {
            "#5C7CFA", "#E56B6F", "#3FA37A", "#F4A261", "#7B6CF6",
            "#4D908E", "#577590", "#BC6C25", "#C8553D", "#8A5CF6"
        };

        unsigned long long hash = 1469598103934665603ULL;
        for (unsigned char ch : username) {
            hash ^= ch;
            hash *= 1099511628211ULL;
        }
        return kPalette[hash % kPalette.size()];
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
                         "UPDATE users "
                         "SET password_salt = ?, "
                         "    password_hash = ?, "
                         "    display_name = COALESCE(NULLIF(display_name, ''), username), "
                         "    bio = COALESCE(bio, ''), "
                         "    avatar_color = COALESCE(NULLIF(avatar_color, ''), ?), "
                         "    last_seen = COALESCE(last_seen, CURRENT_TIMESTAMP) "
                         "WHERE username = ?;",
                         error);
        if (!update) {
            return false;
        }

        sqlite3_bind_text(update.get(), 1, passwordSalt.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update.get(), 2, passwordHash.c_str(), -1, SQLITE_TRANSIENT);
        const std::string defaultColor = defaultAvatarColorForUsername(username);
        sqlite3_bind_text(update.get(), 3, defaultColor.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update.get(), 4, username.c_str(), -1, SQLITE_TRANSIENT);

        const int updateResult = sqlite3_step(update.get());
        if (updateResult != SQLITE_DONE) {
            error = lastError(db_);
            return false;
        }

        return true;
    }

    Statement statement(db_,
                        "INSERT INTO users("
                        "  username, password_salt, password_hash, display_name, bio, avatar_color, created_at, last_seen"
                        ") VALUES (?, ?, ?, ?, '', ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP);",
                        error);
    if (!statement) {
        return false;
    }

    const std::string defaultColor = defaultAvatarColorForUsername(username);
    sqlite3_bind_text(statement.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, passwordSalt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 3, passwordHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 4, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 5, defaultColor.c_str(), -1, SQLITE_TRANSIENT);

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

bool MessageStore::getUserProfile(const std::string& username,
                                  UserProfile& profile,
                                  std::string& error) {
    profile = UserProfile{};
    std::lock_guard<std::mutex> lock(mutex_);

    Statement statement(db_,
                        "SELECT username, "
                        "       COALESCE(NULLIF(display_name, ''), username), "
                        "       COALESCE(bio, ''), "
                        "       COALESCE(NULLIF(avatar_color, ''), ''), "
                        "       COALESCE(created_at, ''), "
                        "       COALESCE(last_seen, '') "
                        "FROM users WHERE username = ?;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        error = "unknown user";
        return false;
    }
    if (result != SQLITE_ROW) {
        error = lastError(db_);
        return false;
    }

    const auto* rawUsername = sqlite3_column_text(statement.get(), 0);
    const auto* rawDisplayName = sqlite3_column_text(statement.get(), 1);
    const auto* rawBio = sqlite3_column_text(statement.get(), 2);
    const auto* rawAvatarColor = sqlite3_column_text(statement.get(), 3);
    const auto* rawCreatedAt = sqlite3_column_text(statement.get(), 4);
    const auto* rawLastSeen = sqlite3_column_text(statement.get(), 5);

    profile.username = rawUsername ? reinterpret_cast<const char*>(rawUsername) : "";
    profile.displayName = rawDisplayName ? reinterpret_cast<const char*>(rawDisplayName) : profile.username;
    profile.bio = rawBio ? reinterpret_cast<const char*>(rawBio) : "";
    profile.avatarColor = rawAvatarColor ? reinterpret_cast<const char*>(rawAvatarColor) : "";
    if (profile.avatarColor.empty()) {
        profile.avatarColor = defaultAvatarColorForUsername(profile.username);
    }
    profile.createdAt = rawCreatedAt ? reinterpret_cast<const char*>(rawCreatedAt) : "";
    profile.lastSeen = rawLastSeen ? reinterpret_cast<const char*>(rawLastSeen) : "";
    return true;
}

bool MessageStore::getUserProfiles(const std::vector<std::string>& usernames,
                                   std::vector<UserProfile>& profiles,
                                   std::string& error) {
    profiles.clear();
    if (usernames.empty()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::string sql =
        "SELECT username, "
        "       COALESCE(NULLIF(display_name, ''), username), "
        "       COALESCE(bio, ''), "
        "       COALESCE(NULLIF(avatar_color, ''), ''), "
        "       COALESCE(created_at, ''), "
        "       COALESCE(last_seen, '') "
        "FROM users WHERE username IN (";

    for (std::size_t i = 0; i < usernames.size(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        sql += '?';
    }
    sql += ") ORDER BY username ASC;";

    Statement statement(db_, sql.c_str(), error);
    if (!statement) {
        return false;
    }

    for (std::size_t i = 0; i < usernames.size(); ++i) {
        sqlite3_bind_text(statement.get(), static_cast<int>(i + 1), usernames[i].c_str(), -1, SQLITE_TRANSIENT);
    }

    while (true) {
        const int result = sqlite3_step(statement.get());
        if (result == SQLITE_DONE) {
            return true;
        }
        if (result != SQLITE_ROW) {
            error = lastError(db_);
            return false;
        }

        UserProfile profile;
        const auto* rawUsername = sqlite3_column_text(statement.get(), 0);
        const auto* rawDisplayName = sqlite3_column_text(statement.get(), 1);
        const auto* rawBio = sqlite3_column_text(statement.get(), 2);
        const auto* rawAvatarColor = sqlite3_column_text(statement.get(), 3);
        const auto* rawCreatedAt = sqlite3_column_text(statement.get(), 4);
        const auto* rawLastSeen = sqlite3_column_text(statement.get(), 5);

        profile.username = rawUsername ? reinterpret_cast<const char*>(rawUsername) : "";
        profile.displayName = rawDisplayName ? reinterpret_cast<const char*>(rawDisplayName) : profile.username;
        profile.bio = rawBio ? reinterpret_cast<const char*>(rawBio) : "";
        profile.avatarColor = rawAvatarColor ? reinterpret_cast<const char*>(rawAvatarColor) : "";
        if (profile.avatarColor.empty()) {
            profile.avatarColor = defaultAvatarColorForUsername(profile.username);
        }
        profile.createdAt = rawCreatedAt ? reinterpret_cast<const char*>(rawCreatedAt) : "";
        profile.lastSeen = rawLastSeen ? reinterpret_cast<const char*>(rawLastSeen) : "";
        profiles.push_back(std::move(profile));
    }
}

bool MessageStore::updateUserProfile(const std::string& username,
                                     const std::string& displayName,
                                     const std::string& bio,
                                     const std::string& avatarColor,
                                     std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    Statement statement(db_,
                        "UPDATE users "
                        "SET display_name = ?, bio = ?, avatar_color = ?, last_seen = CURRENT_TIMESTAMP "
                        "WHERE username = ?;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, displayName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, bio.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 3, avatarColor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 4, username.c_str(), -1, SQLITE_TRANSIENT);

    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }

    if (sqlite3_changes(db_) == 0) {
        error = "unknown user";
        return false;
    }

    return true;
}

bool MessageStore::updateUserLastSeen(const std::string& username, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    Statement statement(db_,
                        "UPDATE users SET last_seen = CURRENT_TIMESTAMP WHERE username = ?;",
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

bool MessageStore::saveMessage(const std::string& sender,
                               const std::string& recipient,
                               const std::string& text,
                               long long replyToMessageId,
                               long long forwardFromMessageId,
                               const std::string& forwardFromSenderOverride,
                               const std::string& forwardFromTextOverride,
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
                                 "SELECT sender, CASE WHEN deleted_at IS NOT NULL THEN ? ELSE body END "
                                 "FROM messages "
                                 "WHERE id = ? AND conversation_id = ?;",
                                 error);
        if (!replyStatement) {
            return false;
        }

        sqlite3_bind_text(replyStatement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(replyStatement.get(), 2, replyToMessageId);
        sqlite3_bind_int64(replyStatement.get(), 3, conversationId);

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

    std::string forwardFromSender;
    std::string forwardFromText;
    if (forwardFromMessageId > 0) {
        if (!forwardFromSenderOverride.empty() || !forwardFromTextOverride.empty()) {
            forwardFromSender = forwardFromSenderOverride;
            forwardFromText = forwardFromTextOverride;
        } else {
            Statement forwardStatement(db_,
                                       "SELECT m.sender, CASE WHEN m.deleted_at IS NOT NULL THEN ? ELSE m.body END "
                                       "FROM messages m "
                                       "JOIN conversations c ON c.id = m.conversation_id "
                                       "WHERE m.id = ? AND (c.user_low = ? OR c.user_high = ?);",
                                       error);
            if (!forwardStatement) {
                return false;
            }

            sqlite3_bind_text(forwardStatement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(forwardStatement.get(), 2, forwardFromMessageId);
            sqlite3_bind_text(forwardStatement.get(), 3, sender.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(forwardStatement.get(), 4, sender.c_str(), -1, SQLITE_TRANSIENT);

            const int forwardResult = sqlite3_step(forwardStatement.get());
            if (forwardResult != SQLITE_ROW && forwardResult != SQLITE_DONE) {
                error = lastError(db_);
                return false;
            }
            if (forwardResult == SQLITE_ROW) {
                const auto* rawForwardSender = sqlite3_column_text(forwardStatement.get(), 0);
                const auto* rawForwardText = sqlite3_column_text(forwardStatement.get(), 1);
                forwardFromSender = rawForwardSender ? reinterpret_cast<const char*>(rawForwardSender) : "";
                forwardFromText = rawForwardText ? reinterpret_cast<const char*>(rawForwardText) : "";
            } else {
                Statement groupForward(db_,
                                       "SELECT m.sender, CASE WHEN m.deleted_at IS NOT NULL THEN ? ELSE m.body END "
                                       "FROM group_messages m "
                                       "JOIN group_members gm ON gm.group_id = m.group_id AND gm.username = ? "
                                       "WHERE m.id = ?;",
                                       error);
                if (!groupForward) {
                    return false;
                }
                sqlite3_bind_text(groupForward.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(groupForward.get(), 2, sender.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(groupForward.get(), 3, forwardFromMessageId);
                const int groupForwardResult = sqlite3_step(groupForward.get());
                if (groupForwardResult != SQLITE_ROW) {
                    error = groupForwardResult == SQLITE_DONE ? "forward target not found" : lastError(db_);
                    return false;
                }
                const auto* rawForwardSender = sqlite3_column_text(groupForward.get(), 0);
                const auto* rawForwardText = sqlite3_column_text(groupForward.get(), 1);
                forwardFromSender = rawForwardSender ? reinterpret_cast<const char*>(rawForwardSender) : "";
                forwardFromText = rawForwardText ? reinterpret_cast<const char*>(rawForwardText) : "";
            }
        }
    }

    Statement statement(db_,
                        "INSERT INTO messages("
                        "  conversation_id,"
                        "  sender,"
                        "  recipient,"
                        "  body,"
                        "  reply_to_message_id,"
                        "  forward_from_message_id,"
                        "  forward_from_sender,"
                        "  forward_from_text"
                        ") VALUES (?, ?, ?, ?, NULLIF(?, 0), NULLIF(?, 0), NULLIF(?, ''), NULLIF(?, ''));",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_int64(statement.get(), 1, conversationId);
    sqlite3_bind_text(statement.get(), 2, sender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 3, recipient.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 4, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.get(), 5, replyToMessageId);
    sqlite3_bind_int64(statement.get(), 6, forwardFromMessageId);
    sqlite3_bind_text(statement.get(), 7, forwardFromSender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 8, forwardFromText.c_str(), -1, SQLITE_TRANSIENT);

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
    storedMessage.forwardFromMessageId = forwardFromMessageId;
    storedMessage.forwardFromSender = forwardFromSender;
    storedMessage.forwardFromText = forwardFromText;

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

bool MessageStore::loadAccessibleMessage(const std::string& username,
                                         long long messageId,
                                         StoredMessage& message,
                                         std::string& error) {
    message = StoredMessage{};
    std::lock_guard<std::mutex> lock(mutex_);

    Statement statement(db_,
                        "SELECT m.id, m.created_at, m.sender, m.recipient, m.body, "
                        "       COALESCE(m.reply_to_message_id, 0), "
                        "       COALESCE(reply.sender, ''), "
                        "       COALESCE(CASE WHEN reply.deleted_at IS NOT NULL THEN ? ELSE reply.body END, ''), "
                        "       COALESCE(m.forward_from_message_id, 0), "
                        "       COALESCE(m.forward_from_sender, COALESCE(fwd.sender, '')), "
                        "       COALESCE(m.forward_from_text, COALESCE(CASE WHEN fwd.deleted_at IS NOT NULL THEN ? ELSE fwd.body END, '')), "
                        "       COALESCE(m.deleted_at, ''), "
                        "       COALESCE(m.deleted_by, '') "
                        "FROM messages m "
                        "JOIN conversations c ON c.id = m.conversation_id "
                        "LEFT JOIN messages reply ON reply.id = m.reply_to_message_id "
                        "LEFT JOIN messages fwd ON fwd.id = m.forward_from_message_id "
                        "WHERE m.id = ? AND (c.user_low = ? OR c.user_high = ?);",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.get(), 3, messageId);
    sqlite3_bind_text(statement.get(), 4, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 5, username.c_str(), -1, SQLITE_TRANSIENT);

    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) {
        auto columnText = [&](int column) -> std::string {
            const auto* value = sqlite3_column_text(statement.get(), column);
            return value ? reinterpret_cast<const char*>(value) : "";
        };

        message.id = sqlite3_column_int64(statement.get(), 0);
        message.createdAt = columnText(1);
        message.sender = columnText(2);
        message.recipient = columnText(3);
        message.text = columnText(4);
        message.replyToMessageId = sqlite3_column_int64(statement.get(), 5);
        message.replyToSender = columnText(6);
        message.replyToText = columnText(7);
        message.forwardFromMessageId = sqlite3_column_int64(statement.get(), 8);
        message.forwardFromSender = columnText(9);
        message.forwardFromText = columnText(10);
        message.deletedAt = columnText(11);
        message.deletedBy = columnText(12);
        return true;
    }

    if (result != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }

    Statement groupStatement(db_,
                             "SELECT m.id, m.created_at, m.sender, g.name, m.body, "
                             "       COALESCE(m.reply_to_message_id, 0), "
                             "       COALESCE(reply.sender, ''), "
                             "       COALESCE(CASE WHEN reply.deleted_at IS NOT NULL THEN ? ELSE reply.body END, ''), "
                             "       COALESCE(m.forward_from_message_id, 0), "
                             "       COALESCE(m.forward_from_sender, COALESCE(fwd.sender, '')), "
                             "       COALESCE(m.forward_from_text, COALESCE(CASE WHEN fwd.deleted_at IS NOT NULL THEN ? ELSE fwd.body END, '')), "
                             "       COALESCE(m.deleted_at, ''), "
                             "       COALESCE(m.deleted_by, '') "
                             "FROM group_messages m "
                             "JOIN groups_chat g ON g.id = m.group_id "
                             "JOIN group_members gm ON gm.group_id = g.id AND gm.username = ? "
                             "LEFT JOIN group_messages reply ON reply.id = m.reply_to_message_id "
                             "LEFT JOIN group_messages fwd ON fwd.id = m.forward_from_message_id "
                             "WHERE m.id = ?;",
                             error);
    if (!groupStatement) {
        return false;
    }
    sqlite3_bind_text(groupStatement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(groupStatement.get(), 2, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(groupStatement.get(), 3, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(groupStatement.get(), 4, messageId);
    const int groupResult = sqlite3_step(groupStatement.get());
    if (groupResult == SQLITE_DONE) {
        message = StoredMessage{};
        return true;
    }
    if (groupResult != SQLITE_ROW) {
        error = lastError(db_);
        return false;
    }

    auto groupColumnText = [&](int column) -> std::string {
        const auto* value = sqlite3_column_text(groupStatement.get(), column);
        return value ? reinterpret_cast<const char*>(value) : "";
    };

    message.id = sqlite3_column_int64(groupStatement.get(), 0);
    message.createdAt = groupColumnText(1);
    message.sender = groupColumnText(2);
    message.recipient = groupColumnText(3);
    message.text = groupColumnText(4);
    message.replyToMessageId = sqlite3_column_int64(groupStatement.get(), 5);
    message.replyToSender = groupColumnText(6);
    message.replyToText = groupColumnText(7);
    message.forwardFromMessageId = sqlite3_column_int64(groupStatement.get(), 8);
    message.forwardFromSender = groupColumnText(9);
    message.forwardFromText = groupColumnText(10);
    message.deletedAt = groupColumnText(11);
    message.deletedBy = groupColumnText(12);
    return true;
}

bool MessageStore::loadAccessibleMessageInChat(const std::string& username,
                                               const std::string& chatId,
                                               long long messageId,
                                               StoredMessage& message,
                                               std::string& error) {
    message = StoredMessage{};
    std::lock_guard<std::mutex> lock(mutex_);

    long long groupId = 0;
    if (parseGroupChatId(chatId, groupId)) {
        bool isMember = false;
        if (!isGroupMemberLocked(groupId, username, isMember, error)) {
            return false;
        }
        if (!isMember) {
            error = "group not found";
            return false;
        }

        Statement groupStatement(db_,
                                 "SELECT m.id, m.created_at, m.sender, g.name, m.body, "
                                 "       COALESCE(m.reply_to_message_id, 0), "
                                 "       COALESCE(reply.sender, ''), "
                                 "       COALESCE(CASE WHEN reply.deleted_at IS NOT NULL THEN ? ELSE reply.body END, ''), "
                                 "       COALESCE(m.forward_from_message_id, 0), "
                                 "       COALESCE(m.forward_from_sender, COALESCE(fwd.sender, '')), "
                                 "       COALESCE(m.forward_from_text, COALESCE(CASE WHEN fwd.deleted_at IS NOT NULL THEN ? ELSE fwd.body END, '')), "
                                 "       COALESCE(m.deleted_at, ''), "
                                 "       COALESCE(m.deleted_by, '') "
                                 "FROM group_messages m "
                                 "JOIN groups_chat g ON g.id = m.group_id "
                                 "LEFT JOIN group_messages reply ON reply.id = m.reply_to_message_id "
                                 "LEFT JOIN group_messages fwd ON fwd.id = m.forward_from_message_id "
                                 "WHERE m.id = ? AND m.group_id = ?;",
                                 error);
        if (!groupStatement) {
            return false;
        }

        sqlite3_bind_text(groupStatement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(groupStatement.get(), 2, kDeletedMessageText, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(groupStatement.get(), 3, messageId);
        sqlite3_bind_int64(groupStatement.get(), 4, groupId);

        const int result = sqlite3_step(groupStatement.get());
        if (result == SQLITE_DONE) {
            return true;
        }
        if (result != SQLITE_ROW) {
            error = lastError(db_);
            return false;
        }

        auto columnText = [&](int column) -> std::string {
            const auto* value = sqlite3_column_text(groupStatement.get(), column);
            return value ? reinterpret_cast<const char*>(value) : "";
        };
        message.id = sqlite3_column_int64(groupStatement.get(), 0);
        message.createdAt = columnText(1);
        message.sender = columnText(2);
        message.recipient = columnText(3);
        message.text = columnText(4);
        message.replyToMessageId = sqlite3_column_int64(groupStatement.get(), 5);
        message.replyToSender = columnText(6);
        message.replyToText = columnText(7);
        message.forwardFromMessageId = sqlite3_column_int64(groupStatement.get(), 8);
        message.forwardFromSender = columnText(9);
        message.forwardFromText = columnText(10);
        message.deletedAt = columnText(11);
        message.deletedBy = columnText(12);
        return true;
    }

    long long conversationId = 0;
    if (!findConversationLocked(username, chatId, conversationId, error)) {
        return false;
    }
    if (conversationId == 0) {
        return true;
    }

    Statement statement(db_,
                        "SELECT m.id, m.created_at, m.sender, m.recipient, m.body, "
                        "       COALESCE(m.reply_to_message_id, 0), "
                        "       COALESCE(reply.sender, ''), "
                        "       COALESCE(CASE WHEN reply.deleted_at IS NOT NULL THEN ? ELSE reply.body END, ''), "
                        "       COALESCE(m.forward_from_message_id, 0), "
                        "       COALESCE(m.forward_from_sender, COALESCE(fwd.sender, '')), "
                        "       COALESCE(m.forward_from_text, COALESCE(CASE WHEN fwd.deleted_at IS NOT NULL THEN ? ELSE fwd.body END, '')), "
                        "       COALESCE(m.deleted_at, ''), "
                        "       COALESCE(m.deleted_by, '') "
                        "FROM messages m "
                        "LEFT JOIN messages reply ON reply.id = m.reply_to_message_id "
                        "LEFT JOIN messages fwd ON fwd.id = m.forward_from_message_id "
                        "WHERE m.id = ? AND m.conversation_id = ?;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.get(), 3, messageId);
    sqlite3_bind_int64(statement.get(), 4, conversationId);

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
    message.id = sqlite3_column_int64(statement.get(), 0);
    message.createdAt = columnText(1);
    message.sender = columnText(2);
    message.recipient = columnText(3);
    message.text = columnText(4);
    message.replyToMessageId = sqlite3_column_int64(statement.get(), 5);
    message.replyToSender = columnText(6);
    message.replyToText = columnText(7);
    message.forwardFromMessageId = sqlite3_column_int64(statement.get(), 8);
    message.forwardFromSender = columnText(9);
    message.forwardFromText = columnText(10);
    message.deletedAt = columnText(11);
    message.deletedBy = columnText(12);
    return true;
}

bool MessageStore::fetchChats(const std::string& user,
                              std::vector<ChatSummary>& chats,
                              std::string& error) {
    chats.clear();
    std::lock_guard<std::mutex> lock(mutex_);

    Statement directStatement(db_,
                              "SELECT "
                              "  peer.id,"
                              "  peer.username,"
                              "  m.created_at,"
                              "  m.sender,"
                              "  CASE "
                              "    WHEN m.deleted_at IS NOT NULL THEN ? "
                              "    WHEN m.body != '' THEN m.body "
                              "    WHEN COALESCE(m.forward_from_text, '') != '' THEN m.forward_from_text "
                              "    ELSE m.body "
                              "  END,"
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
                              "WHERE c.user_low = ? OR c.user_high = ?;",
                              error);
    if (!directStatement) {
        return false;
    }

    sqlite3_bind_text(directStatement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(directStatement.get(), 2, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(directStatement.get(), 3, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(directStatement.get(), 4, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(directStatement.get(), 5, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(directStatement.get(), 6, user.c_str(), -1, SQLITE_TRANSIENT);

    while (true) {
        const int result = sqlite3_step(directStatement.get());
        if (result == SQLITE_DONE) {
            break;
        }

        if (result != SQLITE_ROW) {
            error = lastError(db_);
            return false;
        }

        auto columnText = [&](int column) -> std::string {
            const auto* value = sqlite3_column_text(directStatement.get(), column);
            return value ? reinterpret_cast<const char*>(value) : "";
        };

        chats.push_back({
            columnText(1),
            columnText(1),
            columnText(2),
            columnText(3),
            columnText(4),
            sqlite3_column_int(directStatement.get(), 5),
            false,
            false
        });
    }

    Statement groupStatement(db_,
                             "SELECT "
                             "  g.id,"
                             "  g.name,"
                             "  g.admin_username,"
                             "  m.created_at,"
                             "  m.sender,"
                             "  CASE "
                             "    WHEN m.deleted_at IS NOT NULL THEN ? "
                             "    WHEN m.body != '' THEN m.body "
                             "    WHEN COALESCE(m.forward_from_text, '') != '' THEN m.forward_from_text "
                             "    ELSE m.body "
                             "  END,"
                             "  COALESCE(("
                             "    SELECT COUNT(1) "
                             "    FROM group_messages unread "
                             "    WHERE unread.group_id = g.id "
                             "      AND unread.sender != ? "
                             "      AND unread.id > COALESCE(gr.last_read_message_id, 0)"
                             "  ), 0) "
                             "FROM groups_chat g "
                             "JOIN group_members gm ON gm.group_id = g.id AND gm.username = ? "
                             "LEFT JOIN group_messages m ON m.id = ("
                             "  SELECT id FROM group_messages "
                             "  WHERE group_id = g.id "
                             "  ORDER BY id DESC "
                             "  LIMIT 1"
                             ") "
                             "LEFT JOIN group_reads gr ON gr.group_id = g.id AND gr.username = ?;",
                             error);
    if (!groupStatement) {
        return false;
    }

    sqlite3_bind_text(groupStatement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(groupStatement.get(), 2, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(groupStatement.get(), 3, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(groupStatement.get(), 4, user.c_str(), -1, SQLITE_TRANSIENT);

    while (true) {
        const int result = sqlite3_step(groupStatement.get());
        if (result == SQLITE_DONE) {
            break;
        }

        if (result != SQLITE_ROW) {
            error = lastError(db_);
            return false;
        }

        auto columnText = [&](int column) -> std::string {
            const auto* value = sqlite3_column_text(groupStatement.get(), column);
            return value ? reinterpret_cast<const char*>(value) : "";
        };

        chats.push_back({
            groupChatId(sqlite3_column_int64(groupStatement.get(), 0)),
            columnText(1),
            columnText(3),
            columnText(4),
            columnText(5),
            sqlite3_column_int(groupStatement.get(), 6),
            true,
            columnText(2) == user
        });
    }

    std::sort(chats.begin(), chats.end(), [](const ChatSummary& left, const ChatSummary& right) {
        const long long leftKey = left.lastAt.empty() ? 0 : 1;
        const long long rightKey = right.lastAt.empty() ? 0 : 1;
        if (leftKey != rightKey) {
            return leftKey > rightKey;
        }
        if (left.lastAt != right.lastAt) {
            return left.lastAt > right.lastAt;
        }
        return left.peer < right.peer;
    });
    return true;
}

bool MessageStore::searchUsers(const std::string& query,
                               const std::string& excludeUser,
                               const std::string& scope,
                               const std::string& scopeTarget,
                               int limit,
                               std::vector<UserSearchResult>& users,
                               std::string& error) {
    users.clear();
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string escapedQuery = escapeLikePattern(query);
    const std::string containsPattern = "%" + escapedQuery + "%";
    const std::string prefixPattern = escapedQuery + "%";

    std::string sql =
        "SELECT candidate.id, candidate.username "
        "FROM users candidate "
        "JOIN users current_user ON current_user.username = ? "
        "WHERE candidate.id != current_user.id "
        "  AND candidate.username LIKE ? ESCAPE '\\' ";

    if (scope == "dm") {
        sql +=
            "  AND NOT EXISTS ("
            "    SELECT 1 "
            "    FROM conversations c "
            "    JOIN users low_user ON low_user.username = c.user_low "
            "    JOIN users high_user ON high_user.username = c.user_high "
            "    WHERE (low_user.id = current_user.id AND high_user.id = candidate.id) "
            "       OR (low_user.id = candidate.id AND high_user.id = current_user.id)"
            "  ) ";
    } else if (scope == "group_add") {
        sql +=
            "  AND NOT EXISTS ("
            "    SELECT 1 FROM group_members gm "
            "    WHERE gm.group_id = ? AND gm.username = candidate.username"
            "  ) ";
    }

    sql +=
        "ORDER BY "
        "  CASE "
        "    WHEN candidate.username = ? THEN 0 "
        "    WHEN candidate.username LIKE ? ESCAPE '\\' THEN 1 "
        "    ELSE 2 "
        "  END,"
        "  candidate.username ASC "
        "LIMIT ?;";

    Statement statement(db_, sql.c_str(), error);
    if (!statement) {
        return false;
    }

    int bindIndex = 1;
    sqlite3_bind_text(statement.get(), bindIndex++, excludeUser.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), bindIndex++, containsPattern.c_str(), -1, SQLITE_TRANSIENT);
    if (scope == "group_add") {
        long long groupId = 0;
        if (!parseGroupChatId(scopeTarget, groupId)) {
            error = "invalid group";
            return false;
        }
        sqlite3_bind_int64(statement.get(), bindIndex++, groupId);
    }
    sqlite3_bind_text(statement.get(), bindIndex++, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), bindIndex++, prefixPattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement.get(), bindIndex++, limit);

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

    long long groupId = 0;
    if (parseGroupChatId(peer, groupId)) {
        bool isMember = false;
        if (!isGroupMemberLocked(groupId, user, isMember, error)) {
            return false;
        }

        if (!isMember) {
            error = "group not found";
            return false;
        }

        std::string groupName;
        std::string adminUsername;
        std::vector<std::string> memberUsernames;
        if (!loadGroupContextLocked(groupId, groupName, adminUsername, memberUsernames, error)) {
            return false;
        }

        Statement statement(db_,
                            "SELECT "
                            "  m.id,"
                            "  m.created_at,"
                            "  m.sender,"
                            "  m.body,"
                            "  COALESCE(m.reply_to_message_id, 0),"
                            "  COALESCE(reply.sender, ''),"
                            "  COALESCE(CASE WHEN reply.deleted_at IS NOT NULL THEN ? ELSE reply.body END, ''),"
                            "  COALESCE(m.forward_from_message_id, 0),"
                            "  COALESCE(m.forward_from_sender, COALESCE(fwd.sender, '')),"
                            "  COALESCE(m.forward_from_text, COALESCE(CASE WHEN fwd.deleted_at IS NOT NULL THEN ? ELSE fwd.body END, '')),"
                            "  COALESCE(m.deleted_at, ''),"
                            "  COALESCE(m.deleted_by, '') "
                            "FROM ("
                            "  SELECT id, created_at, sender, body, reply_to_message_id, forward_from_message_id, "
                            "         forward_from_sender, forward_from_text, deleted_at, deleted_by "
                            "  FROM group_messages "
                            "  WHERE group_id = ? "
                            "  ORDER BY id DESC "
                            "  LIMIT ?"
                            ") m "
                            "LEFT JOIN group_messages reply ON reply.id = m.reply_to_message_id "
                            "LEFT JOIN group_messages fwd ON fwd.id = m.forward_from_message_id "
                            "ORDER BY m.id ASC;",
                            error);
        if (!statement) {
            return false;
        }

        sqlite3_bind_text(statement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement.get(), 2, kDeletedMessageText, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement.get(), 3, groupId);
        sqlite3_bind_int(statement.get(), 4, limit);

        while (true) {
            const int result = sqlite3_step(statement.get());
            if (result == SQLITE_DONE) {
                Statement upsert(db_,
                                 "INSERT INTO group_reads(group_id, username, last_read_message_id, updated_at) "
                                 "SELECT ?, ?, COALESCE(MAX(id), 0), CURRENT_TIMESTAMP FROM group_messages WHERE group_id = ? "
                                 "ON CONFLICT(group_id, username) DO UPDATE SET "
                                 "last_read_message_id = excluded.last_read_message_id, "
                                 "updated_at = CURRENT_TIMESTAMP;",
                                 error);
                if (!upsert) {
                    return false;
                }
                sqlite3_bind_int64(upsert.get(), 1, groupId);
                sqlite3_bind_text(upsert.get(), 2, user.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(upsert.get(), 3, groupId);
                if (sqlite3_step(upsert.get()) != SQLITE_DONE) {
                    error = lastError(db_);
                    return false;
                }
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

            StoredMessage item;
            item.id = sqlite3_column_int64(statement.get(), 0);
            item.createdAt = columnText(1);
            item.sender = columnText(2);
            item.recipient = groupName;
            item.text = columnText(3);
            item.replyToMessageId = sqlite3_column_int64(statement.get(), 4);
            item.replyToSender = columnText(5);
            item.replyToText = columnText(6);
            item.forwardFromMessageId = sqlite3_column_int64(statement.get(), 7);
            item.forwardFromSender = columnText(8);
            item.forwardFromText = columnText(9);
            item.deletedAt = columnText(10);
            item.deletedBy = columnText(11);
            messages.push_back(item);
        }
    }

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
                        "  COALESCE(CASE WHEN reply.deleted_at IS NOT NULL THEN ? ELSE reply.body END, ''),"
                        "  COALESCE(m.forward_from_message_id, 0),"
                        "  COALESCE(m.forward_from_sender, COALESCE(fwd.sender, '')),"
                        "  COALESCE(m.forward_from_text, COALESCE(CASE WHEN fwd.deleted_at IS NOT NULL THEN ? ELSE fwd.body END, '')),"
                        "  COALESCE(m.deleted_at, ''),"
                        "  COALESCE(m.deleted_by, '') "
                        "FROM ("
                        "  SELECT id, created_at, sender, recipient, body, reply_to_message_id, forward_from_message_id, "
                        "         forward_from_sender, forward_from_text, deleted_at, deleted_by "
                        "  FROM messages "
                        "  WHERE conversation_id = ? "
                        "  ORDER BY id DESC "
                        "  LIMIT ?"
                        ") m "
                        "LEFT JOIN messages reply ON reply.id = m.reply_to_message_id "
                        "LEFT JOIN messages fwd ON fwd.id = m.forward_from_message_id "
                        "ORDER BY m.id ASC;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.get(), 3, conversationId);
    sqlite3_bind_int(statement.get(), 4, limit);

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
            columnText(7),
            sqlite3_column_int64(statement.get(), 8),
            columnText(9),
            columnText(10),
            columnText(11),
            columnText(12)
        });
    }
}

bool MessageStore::fetchHistoryBefore(const std::string& user,
                                      const std::string& peer,
                                      long long beforeMessageId,
                                      int limit,
                                      std::vector<StoredMessage>& messages,
                                      std::string& error) {
    messages.clear();

    std::lock_guard<std::mutex> lock(mutex_);

    long long groupId = 0;
    if (parseGroupChatId(peer, groupId)) {
        bool isMember = false;
        if (!isGroupMemberLocked(groupId, user, isMember, error)) {
            return false;
        }

        if (!isMember) {
            error = "group not found";
            return false;
        }

        std::string groupName;
        std::string adminUsername;
        std::vector<std::string> memberUsernames;
        if (!loadGroupContextLocked(groupId, groupName, adminUsername, memberUsernames, error)) {
            return false;
        }

        Statement statement(db_,
                            "SELECT "
                            "  m.id,"
                            "  m.created_at,"
                            "  m.sender,"
                            "  m.body,"
                            "  COALESCE(m.reply_to_message_id, 0),"
                            "  COALESCE(reply.sender, ''),"
                            "  COALESCE(CASE WHEN reply.deleted_at IS NOT NULL THEN ? ELSE reply.body END, ''),"
                            "  COALESCE(m.forward_from_message_id, 0),"
                            "  COALESCE(m.forward_from_sender, COALESCE(fwd.sender, '')),"
                            "  COALESCE(m.forward_from_text, COALESCE(CASE WHEN fwd.deleted_at IS NOT NULL THEN ? ELSE fwd.body END, '')),"
                            "  COALESCE(m.deleted_at, ''),"
                            "  COALESCE(m.deleted_by, '') "
                            "FROM ("
                            "  SELECT id, created_at, sender, body, reply_to_message_id, forward_from_message_id, "
                            "         forward_from_sender, forward_from_text, deleted_at, deleted_by "
                            "  FROM group_messages "
                            "  WHERE group_id = ? AND id < ? "
                            "  ORDER BY id DESC "
                            "  LIMIT ?"
                            ") m "
                            "LEFT JOIN group_messages reply ON reply.id = m.reply_to_message_id "
                            "LEFT JOIN group_messages fwd ON fwd.id = m.forward_from_message_id "
                            "ORDER BY m.id ASC;",
                            error);
        if (!statement) {
            return false;
        }

        sqlite3_bind_text(statement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement.get(), 2, kDeletedMessageText, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement.get(), 3, groupId);
        sqlite3_bind_int64(statement.get(), 4, beforeMessageId);
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

            auto columnText = [&](int column) -> std::string {
                const auto* value = sqlite3_column_text(statement.get(), column);
                return value ? reinterpret_cast<const char*>(value) : "";
            };

            StoredMessage item;
            item.id = sqlite3_column_int64(statement.get(), 0);
            item.createdAt = columnText(1);
            item.sender = columnText(2);
            item.recipient = groupName;
            item.text = columnText(3);
            item.replyToMessageId = sqlite3_column_int64(statement.get(), 4);
            item.replyToSender = columnText(5);
            item.replyToText = columnText(6);
            item.forwardFromMessageId = sqlite3_column_int64(statement.get(), 7);
            item.forwardFromSender = columnText(8);
            item.forwardFromText = columnText(9);
            item.deletedAt = columnText(10);
            item.deletedBy = columnText(11);
            messages.push_back(item);
        }
    }

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
                        "  COALESCE(CASE WHEN reply.deleted_at IS NOT NULL THEN ? ELSE reply.body END, ''),"
                        "  COALESCE(m.forward_from_message_id, 0),"
                        "  COALESCE(m.forward_from_sender, COALESCE(fwd.sender, '')),"
                        "  COALESCE(m.forward_from_text, COALESCE(CASE WHEN fwd.deleted_at IS NOT NULL THEN ? ELSE fwd.body END, '')),"
                        "  COALESCE(m.deleted_at, ''),"
                        "  COALESCE(m.deleted_by, '') "
                        "FROM ("
                        "  SELECT id, created_at, sender, recipient, body, reply_to_message_id, forward_from_message_id, "
                        "         forward_from_sender, forward_from_text, deleted_at, deleted_by "
                        "  FROM messages "
                        "  WHERE conversation_id = ? AND id < ? "
                        "  ORDER BY id DESC "
                        "  LIMIT ?"
                        ") m "
                        "LEFT JOIN messages reply ON reply.id = m.reply_to_message_id "
                        "LEFT JOIN messages fwd ON fwd.id = m.forward_from_message_id "
                        "ORDER BY m.id ASC;",
                        error);
    if (!statement) {
        return false;
    }

    sqlite3_bind_text(statement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement.get(), 3, conversationId);
    sqlite3_bind_int64(statement.get(), 4, beforeMessageId);
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
            columnText(7),
            sqlite3_column_int64(statement.get(), 8),
            columnText(9),
            columnText(10),
            columnText(11),
            columnText(12)
        });
    }
}

bool MessageStore::markConversationRead(const std::string& user,
                                        const std::string& peer,
                                        std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);

    long long groupId = 0;
    if (parseGroupChatId(peer, groupId)) {
        bool isMember = false;
        if (!isGroupMemberLocked(groupId, user, isMember, error)) {
            return false;
        }
        if (!isMember) {
            return true;
        }

        Statement upsert(db_,
                         "INSERT INTO group_reads(group_id, username, last_read_message_id, updated_at) "
                         "SELECT ?, ?, COALESCE(MAX(id), 0), CURRENT_TIMESTAMP FROM group_messages WHERE group_id = ? "
                         "ON CONFLICT(group_id, username) DO UPDATE SET "
                         "last_read_message_id = excluded.last_read_message_id, "
                         "updated_at = CURRENT_TIMESTAMP;",
                         error);
        if (!upsert) {
            return false;
        }
        sqlite3_bind_int64(upsert.get(), 1, groupId);
        sqlite3_bind_text(upsert.get(), 2, user.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(upsert.get(), 3, groupId);
        if (sqlite3_step(upsert.get()) != SQLITE_DONE) {
            error = lastError(db_);
            return false;
        }
        return true;
    }

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

bool MessageStore::deleteMessage(const std::string& requester,
                                 long long messageId,
                                 StoredMessage& storedMessage,
                                 std::string& chatId,
                                 std::vector<std::string>& audienceUsernames,
                                 std::string& error) {
    storedMessage = StoredMessage{};
    chatId.clear();
    audienceUsernames.clear();
    std::lock_guard<std::mutex> lock(mutex_);

    Statement directLookup(db_,
                           "SELECT m.sender, m.recipient, COALESCE(m.deleted_at, '') "
                           "FROM messages m "
                           "JOIN conversations c ON c.id = m.conversation_id "
                           "WHERE m.id = ? AND (c.user_low = ? OR c.user_high = ?);",
                           error);
    if (!directLookup) {
        return false;
    }
    sqlite3_bind_int64(directLookup.get(), 1, messageId);
    sqlite3_bind_text(directLookup.get(), 2, requester.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(directLookup.get(), 3, requester.c_str(), -1, SQLITE_TRANSIENT);

    const int directResult = sqlite3_step(directLookup.get());
    if (directResult == SQLITE_ROW) {
        const auto* rawSender = sqlite3_column_text(directLookup.get(), 0);
        const auto* rawRecipient = sqlite3_column_text(directLookup.get(), 1);
        const auto* rawDeletedAt = sqlite3_column_text(directLookup.get(), 2);
        const std::string sender = rawSender ? reinterpret_cast<const char*>(rawSender) : "";
        const std::string recipient = rawRecipient ? reinterpret_cast<const char*>(rawRecipient) : "";
        const std::string deletedAt = rawDeletedAt ? reinterpret_cast<const char*>(rawDeletedAt) : "";
        if (sender != requester) {
            error = "cannot_delete_other_user_message";
            return false;
        }

        if (deletedAt.empty()) {
            Statement update(db_,
                             "UPDATE messages SET deleted_at = CURRENT_TIMESTAMP, deleted_by = ? WHERE id = ?;",
                             error);
            if (!update) {
                return false;
            }
            sqlite3_bind_text(update.get(), 1, requester.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(update.get(), 2, messageId);
            if (sqlite3_step(update.get()) != SQLITE_DONE) {
                error = lastError(db_);
                return false;
            }
        }

        Statement directLoad(db_,
                             "SELECT m.id, m.created_at, m.sender, m.recipient, m.body, "
                             "       COALESCE(m.reply_to_message_id, 0), "
                             "       COALESCE(reply.sender, ''), "
                             "       COALESCE(CASE WHEN reply.deleted_at IS NOT NULL THEN ? ELSE reply.body END, ''), "
                             "       COALESCE(m.forward_from_message_id, 0), "
                             "       COALESCE(fwd.sender, ''), "
                             "       COALESCE(CASE WHEN fwd.deleted_at IS NOT NULL THEN ? ELSE fwd.body END, ''), "
                             "       COALESCE(m.deleted_at, ''), "
                             "       COALESCE(m.deleted_by, '') "
                             "FROM messages m "
                             "LEFT JOIN messages reply ON reply.id = m.reply_to_message_id "
                             "LEFT JOIN messages fwd ON fwd.id = m.forward_from_message_id "
                             "WHERE m.id = ?;",
                             error);
        if (!directLoad) {
            return false;
        }
        sqlite3_bind_text(directLoad.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(directLoad.get(), 2, kDeletedMessageText, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(directLoad.get(), 3, messageId);
        if (sqlite3_step(directLoad.get()) != SQLITE_ROW) {
            error = "message_not_found";
            return false;
        }
        auto directText = [&](int column) -> std::string {
            const auto* value = sqlite3_column_text(directLoad.get(), column);
            return value ? reinterpret_cast<const char*>(value) : "";
        };
        storedMessage.id = sqlite3_column_int64(directLoad.get(), 0);
        storedMessage.createdAt = directText(1);
        storedMessage.sender = directText(2);
        storedMessage.recipient = directText(3);
        storedMessage.text = directText(4);
        storedMessage.replyToMessageId = sqlite3_column_int64(directLoad.get(), 5);
        storedMessage.replyToSender = directText(6);
        storedMessage.replyToText = directText(7);
        storedMessage.forwardFromMessageId = sqlite3_column_int64(directLoad.get(), 8);
        storedMessage.forwardFromSender = directText(9);
        storedMessage.forwardFromText = directText(10);
        storedMessage.deletedAt = directText(11);
        storedMessage.deletedBy = directText(12);

        audienceUsernames.push_back(sender);
        if (recipient != sender) {
            audienceUsernames.push_back(recipient);
        }
        return true;
    }
    if (directResult != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }

    Statement groupLookup(db_,
                          "SELECT m.group_id, m.sender, COALESCE(m.deleted_at, '') "
                          "FROM group_messages m "
                          "JOIN group_members gm ON gm.group_id = m.group_id AND gm.username = ? "
                          "WHERE m.id = ?;",
                          error);
    if (!groupLookup) {
        return false;
    }
    sqlite3_bind_text(groupLookup.get(), 1, requester.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(groupLookup.get(), 2, messageId);

    const int groupResult = sqlite3_step(groupLookup.get());
    if (groupResult != SQLITE_ROW) {
        if (groupResult == SQLITE_DONE) {
            error = "message_not_found";
        } else {
            error = lastError(db_);
        }
        return false;
    }

    const long long groupId = sqlite3_column_int64(groupLookup.get(), 0);
    const auto* rawGroupSender = sqlite3_column_text(groupLookup.get(), 1);
    const auto* rawGroupDeletedAt = sqlite3_column_text(groupLookup.get(), 2);
    const std::string sender = rawGroupSender ? reinterpret_cast<const char*>(rawGroupSender) : "";
    const std::string deletedAt = rawGroupDeletedAt ? reinterpret_cast<const char*>(rawGroupDeletedAt) : "";
    if (sender != requester) {
        error = "cannot_delete_other_user_message";
        return false;
    }

    if (deletedAt.empty()) {
        Statement update(db_,
                         "UPDATE group_messages SET deleted_at = CURRENT_TIMESTAMP, deleted_by = ? WHERE id = ?;",
                         error);
        if (!update) {
            return false;
        }
        sqlite3_bind_text(update.get(), 1, requester.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(update.get(), 2, messageId);
        if (sqlite3_step(update.get()) != SQLITE_DONE) {
            error = lastError(db_);
            return false;
        }
    }

    std::string groupName;
    std::string adminUsername;
    if (!loadGroupContextLocked(groupId, groupName, adminUsername, audienceUsernames, error)) {
        return false;
    }
    if (groupName.empty()) {
        error = "group_not_found";
        return false;
    }

    Statement groupLoad(db_,
                        "SELECT m.id, m.created_at, m.sender, g.name, m.body, "
                        "       COALESCE(m.reply_to_message_id, 0), "
                        "       COALESCE(reply.sender, ''), "
                        "       COALESCE(CASE WHEN reply.deleted_at IS NOT NULL THEN ? ELSE reply.body END, ''), "
                        "       COALESCE(m.forward_from_message_id, 0), "
                        "       COALESCE(fwd.sender, ''), "
                        "       COALESCE(CASE WHEN fwd.deleted_at IS NOT NULL THEN ? ELSE fwd.body END, ''), "
                        "       COALESCE(m.deleted_at, ''), "
                        "       COALESCE(m.deleted_by, '') "
                        "FROM group_messages m "
                        "JOIN groups_chat g ON g.id = m.group_id "
                        "LEFT JOIN group_messages reply ON reply.id = m.reply_to_message_id "
                        "LEFT JOIN group_messages fwd ON fwd.id = m.forward_from_message_id "
                        "WHERE m.id = ? AND m.group_id = ?;",
                        error);
    if (!groupLoad) {
        return false;
    }
    sqlite3_bind_text(groupLoad.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(groupLoad.get(), 2, kDeletedMessageText, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(groupLoad.get(), 3, messageId);
    sqlite3_bind_int64(groupLoad.get(), 4, groupId);
    if (sqlite3_step(groupLoad.get()) != SQLITE_ROW) {
        error = "message_not_found";
        return false;
    }
    auto groupText = [&](int column) -> std::string {
        const auto* value = sqlite3_column_text(groupLoad.get(), column);
        return value ? reinterpret_cast<const char*>(value) : "";
    };
    storedMessage.id = sqlite3_column_int64(groupLoad.get(), 0);
    storedMessage.createdAt = groupText(1);
    storedMessage.sender = groupText(2);
    storedMessage.recipient = groupText(3);
    storedMessage.text = groupText(4);
    storedMessage.replyToMessageId = sqlite3_column_int64(groupLoad.get(), 5);
    storedMessage.replyToSender = groupText(6);
    storedMessage.replyToText = groupText(7);
    storedMessage.forwardFromMessageId = sqlite3_column_int64(groupLoad.get(), 8);
    storedMessage.forwardFromSender = groupText(9);
    storedMessage.forwardFromText = groupText(10);
    storedMessage.deletedAt = groupText(11);
    storedMessage.deletedBy = groupText(12);

    chatId = groupChatId(groupId);
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

bool MessageStore::createGroup(const std::string& creator,
                               const std::string& name,
                               const std::string& adminUsername,
                               const std::vector<std::string>& members,
                               long long& groupId,
                               std::string& error) {
    groupId = 0;
    std::lock_guard<std::mutex> lock(mutex_);

    bool creatorExists = false;
    bool adminExists = false;
    if (!userExistsLocked(creator, creatorExists, error) ||
        !userExistsLocked(adminUsername, adminExists, error)) {
        return false;
    }
    if (!creatorExists || !adminExists) {
        error = "unknown user";
        return false;
    }

    if (!executeLocked("BEGIN IMMEDIATE TRANSACTION;", error)) {
        return false;
    }

    Statement createGroupStatement(db_,
                                   "INSERT INTO groups_chat(name, admin_username) VALUES (?, ?);",
                                   error);
    if (!createGroupStatement) {
        executeLocked("ROLLBACK;", error);
        return false;
    }
    sqlite3_bind_text(createGroupStatement.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(createGroupStatement.get(), 2, adminUsername.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(createGroupStatement.get()) != SQLITE_DONE) {
        error = lastError(db_);
        executeLocked("ROLLBACK;", error);
        return false;
    }

    groupId = sqlite3_last_insert_rowid(db_);

    Statement memberStatement(db_,
                              "INSERT OR IGNORE INTO group_members(group_id, username) VALUES (?, ?);",
                              error);
    if (!memberStatement) {
        executeLocked("ROLLBACK;", error);
        return false;
    }

    for (const auto& username : members) {
        sqlite3_reset(memberStatement.get());
        sqlite3_clear_bindings(memberStatement.get());
        sqlite3_bind_int64(memberStatement.get(), 1, groupId);
        sqlite3_bind_text(memberStatement.get(), 2, username.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(memberStatement.get()) != SQLITE_DONE) {
            error = lastError(db_);
            executeLocked("ROLLBACK;", error);
            return false;
        }
    }

    if (!executeLocked("COMMIT;", error)) {
        executeLocked("ROLLBACK;", error);
        return false;
    }

    return true;
}

bool MessageStore::fetchGroupInfo(const std::string& requester,
                                  long long groupId,
                                  GroupInfo& info,
                                  std::vector<GroupMemberInfo>& members,
                                  std::string& error) {
    info = GroupInfo{};
    members.clear();
    std::lock_guard<std::mutex> lock(mutex_);

    bool isMember = false;
    if (!isGroupMemberLocked(groupId, requester, isMember, error)) {
        return false;
    }
    if (!isMember) {
        error = "group not found";
        return false;
    }

    Statement infoStatement(db_,
                            "SELECT name, admin_username FROM groups_chat WHERE id = ?;",
                            error);
    if (!infoStatement) {
        return false;
    }
    sqlite3_bind_int64(infoStatement.get(), 1, groupId);
    const int infoResult = sqlite3_step(infoStatement.get());
    if (infoResult != SQLITE_ROW) {
        error = infoResult == SQLITE_DONE ? "group not found" : lastError(db_);
        return false;
    }

    const auto* rawName = sqlite3_column_text(infoStatement.get(), 0);
    const auto* rawAdmin = sqlite3_column_text(infoStatement.get(), 1);
    info.chatId = groupChatId(groupId);
    info.name = rawName ? reinterpret_cast<const char*>(rawName) : "";
    info.adminUsername = rawAdmin ? reinterpret_cast<const char*>(rawAdmin) : "";
    info.canManage = info.adminUsername == requester;

    Statement membersStatement(db_,
                               "SELECT username FROM group_members WHERE group_id = ? ORDER BY username ASC;",
                               error);
    if (!membersStatement) {
        return false;
    }
    sqlite3_bind_int64(membersStatement.get(), 1, groupId);

    while (true) {
      const int result = sqlite3_step(membersStatement.get());
      if (result == SQLITE_DONE) {
        return true;
      }
      if (result != SQLITE_ROW) {
        error = lastError(db_);
        return false;
      }
      const auto* rawUser = sqlite3_column_text(membersStatement.get(), 0);
      const std::string username = rawUser ? reinterpret_cast<const char*>(rawUser) : "";
      members.push_back({username, username == info.adminUsername});
    }
}

bool MessageStore::addGroupMembers(const std::string& requester,
                                   long long groupId,
                                   const std::vector<std::string>& usernames,
                                   std::vector<std::string>& addedUsers,
                                   std::string& error) {
    addedUsers.clear();
    std::lock_guard<std::mutex> lock(mutex_);

    std::string groupName;
    std::string adminUsername;
    std::vector<std::string> memberUsernames;
    if (!loadGroupContextLocked(groupId, groupName, adminUsername, memberUsernames, error)) {
        return false;
    }
    if (groupName.empty()) {
        error = "group not found";
        return false;
    }
    if (adminUsername != requester) {
        error = "only admin can add members";
        return false;
    }

    Statement insert(db_,
                     "INSERT OR IGNORE INTO group_members(group_id, username) VALUES (?, ?);",
                     error);
    if (!insert) {
        return false;
    }

    for (const auto& username : usernames) {
        bool exists = false;
        if (!userExistsLocked(username, exists, error)) {
            return false;
        }
        if (!exists) {
            continue;
        }

        sqlite3_reset(insert.get());
        sqlite3_clear_bindings(insert.get());
        sqlite3_bind_int64(insert.get(), 1, groupId);
        sqlite3_bind_text(insert.get(), 2, username.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert.get()) != SQLITE_DONE) {
            error = lastError(db_);
            return false;
        }
        if (sqlite3_changes(db_) > 0) {
            addedUsers.push_back(username);
        }
    }
    return true;
}

bool MessageStore::removeGroupMember(const std::string& requester,
                                     long long groupId,
                                     const std::string& username,
                                     std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string groupName;
    std::string adminUsername;
    std::vector<std::string> memberUsernames;
    if (!loadGroupContextLocked(groupId, groupName, adminUsername, memberUsernames, error)) {
        return false;
    }
    if (groupName.empty()) {
        error = "group not found";
        return false;
    }
    if (adminUsername != requester) {
        error = "only admin can remove members";
        return false;
    }
    if (username == requester) {
        error = "admin cannot remove themselves";
        return false;
    }

    Statement removeStatement(db_,
                              "DELETE FROM group_members WHERE group_id = ? AND username = ?;",
                              error);
    if (!removeStatement) {
        return false;
    }
    sqlite3_bind_int64(removeStatement.get(), 1, groupId);
    sqlite3_bind_text(removeStatement.get(), 2, username.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(removeStatement.get()) != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        error = "member not found";
        return false;
    }
    return true;
}

bool MessageStore::transferGroupAdmin(const std::string& requester,
                                      long long groupId,
                                      const std::string& newAdminUsername,
                                      std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string groupName;
    std::string adminUsername;
    std::vector<std::string> memberUsernames;
    if (!loadGroupContextLocked(groupId, groupName, adminUsername, memberUsernames, error)) {
        return false;
    }
    if (groupName.empty()) {
        error = "group not found";
        return false;
    }
    if (adminUsername != requester) {
        error = "only admin can transfer admin";
        return false;
    }
    bool isMember = false;
    if (!isGroupMemberLocked(groupId, newAdminUsername, isMember, error)) {
        return false;
    }
    if (!isMember) {
        error = "new admin must be a group member";
        return false;
    }

    Statement updateStatement(db_,
                              "UPDATE groups_chat SET admin_username = ? WHERE id = ?;",
                              error);
    if (!updateStatement) {
        return false;
    }
    sqlite3_bind_text(updateStatement.get(), 1, newAdminUsername.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(updateStatement.get(), 2, groupId);
    if (sqlite3_step(updateStatement.get()) != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }
    return true;
}

bool MessageStore::leaveGroup(const std::string& requester,
                              long long groupId,
                              std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string groupName;
    std::string adminUsername;
    std::vector<std::string> memberUsernames;
    if (!loadGroupContextLocked(groupId, groupName, adminUsername, memberUsernames, error)) {
        return false;
    }
    if (groupName.empty()) {
        error = "group not found";
        return false;
    }
    if (adminUsername == requester) {
        error = "admin cannot leave the group";
        return false;
    }

    Statement deleteStatement(db_,
                              "DELETE FROM group_members WHERE group_id = ? AND username = ?;",
                              error);
    if (!deleteStatement) {
        return false;
    }
    sqlite3_bind_int64(deleteStatement.get(), 1, groupId);
    sqlite3_bind_text(deleteStatement.get(), 2, requester.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(deleteStatement.get()) != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        error = "group not found";
        return false;
    }
    return true;
}

bool MessageStore::deleteGroup(const std::string& requester,
                               long long groupId,
                               std::vector<std::string>& removedUsers,
                               std::string& error) {
    removedUsers.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    std::string groupName;
    std::string adminUsername;
    if (!loadGroupContextLocked(groupId, groupName, adminUsername, removedUsers, error)) {
        return false;
    }
    if (groupName.empty()) {
        error = "group not found";
        return false;
    }
    if (adminUsername != requester) {
        error = "only admin can delete the group";
        return false;
    }

    Statement deleteStatement(db_,
                              "DELETE FROM groups_chat WHERE id = ?;",
                              error);
    if (!deleteStatement) {
        return false;
    }
    sqlite3_bind_int64(deleteStatement.get(), 1, groupId);
    if (sqlite3_step(deleteStatement.get()) != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }
    return true;
}

bool MessageStore::saveGroupMessage(const std::string& sender,
                                    long long groupId,
                                    const std::string& text,
                                    long long replyToMessageId,
                                    long long forwardFromMessageId,
                                    const std::string& forwardFromSenderOverride,
                                    const std::string& forwardFromTextOverride,
                                    StoredMessage& storedMessage,
                                    std::string& groupName,
                                    std::vector<std::string>& memberUsernames,
                                    std::string& error) {
    storedMessage = StoredMessage{};
    std::lock_guard<std::mutex> lock(mutex_);
    std::string adminUsername;
    if (!loadGroupContextLocked(groupId, groupName, adminUsername, memberUsernames, error)) {
        return false;
    }
    if (groupName.empty()) {
        error = "group not found";
        return false;
    }
    if (sender != "System") {
        bool isMember = false;
        if (!isGroupMemberLocked(groupId, sender, isMember, error)) {
            return false;
        }
        if (!isMember) {
            error = "you are not a member of this group";
            return false;
        }
    }

    std::string replyToSender;
    std::string replyToText;
    if (replyToMessageId > 0) {
        Statement replyStatement(db_,
                                 "SELECT sender, CASE WHEN deleted_at IS NOT NULL THEN ? ELSE body END "
                                 "FROM group_messages WHERE id = ? AND group_id = ?;",
                                 error);
        if (!replyStatement) {
            return false;
        }
        sqlite3_bind_text(replyStatement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(replyStatement.get(), 2, replyToMessageId);
        sqlite3_bind_int64(replyStatement.get(), 3, groupId);
        const int replyResult = sqlite3_step(replyStatement.get());
        if (replyResult != SQLITE_ROW) {
            error = replyResult == SQLITE_DONE ? "reply target not found" : lastError(db_);
            return false;
        }
        const auto* rawSender = sqlite3_column_text(replyStatement.get(), 0);
        const auto* rawText = sqlite3_column_text(replyStatement.get(), 1);
        replyToSender = rawSender ? reinterpret_cast<const char*>(rawSender) : "";
        replyToText = rawText ? reinterpret_cast<const char*>(rawText) : "";
    }

    std::string forwardFromSender;
    std::string forwardFromText;
    if (forwardFromMessageId > 0) {
        if (!forwardFromSenderOverride.empty() || !forwardFromTextOverride.empty()) {
            forwardFromSender = forwardFromSenderOverride;
            forwardFromText = forwardFromTextOverride;
        } else {
            Statement forwardStatement(db_,
                                       "SELECT m.sender, CASE WHEN m.deleted_at IS NOT NULL THEN ? ELSE m.body END "
                                       "FROM group_messages m "
                                       "JOIN group_members gm ON gm.group_id = m.group_id AND gm.username = ? "
                                       "WHERE m.id = ?;",
                                       error);
            if (!forwardStatement) {
                return false;
            }
            sqlite3_bind_text(forwardStatement.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(forwardStatement.get(), 2, sender.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(forwardStatement.get(), 3, forwardFromMessageId);
            const int forwardResult = sqlite3_step(forwardStatement.get());
            if (forwardResult == SQLITE_ROW) {
                const auto* rawSender = sqlite3_column_text(forwardStatement.get(), 0);
                const auto* rawText = sqlite3_column_text(forwardStatement.get(), 1);
                forwardFromSender = rawSender ? reinterpret_cast<const char*>(rawSender) : "";
                forwardFromText = rawText ? reinterpret_cast<const char*>(rawText) : "";
            } else if (forwardResult == SQLITE_DONE) {
                Statement directForward(db_,
                                        "SELECT m.sender, CASE WHEN m.deleted_at IS NOT NULL THEN ? ELSE m.body END "
                                        "FROM messages m "
                                        "JOIN conversations c ON c.id = m.conversation_id "
                                        "WHERE m.id = ? AND (c.user_low = ? OR c.user_high = ?);",
                                        error);
                if (!directForward) {
                    return false;
                }
                sqlite3_bind_text(directForward.get(), 1, kDeletedMessageText, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(directForward.get(), 2, forwardFromMessageId);
                sqlite3_bind_text(directForward.get(), 3, sender.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(directForward.get(), 4, sender.c_str(), -1, SQLITE_TRANSIENT);
                const int directResult = sqlite3_step(directForward.get());
                if (directResult != SQLITE_ROW) {
                    error = directResult == SQLITE_DONE ? "forward target not found" : lastError(db_);
                    return false;
                }
                const auto* rawSender = sqlite3_column_text(directForward.get(), 0);
                const auto* rawText = sqlite3_column_text(directForward.get(), 1);
                forwardFromSender = rawSender ? reinterpret_cast<const char*>(rawSender) : "";
                forwardFromText = rawText ? reinterpret_cast<const char*>(rawText) : "";
            } else {
                error = lastError(db_);
                return false;
            }
        }
    }

    Statement insert(db_,
                     "INSERT INTO group_messages("
                     "group_id, sender, body, reply_to_message_id, forward_from_message_id, forward_from_sender, forward_from_text"
                     ") VALUES (?, ?, ?, NULLIF(?, 0), NULLIF(?, 0), NULLIF(?, ''), NULLIF(?, ''));",
                     error);
    if (!insert) {
        return false;
    }
    sqlite3_bind_int64(insert.get(), 1, groupId);
    sqlite3_bind_text(insert.get(), 2, sender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.get(), 3, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.get(), 4, replyToMessageId);
    sqlite3_bind_int64(insert.get(), 5, forwardFromMessageId);
    sqlite3_bind_text(insert.get(), 6, forwardFromSender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.get(), 7, forwardFromText.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) {
        error = lastError(db_);
        return false;
    }

    storedMessage.id = sqlite3_last_insert_rowid(db_);
    storedMessage.sender = sender;
    storedMessage.recipient = groupName;
    storedMessage.text = text;
    storedMessage.replyToMessageId = replyToMessageId;
    storedMessage.replyToSender = replyToSender;
    storedMessage.replyToText = replyToText;
    storedMessage.forwardFromMessageId = forwardFromMessageId;
    storedMessage.forwardFromSender = forwardFromSender;
    storedMessage.forwardFromText = forwardFromText;

    Statement createdAtStatement(db_,
                                 "SELECT created_at FROM group_messages WHERE id = ?;",
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
    storedMessage.createdAt = rawCreatedAt ? reinterpret_cast<const char*>(rawCreatedAt) : "";
    return true;
}

bool MessageStore::saveGroupSystemMessage(long long groupId,
                                          const std::string& text,
                                          StoredMessage& storedMessage,
                                          std::string& groupName,
                                          std::vector<std::string>& memberUsernames,
                                          std::string& error) {
    return saveGroupMessage("System",
                            groupId,
                            text,
                            0,
                            0,
                            "",
                            "",
                            storedMessage,
                            groupName,
                            memberUsernames,
                            error);
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
        "  display_name TEXT,"
        "  bio TEXT DEFAULT '',"
        "  avatar_color TEXT DEFAULT '',"
        "  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  last_seen TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS conversations ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  user_low TEXT NOT NULL,"
        "  user_high TEXT NOT NULL,"
        "  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE(user_low, user_high)"
        ");"
        "CREATE TABLE IF NOT EXISTS groups_chat ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  admin_username TEXT NOT NULL,"
        "  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS group_members ("
        "  group_id INTEGER NOT NULL,"
        "  username TEXT NOT NULL,"
        "  joined_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE(group_id, username),"
        "  FOREIGN KEY(group_id) REFERENCES groups_chat(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  conversation_id INTEGER NOT NULL,"
        "  sender TEXT NOT NULL,"
        "  recipient TEXT NOT NULL,"
        "  body TEXT NOT NULL,"
        "  reply_to_message_id INTEGER,"
        "  forward_from_message_id INTEGER,"
        "  forward_from_sender TEXT,"
        "  forward_from_text TEXT,"
        "  deleted_at TEXT,"
        "  deleted_by TEXT,"
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
        "ON conversation_reads(username, conversation_id);"
        "CREATE TABLE IF NOT EXISTS group_messages ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  group_id INTEGER NOT NULL,"
        "  sender TEXT NOT NULL,"
        "  body TEXT NOT NULL,"
        "  reply_to_message_id INTEGER,"
        "  forward_from_message_id INTEGER,"
        "  forward_from_sender TEXT,"
        "  forward_from_text TEXT,"
        "  deleted_at TEXT,"
        "  deleted_by TEXT,"
        "  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY(group_id) REFERENCES groups_chat(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS group_reads ("
        "  group_id INTEGER NOT NULL,"
        "  username TEXT NOT NULL,"
        "  last_read_message_id INTEGER NOT NULL DEFAULT 0,"
        "  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE(group_id, username),"
        "  FOREIGN KEY(group_id) REFERENCES groups_chat(id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_group_members_username "
        "ON group_members(username, group_id);"
        "CREATE INDEX IF NOT EXISTS idx_group_messages_group_id "
        "ON group_messages(group_id, id);"
        "CREATE INDEX IF NOT EXISTS idx_group_reads_user "
        "ON group_reads(username, group_id);",
        error)) {
        return false;
    }

    return ensureColumnLocked("users", "password_salt", "TEXT", error) &&
           ensureColumnLocked("users", "password_hash", "TEXT", error) &&
           ensureColumnLocked("users", "display_name", "TEXT", error) &&
           ensureColumnLocked("users", "bio", "TEXT DEFAULT ''", error) &&
           ensureColumnLocked("users", "avatar_color", "TEXT DEFAULT ''", error) &&
           ensureColumnLocked("users", "created_at", "TEXT", error) &&
           ensureColumnLocked("users", "last_seen", "TEXT", error) &&
           ensureColumnLocked("messages", "reply_to_message_id", "INTEGER", error) &&
           ensureColumnLocked("messages", "forward_from_message_id", "INTEGER", error) &&
           ensureColumnLocked("messages", "forward_from_sender", "TEXT", error) &&
           ensureColumnLocked("messages", "forward_from_text", "TEXT", error) &&
           ensureColumnLocked("messages", "deleted_at", "TEXT", error) &&
           ensureColumnLocked("messages", "deleted_by", "TEXT", error) &&
           ensureColumnLocked("group_messages", "reply_to_message_id", "INTEGER", error) &&
           ensureColumnLocked("group_messages", "forward_from_message_id", "INTEGER", error) &&
           ensureColumnLocked("group_messages", "forward_from_sender", "TEXT", error) &&
           ensureColumnLocked("group_messages", "forward_from_text", "TEXT", error) &&
           ensureColumnLocked("group_messages", "deleted_at", "TEXT", error) &&
           ensureColumnLocked("group_messages", "deleted_by", "TEXT", error);
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

bool MessageStore::loadGroupContextLocked(long long groupId,
                                          std::string& groupName,
                                          std::string& adminUsername,
                                          std::vector<std::string>& memberUsernames,
                                          std::string& error) {
    groupName.clear();
    adminUsername.clear();
    memberUsernames.clear();

    Statement groupStatement(db_,
                             "SELECT name, admin_username FROM groups_chat WHERE id = ?;",
                             error);
    if (!groupStatement) {
        return false;
    }
    sqlite3_bind_int64(groupStatement.get(), 1, groupId);
    const int groupResult = sqlite3_step(groupStatement.get());
    if (groupResult == SQLITE_DONE) {
        return true;
    }
    if (groupResult != SQLITE_ROW) {
        error = lastError(db_);
        return false;
    }

    const auto* rawName = sqlite3_column_text(groupStatement.get(), 0);
    const auto* rawAdmin = sqlite3_column_text(groupStatement.get(), 1);
    groupName = rawName ? reinterpret_cast<const char*>(rawName) : "";
    adminUsername = rawAdmin ? reinterpret_cast<const char*>(rawAdmin) : "";

    Statement memberStatement(db_,
                              "SELECT username FROM group_members WHERE group_id = ? ORDER BY username ASC;",
                              error);
    if (!memberStatement) {
        return false;
    }
    sqlite3_bind_int64(memberStatement.get(), 1, groupId);
    while (true) {
        const int result = sqlite3_step(memberStatement.get());
        if (result == SQLITE_DONE) {
            return true;
        }
        if (result != SQLITE_ROW) {
            error = lastError(db_);
            return false;
        }
        const auto* rawUser = sqlite3_column_text(memberStatement.get(), 0);
        memberUsernames.push_back(rawUser ? reinterpret_cast<const char*>(rawUser) : "");
    }
}

bool MessageStore::isGroupMemberLocked(long long groupId,
                                       const std::string& username,
                                       bool& isMember,
                                       std::string& error) {
    isMember = false;
    Statement statement(db_,
                        "SELECT 1 FROM group_members WHERE group_id = ? AND username = ? LIMIT 1;",
                        error);
    if (!statement) {
        return false;
    }
    sqlite3_bind_int64(statement.get(), 1, groupId);
    sqlite3_bind_text(statement.get(), 2, username.c_str(), -1, SQLITE_TRANSIENT);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_ROW) {
        isMember = true;
        return true;
    }
    if (result == SQLITE_DONE) {
        return true;
    }
    error = lastError(db_);
    return false;
}

bool MessageStore::resolveChatIdLocked(const std::string& user,
                                       const std::string& chatId,
                                       bool& isGroup,
                                       long long& numericId,
                                       std::string& error) {
    isGroup = false;
    numericId = 0;
    long long groupId = 0;
    if (parseGroupChatId(chatId, groupId)) {
        bool isMember = false;
        if (!isGroupMemberLocked(groupId, user, isMember, error)) {
            return false;
        }
        if (!isMember) {
            error = "group not found";
            return false;
        }
        isGroup = true;
        numericId = groupId;
        return true;
    }

    if (!findConversationLocked(user, chatId, numericId, error)) {
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
