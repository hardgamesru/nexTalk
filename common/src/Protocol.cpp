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
        case CommandType::Login: return "login";
        case CommandType::LoginResult: return "login_result";
        case CommandType::SendMessage: return "send_message";
        case CommandType::IncomingMessage: return "incoming_message";
        case CommandType::FetchHistory: return "fetch_history";
        case CommandType::Info: return "info";
        case CommandType::Error: return "error";
        case CommandType::Quit: return "quit";
        case CommandType::UploadAttachment: return "upload_attachment";
        case CommandType::AnalyzeAttachment: return "analyze_attachment";
        default: return "unknown";
        }
    }

    CommandType commandTypeFromString(const std::string& value) {
        if (value == "login") return CommandType::Login;
        if (value == "login_result") return CommandType::LoginResult;
        if (value == "send_message") return CommandType::SendMessage;
        if (value == "incoming_message") return CommandType::IncomingMessage;
        if (value == "fetch_history") return CommandType::FetchHistory;
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
