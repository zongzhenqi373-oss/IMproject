cmake_minimum_required(VERSION 3.20)
project(client_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CLIENT_CORE_BUILD_TESTS "Build client_core unit tests" ON)

# MSVC 默认按系统代码页解析源文件，强制 UTF-8 以正确编译中文注释/字符串
if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()

# protobuf：协议序列化。
# CONFIG 模式：新版 protobuf(>=22) 依赖 abseil，其 cmake config 会通过 find_dependency 传递 absl 头路径与链接库
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH
        /opt/homebrew/opt/protobuf /usr/local/opt/protobuf
        /opt/homebrew/opt/abseil   /usr/local/opt/abseil)
endif()
find_package(Protobuf CONFIG REQUIRED)

# pb 生成物策略：Windows 的 VS/qmake 直接编译仓库里提交的 protocol/generated/im.pb.*
# （与 Windows 侧 vcpkg 运行时 6.33.x 匹配）；
# 本 CMake 工程改为构建时用本机 protoc 现场生成，保证生成物与本机运行时主版本严格一致
# （protobuf 有跨版本编译期检查 #if PROTOBUF_VERSION，混用会直接 #error）。
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/im.proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
add_custom_command(
    OUTPUT  ${PROTO_GEN_DIR}/im.pb.cc ${PROTO_GEN_DIR}/im.pb.h
    COMMAND protobuf::protoc --cpp_out=${PROTO_GEN_DIR}
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/../protocol im.proto
    DEPENDS ${PROTO_FILE}
    COMMENT "protoc 生成 im.pb.*（本机运行时版本）"
)

# ---------------- client_core 静态库 ----------------
add_library(client_core STATIC
    src/ClientCore.cpp
    src/TcpTransport.cpp
    src/SqliteStorage.cpp
    ${PROTO_GEN_DIR}/im.pb.cc
)

target_include_directories(client_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${PROTO_GEN_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

target_compile_definitions(client_core PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)

find_package(Threads REQUIRED)
target_link_libraries(client_core PUBLIC Threads::Threads protobuf::libprotobuf)

# asio standalone 在 Windows 需要 ws2_32
if(WIN32)
    target_link_libraries(client_core PUBLIC ws2_32)
endif()

# SQLite：优先系统/Homebrew 安装
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/sqlite /usr/local/opt/sqlite)
endif()
find_package(SQLite3 REQUIRED)
target_link_libraries(client_core PUBLIC SQLite::SQLite3)

# ---------------- 单元测试 ----------------
if(CLIENT_CORE_BUILD_TESTS)
    enable_testing()

    add_executable(test_protocol tests/test_protocol.cpp)
    target_link_libraries(test_protocol PRIVATE client_core)
    target_include_directories(test_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
    add_test(NAME protocol COMMAND test_protocol)

    add_executable(test_storage tests/test_storage.cpp)
    target_link_libraries(test_storage PRIVATE client_core)
    add_test(NAME storage COMMAND test_storage)

    # 回环集成测试：自带 asio 假服务端，需要 asio 头文件
    add_executable(test_integration tests/test_integration.cpp)
    target_link_libraries(test_integration PRIVATE client_core)
    target_include_directories(test_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/include)
    target_compile_definitions(test_integration PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
    if(WIN32)
        target_link_libraries(test_integration PRIVATE ws2_32)
    endif()
    add_test(NAME integration COMMAND test_integration)
endif()
