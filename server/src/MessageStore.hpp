#pragma once

#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

/**
 * Одно сообщение, загруженное из SQLite.
 *
 * Структура специально близка к полям протокола: сервер почти без
 * преобразований отправляет StoredMessage клиенту как history_message,
 * incoming_message или send_message_result.
 */
struct StoredMessage {
    // Уникальный id сообщения в общей таблице messages.
    long long id{0};
    // Серверное время создания в текстовом формате.
    std::string createdAt;
    // Автор сообщения. Для системных сообщений группы может быть пустым.
    std::string sender;
    // Получатель: username для личного чата или group:<id> для группы.
    std::string recipient;
    std::string text;
    // Поля ответа: id исходного сообщения и короткий снимок его автора/текста.
    long long replyToMessageId{0};
    std::string replyToSender;
    std::string replyToText;
    // Поля пересылки: ссылка на оригинал и сохраненный текст на момент пересылки.
    long long forwardFromMessageId{0};
    std::string forwardFromSender;
    std::string forwardFromText;
    // Мягкое удаление: запись остается в базе, но клиент показывает заглушку.
    std::string deletedAt;
    std::string deletedBy;
};

/**
 * Короткая карточка диалога для списка чатов.
 */
struct ChatSummary {
    // Для личного чата это username собеседника, для группы строка group:<id>.
    std::string peerId;
    // Отображаемое имя: username/display name или название группы.
    std::string peer;
    std::string lastAt;
    std::string lastSender;
    std::string lastText;
    int unreadCount{0};
    // Эти флаги помогают клиенту выбрать правильные кнопки в UI.
    bool isGroup{false};
    bool canManage{false};
};

/**
 * Результат поиска пользователя для создания личного или группового чата.
 */
struct UserSearchResult {
    std::string id;
    std::string username;
};

/**
 * Участник группы вместе с признаком администратора.
 */
struct GroupMemberInfo {
    std::string username;
    bool isAdmin{false};
};

/**
 * Метаданные группы, которые нужны окну настроек.
 */
struct GroupInfo {
    std::string chatId;
    std::string name;
    std::string adminUsername;
    // true, если requester является администратором этой группы.
    bool canManage{false};
};

/**
 * Публичный профиль пользователя. Пароли и соли здесь намеренно не живут.
 */
struct UserProfile {
    std::string username;
    std::string displayName;
    std::string bio;
    std::string avatarColor;
    std::string createdAt;
    std::string lastSeen;
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
    bool loadPasswordHash(const std::string& username,
                          std::string& passwordHash,
                          std::string& error);
    bool userExists(const std::string& username, bool& exists, std::string& error);
    bool getUserProfile(const std::string& username,
                        UserProfile& profile,
                        std::string& error);
    bool getUserProfiles(const std::vector<std::string>& usernames,
                         std::vector<UserProfile>& profiles,
                         std::string& error);
    bool updateUserProfile(const std::string& username,
                           const std::string& displayName,
                           const std::string& bio,
                           const std::string& avatarColor,
                           std::string& error);
    bool updateUserLastSeen(const std::string& username, std::string& error);

    // Сохраняет личное сообщение. Если replyToMessageId или forwardFromMessageId
    // заполнены, MessageStore проверяет доступность исходного сообщения.
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
    // То же, но дополнительно ограничивает поиск конкретным чатом. Это важно
    // для удаления/ответа, чтобы пользователь не мог сослаться на чужой диалог.
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
    // Пагинация вверх: клиент передает id самого старого загруженного сообщения,
    // а сервер возвращает пачку сообщений строго раньше него.
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
    // scope/scopeTarget позволяют переиспользовать поиск: обычный поиск,
    // добавление в группу и создание группы имеют разные исключения.
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
    // При создании группы creator может сразу назначить другого администратора,
    // если этот пользователь входит в список участников.
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
    // Для группового сообщения возвращаем не только StoredMessage, но и список
    // участников, которым сервер должен отправить incoming_message.
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
                       const std::string& requestedChatId,
                       long long messageId,
                       StoredMessage& storedMessage,
                       std::string& chatId,
                       std::vector<std::string>& audienceUsernames,
                       std::string& error);
    // Удаление личного диалога полностью стирает conversation и связанные
    // сообщения. Группы удаляются отдельным deleteGroup.
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
