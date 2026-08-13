QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

# client_core 需要 C++17
CONFIG += c++17

msvc {
    # 客户端源码与 protobuf 生成物统一按 UTF-8 解析。
    QMAKE_CXXFLAGS += /utf-8
    DEFINES += _HAS_STD_BYTE=0 _WIN32_WINNT=0x0601
}

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

# asio standalone（client_core 网络层）在 Windows 需要 Ws2_32
LIBS += -lWs2_32

# 阶段0：网络/协议/业务逻辑下沉到 client_core（纯 C++），源码直接编入本工程
# 说明：SqliteStorage.cpp 暂未编入（Windows 侧 sqlite3 依赖待接入，存储功能已在 client_core 测试中覆盖）
INCLUDEPATH += \
    ../client_core/include \
    ../client_core/third_party/asio/include \
    ../protocol/generated

DEFINES += ASIO_STANDALONE ASIO_NO_DEPRECATED

# 阶段 P2：协议迁移 protobuf。Windows 侧需安装 protobuf（建议 vcpkg）：
#   vcpkg install protobuf:x64-windows
# 然后把下面 PROTOBUF_DIR 改为你的 vcpkg 安装路径。
# 注意：protobuf >= 22 依赖 abseil，若链接报 absl 相关错误，需在 LIBS 中补充 absl 库。
PROTOBUF_DIR = D:/vcpkg/installed/x64-windows
INCLUDEPATH += $$PROTOBUF_DIR/include

CONFIG(debug, debug|release) {
    LIBS += -L$$PROTOBUF_DIR/debug/lib \
        -llibprotobufd \
        -labseil_dll \
        -lutf8_validity
    VCPKG_RUNTIME_DIR = $$PROTOBUF_DIR/debug/bin
    VCPKG_PROTOBUF_DLL = libprotobufd.dll
    VCPKG_RUNTIME_DEST = $$OUT_PWD/debug
} else {
    LIBS += -L$$PROTOBUF_DIR/lib \
        -llibprotobuf \
        -labseil_dll \
        -lutf8_validity
    VCPKG_RUNTIME_DIR = $$PROTOBUF_DIR/bin
    VCPKG_PROTOBUF_DLL = libprotobuf.dll
    VCPKG_RUNTIME_DEST = $$OUT_PWD/release
}

win32 {
    # Qt Creator 的运行环境未必包含 vcpkg/bin，构建后把直接依赖部署到 exe 旁边。
    QMAKE_POST_LINK += $$QMAKE_COPY /Y $$shell_path($$VCPKG_RUNTIME_DIR/$$VCPKG_PROTOBUF_DLL) $$shell_path($$VCPKG_RUNTIME_DEST) $$escape_expand(\n\t)
    QMAKE_POST_LINK += $$QMAKE_COPY /Y $$shell_path($$VCPKG_RUNTIME_DIR/abseil_dll.dll) $$shell_path($$VCPKG_RUNTIME_DEST) $$escape_expand(\n\t)
}

SOURCES += \
    Frienditem.cpp \
    chatdig.cpp \
    kernal.cpp \
    main.cpp \
    logindia.cpp \
    mainwdiget.cpp \
    ../client_core/src/ClientCore.cpp \
    ../client_core/src/TcpTransport.cpp \
    ../protocol/generated/im.pb.cc

msvc {
    # Qt 5.12 的 qmake 无法对单个 SOURCES 项方便地设置 MSVC 告警选项。
    # 关闭来自 protobuf/abseil 公共头文件的已知告警，项目仍保留其余告警检查。
    QMAKE_CXXFLAGS_WARN_ON += /wd4018 /wd4100 /wd4244 /wd4251 /wd4267
}

HEADERS += \
    Frienditem.h \
    chatdig.h \
    kernal.h \
    logindia.h \
    mainwdiget.h

FORMS += \
    Frienditem.ui \
    chatdig.ui \
    logindia.ui \
    mainwdiget.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    resImages.qrc
