#pragma once

#include <string>

namespace common {

    enum class CommandType {
        Login,
        SendMessage,
        FetchHistory,
        UploadAttachment,
        AnalyzeAttachment,
        Unknown
    };

    std::string commandTypeToString(CommandType type);
    CommandType commandTypeFromString(const std::string& value);

}