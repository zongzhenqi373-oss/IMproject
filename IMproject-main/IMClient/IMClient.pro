QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

# client_core 需要 C++17
CONFIG += c++17

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
LIBS += -L$$PROTOBUF_DIR/lib -lprotobuf

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
