#pragma once

#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

/**
 * One persisted chat message loaded from SQLite.
 */
struct StoredMessage {
    long long id{0};
    std::string createdAt;
    std::string sender;
    std::string recipient;
    std::string text;
    long long replyToMessageId{0};
    std::string replyToSender;
    std::string replyToText;
    long long forwardFromMessageId{0};
    std::string forwardFromSender;
    std::string forwardFromText;
    std::string deletedAt;
    std::string deletedBy;
};

struct ChatSummary {
    std::string peerId;
    std::string peer;
    std::string lastAt;
    std::string lastSender;
    std::string lastText;
    int unreadCount{0};
    bool isGroup{false};
    bool canManage{false};
};

struct UserSearchResult {
    std::string id;
    std::string username;
};

struct GroupMemberInfo {
    std::string username;
    bool isAdmin{false};
};

struct GroupInfo {
    std::string chatId;
    std::string name;
    std::string adminUsername;
    bool canManage{false};
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
                     long long replyToMessageId,
                     long long forwardFromMessageId,
                     const std::string& forwardFromSenderOverride,
                     const std::string& forwardFromTextOverride,
                     StoredMessage& storedMessage,
                     std::string& error);
    bool loadAccessibleMessage(const std::string& username,
                               long long messageId,
                               StoredMessage& message,
                               std::string& error);
    bool loadAccessibleMessageInChat(const std::string& username,
                                     const std::string& chatId,
                                     long long messageId,
                                     StoredMessage& message,
                                     std::string& error);
    bool fetchHistory(const std::string& user,
                      const std::string& peer,
                      int limit,
                      std::vector<StoredMessage>& messages,
                      std::string& error);
    bool fetchHistoryBefore(const std::string& user,
                            const std::string& peer,
                            long long beforeMessageId,
                            int limit,
                            std::vector<StoredMessage>& messages,
                            std::string& error);
    bool markConversationRead(const std::string& user,
                              const std::string& peer,
                              std::string& error);
    bool fetchChats(const std::string& user,
                    std::vector<ChatSummary>& chats,
                    std::string& error);
    bool searchUsers(const std::string& query,
                     const std::string& excludeUser,
                     const std::string& scope,
                     const std::string& scopeTarget,
                     int limit,
                     std::vector<UserSearchResult>& users,
                     std::string& error);
    bool createConversation(const std::string& user,
                            long long peerId,
                            std::string& peerUsername,
                            std::string& error);
    bool createGroup(const std::string& creator,
                     const std::string& name,
                     const std::string& adminUsername,
                     const std::vector<std::string>& members,
                     long long& groupId,
                     std::string& error);
    bool fetchGroupInfo(const std::string& requester,
                        long long groupId,
                        GroupInfo& info,
                        std::vector<GroupMemberInfo>& members,
                        std::string& error);
    bool addGroupMembers(const std::string& requester,
                         long long groupId,
                         const std::vector<std::string>& usernames,
                         std::vector<std::string>& addedUsers,
                         std::string& error);
    bool removeGroupMember(const std::string& requester,
                           long long groupId,
                           const std::string& username,
                           std::string& error);
    bool transferGroupAdmin(const std::string& requester,
                            long long groupId,
                            const std::string& newAdminUsername,
                            std::string& error);
    bool leaveGroup(const std::string& requester,
                    long long groupId,
                    std::string& error);
    bool deleteGroup(const std::string& requester,
                     long long groupId,
                     std::vector<std::string>& removedUsers,
                     std::string& error);
    bool saveGroupMessage(const std::string& sender,
                          long long groupId,
                          const std::string& text,
                          long long replyToMessageId,
                          long long forwardFromMessageId,
                          const std::string& forwardFromSenderOverride,
                          const std::string& forwardFromTextOverride,
                          StoredMessage& storedMessage,
                          std::string& groupName,
                          std::vector<std::string>& memberUsernames,
                          std::string& error);
    bool saveGroupSystemMessage(long long groupId,
                                const std::string& text,
                                StoredMessage& storedMessage,
                                std::string& groupName,
                                std::vector<std::string>& memberUsernames,
                                std::string& error);
    bool deleteMessage(const std::string& requester,
                       long long messageId,
                       StoredMessage& storedMessage,
                       std::string& chatId,
                       std::vector<std::string>& audienceUsernames,
                       std::string& error);
    bool deleteConversation(const std::string& user,
                            const std::string& peer,
                            std::string& error);

private:
    bool executeLocked(const char* sql, std::string& error);
    bool markConversationReadLocked(long long conversationId,
                                    const std::string& username,
                                    std::string& error);
    bool ensureSchemaLocked(std::string& error);
    bool ensureColumnLocked(const std::string& table,
                            const std::string& column,
                            const std::string& definition,
                            std::string& error);
    bool userExistsLocked(const std::string& username, bool& exists, std::string& error);
    bool loadGroupContextLocked(long long groupId,
                                std::string& groupName,
                                std::string& adminUsername,
                                std::vector<std::string>& memberUsernames,
                                std::string& error);
    bool isGroupMemberLocked(long long groupId,
                             const std::string& username,
                             bool& isMember,
                             std::string& error);
    bool resolveChatIdLocked(const std::string& user,
                             const std::string& chatId,
                             bool& isGroup,
                             long long& numericId,
                             std::string& error);
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
