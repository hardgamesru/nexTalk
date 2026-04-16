add_executable(server
        src/MessengerServer.cpp
        src/main.cpp
)

target_link_libraries(server PRIVATE common Threads::Threads)
