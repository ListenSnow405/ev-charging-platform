# tools/pile-simulator/pile-simulator.pro  —  电桩模拟器　归属 L1
TEMPLATE = app
TARGET   = ecp-pile-sim
DESTDIR  = $$PWD/../../build/bin

QT      += core network
QT      -= gui
CONFIG  += console c++17
CONFIG  -= app_bundle

include(../../common/common.pri)

SOURCES += main.cpp
