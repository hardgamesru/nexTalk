#pragma once

#include <string>
#include <vector>

namespace common {

    /**
     * Тип прикладной команды в протоколе NexTalk.
     *
     * Клиент и сервер передают по TCP не C++-объекты, а строки. Это enum нужен,
     * чтобы внутри программы работать с понятными типами команд, а не сравнивать
     * строковые литералы по всему коду.
     */
    enum class CommandType {
        Login,
        LoginResult,
        SendMessage,
        IncomingMessage,
        FetchHistory,
        Info,
        Error,
        Quit,
        UploadAttachment,
        AnalyzeAttachment,
        Unknown
    };

    /**
     * Универсальное представление одного сообщения протокола.
     *
     * Пример команды клиента:
     *   type = CommandType::SendMessage
     *   fields = {"bob", "Привет"}
     *
     * На проводе это будет строка:
     *   send_message<TAB>bob<TAB>Привет\n
     */
    struct ProtocolMessage {
        CommandType type{CommandType::Unknown};
        std::vector<std::string> fields;
    };

    /**
     * Преобразует enum команды в строковое имя для отправки по сети.
     */
    std::string commandTypeToString(CommandType type);

    /**
     * Преобразует строковое имя команды из TCP-сообщения в enum.
     */
    CommandType commandTypeFromString(const std::string& value);

    /**
     * Сериализует сообщение в одну строку протокола.
     *
     * Формат:
     *   command<TAB>field1<TAB>field2\n
     *
     * Поля экранируются, поэтому внутри текста сообщения можно безопасно
     * использовать табуляции, переносы строк и обратные слеши.
     */
    std::string serializeMessage(const ProtocolMessage& message);

    /**
     * Разбирает одну строку протокола в ProtocolMessage.
     *
     * Возвращает false, если команда неизвестна или в строке повреждено
     * экранирование.
     */
    bool parseMessage(const std::string& line, ProtocolMessage& message);

}
