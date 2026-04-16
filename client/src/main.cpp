#include <iostream>
#include "common/Protocol.hpp"

int main() {
    std::cout << "NexTalk client started" << std::endl;
    std::cout << "Test command: "
              << common::commandTypeToString(common::CommandType::Login)
              << std::endl;
    return 0;
}