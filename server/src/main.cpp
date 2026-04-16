#include <iostream>
#include "common/Protocol.hpp"

int main() {
    std::cout << "NexTalk server started" << std::endl;
    std::cout << "Test command: "
              << common::commandTypeToString(common::CommandType::SendMessage)
              << std::endl;
    return 0;
}