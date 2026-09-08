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

DEFINES += _GNU_SOURCE        # epoll/eventfd 需要（io_wake.cpp / epoll_loop.cpp）

include(../common/common.pri)      # 冻结契约：协议 / 错误码 / 帧编解码 / 日志 / 时间

INCLUDEPATH += $$PWD

SOURCES += \
    main.cpp \
    net/thread_pool.cpp \
    net/tcp_server.cpp \
    net/session.cpp \
    net/dispatcher.cpp \
    net/conn_ctx.cpp \
    net/io_wake.cpp \
    net/device_registry.cpp \
    net/epoll_loop.cpp \
    dao/db.cpp \
    biz/user_service.cpp \
    biz/admin_service.cpp \
    biz/wallet_service.cpp \
    biz/user_management_service.cpp \
    biz/station_service.cpp \
    biz/pile_service.cpp \
    biz/reservation_service.cpp \
    biz/order_service.cpp \
    biz/statistics_service.cpp \
    biz/ext_08_carbon_calc.cpp \
    biz/ext_08_carbon_service.cpp \
    biz/order_flow_service.cpp

HEADERS += \
    ../common/protocol_ext.h \
    ../common/error_code_ext.h \
    net/thread_pool.h \
    net/tcp_server.h \
    net/session.h \
    net/dispatcher.h \
    net/conn_ctx.h \
    net/io_wake.h \
    net/device_registry.h \
    net/epoll_loop.h \
    dao/db.h \
    biz/ext_08_carbon_calc.h
