# =============================================================================
#  user-client/user-client.pro  —  充电用户端　归属 L4
#  [说明书] 1.4 模拟手机端交互体验，在 Linux 桌面环境下运行
# =============================================================================
TEMPLATE = app
TARGET   = ecp-user
DESTDIR  = $$PWD/../build/bin

QT      += core gui widgets network
CONFIG  += c++17
CONFIG  -= app_bundle

include(../common/common.pri)

# [说明书] 1.4 一键导航：调用腾讯地图 Web API（QWebEngineView 加载）。
# QtWebEngine 不在 qt6-base-dev 里，需 sudo apt install qt6-webengine-dev。
# 未安装时仍可编译出空壳，导航页会提示如何安装（见 docs/conventions.md 第 5 节）。
qtHaveModule(webenginewidgets) {
    QT      += webenginewidgets
    DEFINES += HAVE_WEBENGINE
} else {
    warning("QtWebEngineWidgets 未安装，一键导航将以占位提示替代。安装：sudo apt install qt6-webengine-dev qt6-webengine-dev-tools")
}

INCLUDEPATH += $$PWD

SOURCES += main.cpp net_client.cpp login_window.cpp main_window.cpp
HEADERS +=          net_client.h   login_window.h   main_window.h
