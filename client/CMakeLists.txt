add_executable(client
        src/ClientConnection.cpp
        src/main.cpp
)

target_include_directories(client PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(client PRIVATE common Threads::Threads)

option(BUILD_QT_CLIENT "Build the Qt Widgets NexTalk client when Qt6 is available" ON)

if(BUILD_QT_CLIENT)
    find_package(Qt6 QUIET COMPONENTS Widgets)

    if(Qt6_FOUND)
        add_executable(nextalk_gui
                src/ClientConnection.cpp
                src/qt/MainWindow.cpp
                src/qt/main.cpp
        )

        target_include_directories(nextalk_gui PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/qt
        )

        target_link_libraries(nextalk_gui PRIVATE
                common
                Threads::Threads
                Qt6::Widgets
        )
    else()
        message(STATUS "Qt6 Widgets not found; nextalk_gui target will not be built")
    endif()
endif()
