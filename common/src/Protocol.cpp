#include "common/Protocol.hpp"

namespace common {
    namespace {
        // TCP передает поток байтов, а наш протокол разделяет поля символом TAB.
        // Поэтому спецсимволы внутри пользовательского текста нужно экранировать:
        // иначе сообщение "hello<TAB>world" выглядело бы как два разных поля.
        std::string escapeField(const std::string& value) {
            std::string escaped;
            escaped.reserve(value.size());

            for (char ch : value) {
                switch (ch) {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                default:
                    escaped += ch;
                    break;
                }
            }

            return escaped;
        }

        // Обратная операция к escapeField: превращает последовательности \t, \n,
        // \r и \\ обратно в реальные символы внутри поля протокола.
        bool unescapeField(const std::string& value, std::string& output) {
            output.clear();
            output.reserve(value.size());

            for (std::size_t i = 0; i < value.size(); ++i) {
                if (value[i] != '\\') {
                    output += value[i];
                    continue;
                }

                if (i + 1 >= value.size()) {
                    return false;
                }

                const char marker = value[++i];
                switch (marker) {
                case '\\':
                    output += '\\';
                    break;
                case 't':
                    output += '\t';
                    break;
                case 'n':
                    output += '\n';
                    break;
                case 'r':
                    output += '\r';
                    break;
                default:
                    return false;
                }
            }

            return true;
        }
    }

    std::string commandTypeToString(CommandType type) {
        // Эти строковые значения являются частью сетевого протокола. Если их
        // изменить, старые клиенты и серверы перестанут понимать друг друга.
        switch (type) {
        case CommandType::Register: return "register";
        case CommandType::RegisterResult: return "register_result";
        case CommandType::Login: return "login";
        case CommandType::LoginResult: return "login_result";
        case CommandType::SendMessage: return "send_message";
        case CommandType::IncomingMessage: return "incoming_message";
        case CommandType::FetchHistory: return "fetch_history";
        case CommandType::HistoryMessage: return "history_message";
        case CommandType::HistoryResult: return "history_result";
        case CommandType::FetchChats: return "fetch_chats";
        case CommandType::ChatItem: return "chat_item";
        case CommandType::ChatListResult: return "chat_list_result";
        case CommandType::SearchUsers: return "search_users";
        case CommandType::UserSearchItem: return "user_search_item";
        case CommandType::UserSearchResult: return "user_search_result";
        case CommandType::CreateChat: return "create_chat";
        case CommandType::CreateChatResult: return "create_chat_result";
        case CommandType::DeleteChat: return "delete_chat";
        case CommandType::DeleteChatResult: return "delete_chat_result";
        case CommandType::MarkRead: return "mark_read";
        case CommandType::MarkReadResult: return "mark_read_result";
        case CommandType::Info: return "info";
        case CommandType::Error: return "error";
        case CommandType::Quit: return "quit";
        case CommandType::UploadAttachment: return "upload_attachment";
        case CommandType::AnalyzeAttachment: return "analyze_attachment";
        default: return "unknown";
        }
    }

    CommandType commandTypeFromString(const std::string& value) {
        if (value == "register") return CommandType::Register;
        if (value == "register_result") return CommandType::RegisterResult;
        if (value == "login") return CommandType::Login;
        if (value == "login_result") return CommandType::LoginResult;
        if (value == "send_message") return CommandType::SendMessage;
        if (value == "incoming_message") return CommandType::IncomingMessage;
        if (value == "fetch_history") return CommandType::FetchHistory;
        if (value == "history_message") return CommandType::HistoryMessage;
        if (value == "history_result") return CommandType::HistoryResult;
        if (value == "fetch_chats") return CommandType::FetchChats;
        if (value == "chat_item") return CommandType::ChatItem;
        if (value == "chat_list_result") return CommandType::ChatListResult;
        if (value == "search_users") return CommandType::SearchUsers;
        if (value == "user_search_item") return CommandType::UserSearchItem;
        if (value == "user_search_result") return CommandType::UserSearchResult;
        if (value == "create_chat") return CommandType::CreateChat;
        if (value == "create_chat_result") return CommandType::CreateChatResult;
        if (value == "delete_chat") return CommandType::DeleteChat;
        if (value == "delete_chat_result") return CommandType::DeleteChatResult;
        if (value == "mark_read") return CommandType::MarkRead;
        if (value == "mark_read_result") return CommandType::MarkReadResult;
        if (value == "info") return CommandType::Info;
        if (value == "error") return CommandType::Error;
        if (value == "quit") return CommandType::Quit;
        if (value == "upload_attachment") return CommandType::UploadAttachment;
        if (value == "analyze_attachment") return CommandType::AnalyzeAttachment;
        return CommandType::Unknown;
    }

    std::string serializeMessage(const ProtocolMessage& message) {
        std::string line = commandTypeToString(message.type);

        // Каждое поле отделяется табуляцией. В конце обязательно добавляем '\n',
        // чтобы принимающая сторона могла читать поток TCP построчно.
        for (const auto& field : message.fields) {
            line += '\t';
            line += escapeField(field);
        }

        line += '\n';
        return line;
    }

    bool parseMessage(const std::string& line, ProtocolMessage& message) {
        message = ProtocolMessage{};

        // readLine оставляет перевод строки в конце. Убираем \n и \r, чтобы
        // дальше работать только с содержимым протокольного сообщения.
        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
            trimmed.pop_back();
        }

        if (trimmed.empty()) {
            return false;
        }

        std::vector<std::string> parts;
        std::size_t start = 0;

        // Разбиваем строку по TAB. Табуляции внутри пользовательского текста
        // к этому моменту находятся в экранированном виде "\\t" и не мешают.
        while (start <= trimmed.size()) {
            const std::size_t separator = trimmed.find('\t', start);
            if (separator == std::string::npos) {
                parts.push_back(trimmed.substr(start));
                break;
            }

            parts.push_back(trimmed.substr(start, separator - start));
            start = separator + 1;
        }

        message.type = commandTypeFromString(parts.front());
        for (std::size_t i = 1; i < parts.size(); ++i) {
            std::string field;
            // Если поле заканчивается одиночным '\' или содержит неизвестную
            // escape-последовательность, считаем все сообщение поврежденным.
            if (!unescapeField(parts[i], field)) {
                message = ProtocolMessage{};
                return false;
            }

            message.fields.push_back(field);
        }

        return message.type != CommandType::Unknown;
    }

}
