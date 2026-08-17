# im_server：跨平台 IM 服务端（asio Reactor + 线程池并发 + SQLite + protobuf）
# 在 VS Code 中通过 CMake Tools 一键构建/调试；命令行：
#   cmake -S . -B build && cmake --build build -j8 && ./build/im_server
cmake_minimum_required(VERSION 3.20)
project(im_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(IM_SERVER_BUILD_TESTS "Build im_server e2e tests" ON)

if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()

# ---- 依赖：协议库（pb）、SQLite、Threads ----
if(NOT TARGET im_protocol)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../protocol ${CMAKE_BINARY_DIR}/protocol)
endif()
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/sqlite /usr/local/opt/sqlite)
endif()
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

set(ASIO_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../client_core/third_party/asio/include)

# ---------------- im_server_lib（业务库，供可执行文件与测试共用） ----------------
add_library(im_server_lib STATIC
    src/Database.cpp
    src/Presence.cpp
    src/Session.cpp
    src/Dispatcher.cpp
    src/Server.cpp
)
target_include_directories(im_server_lib
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${ASIO_INCLUDE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
            ${CMAKE_CURRENT_SOURCE_DIR}/../client_core/include
)
target_compile_definitions(im_server_lib PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(im_server_lib PUBLIC im_protocol SQLite3::SQLite3 Threads::Threads)
if(WIN32)
    target_link_libraries(im_server_lib PUBLIC ws2_32)
endif()

# ---------------- im_server 可执行文件 ----------------
add_executable(im_server src/main.cpp)
target_link_libraries(im_server PRIVATE im_server_lib)

# ---------------- e2e 测试（真服务端 + client_core 真客户端回环） ----------------
if(IM_SERVER_BUILD_TESTS)
    enable_testing()
    set(CLIENT_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(NOT TARGET client_core)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../client_core ${CMAKE_BINARY_DIR}/client_core)
    endif()

    add_executable(test_e2e tests/test_e2e.cpp)
    target_link_libraries(test_e2e PRIVATE im_server_lib client_core)
    target_include_directories(test_e2e PRIVATE ${ASIO_INCLUDE})
    add_test(NAME e2e COMMAND test_e2e)
endif()
