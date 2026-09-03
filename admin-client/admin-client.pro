# =============================================================================
#  admin-client/admin-client.pro  —  PC 服务器端（管理后台）　归属 L3
#  [说明书] 1.4 宽屏 PC 风格，以表格、图表为主
# =============================================================================
TEMPLATE = app
TARGET   = ecp-admin
DESTDIR  = $$PWD/../build/bin

QT      += core gui widgets network
CONFIG  += c++17
CONFIG  -= app_bundle

include(../common/common.pri)

# [说明书] 1.4 营收趋势折线图使用 QChart 组件。
# QtCharts 不在 qt6-base-dev 里，需 sudo apt install libqt6charts6-dev。
# 未安装时仍可编译出空壳，界面会提示如何安装（见 docs/conventions.md 第 5 节）。
qtHaveModule(charts) {
    QT      += charts
    DEFINES += HAVE_CHARTS
} else {
    warning("QtCharts 未安装，营收趋势图将以占位提示替代。安装：sudo apt install libqt6charts6-dev")
}

INCLUDEPATH += $$PWD

SOURCES += main.cpp net_client.cpp login_window.cpp main_window.cpp overview_page.cpp \
           station_page.cpp add_station_dialog.cpp pile_page.cpp user_page.cpp
HEADERS +=          net_client.h   login_window.h   main_window.h   overview_page.h \
                   station_page.h add_station_dialog.h pile_page.h user_page.h
