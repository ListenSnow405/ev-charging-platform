# =============================================================================
#  server/server.pro  —  业务服务端
#  归属：net/ = L1   biz/ 与 dao/ = L2
# =============================================================================
TEMPLATE = app
TARGET   = ecp-server
DESTDIR  = $$PWD/../build/bin

QT      += core sql        # [说明书] 1.6 QSQLite；网络用 POSIX socket，不需要 QtNetwork
QT      -= gui
CONFIG  += console c++17
CONFIG  -= app_bundle

include(../common/common.pri)      # 冻结契约：协议 / 错误码 / 帧编解码 / 日志 / 时间

INCLUDEPATH += $$PWD

SOURCES += \
    main.cpp \
    net/thread_pool.cpp \
    net/tcp_server.cpp \
    net/session.cpp \
    net/dispatcher.cpp \
    dao/db.cpp \
    biz/user_service.cpp \
    biz/admin_service.cpp

HEADERS += \
    net/thread_pool.h \
    net/tcp_server.h \
    net/session.h \
    net/dispatcher.h \
    dao/db.h
