add_executable(server
        src/MessageStore.cpp
        src/MessengerServer.cpp
        src/main.cpp
)

target_link_libraries(server PRIVATE common Threads::Threads sqlite3 OpenSSL::SSL OpenSSL::Crypto)
