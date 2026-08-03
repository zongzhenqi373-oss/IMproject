QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

#DEFINES += NOMINMAX _HAS_STD_BYTE=0

LIBS += -lWs2_32

SOURCES += \
    Frienditem.cpp \
    chatdig.cpp \
    kernal.cpp \
    main.cpp \
    logindia.cpp \
    mainwdiget.cpp \
    mediator/TCPClientmediator.cpp \
    mediator/TCPServermediator.cpp \
    mediator/UDPmediator.cpp \
    net/TCPClient.cpp \
    net/TCPServer.cpp \
    net/UDP.cpp

HEADERS += \
    Frienditem.h \
    chatdig.h \
    kernal.h \
    logindia.h \
    mainwdiget.h \
    mediator/INetmediator.h \
    mediator/TCPClientmediator.h \
    mediator/TCPServermediator.h \
    mediator/UDPmediator.h \
    net/INet.h \
    net/TCPClient.h \
    net/TCPServer.h \
    net/UDP.h \
    net/def.h

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
