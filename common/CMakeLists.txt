add_library(common STATIC
        src/Protocol.cpp
)

target_include_directories(common PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)