#include "common/Protocol.hpp"

namespace common {

    std::string commandTypeToString(CommandType type) {
        switch (type) {
        case CommandType::Login: return "login";
        case CommandType::SendMessage: return "send_message";
        case CommandType::FetchHistory: return "fetch_history";
        case CommandType::UploadAttachment: return "upload_attachment";
        case CommandType::AnalyzeAttachment: return "analyze_attachment";
        default: return "unknown";
        }
    }

    CommandType commandTypeFromString(const std::string& value) {
        if (value == "login") return CommandType::Login;
        if (value == "send_message") return CommandType::SendMessage;
        if (value == "fetch_history") return CommandType::FetchHistory;
        if (value == "upload_attachment") return CommandType::UploadAttachment;
        if (value == "analyze_attachment") return CommandType::AnalyzeAttachment;
        return CommandType::Unknown;
    }

}