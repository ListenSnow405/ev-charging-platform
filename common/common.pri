# =============================================================================
#  common/common.pri  —  各子工程统一引入共享基座
#
#  冻结契约 · 属主 L1，其他人只读。
#  用法：在 server / admin-client / user-client / tools 的 .pro 中加入
#      include(../common/common.pri)
# =============================================================================

QT      += core          # [本组自定] JSON 用 QJsonDocument，不引入第三方库
CONFIG  += c++17         # [本组自定] C++17
CONFIG  -= app_bundle

INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/protocol.h \
    $$PWD/error_code.h \
    $$PWD/frame.h \
    $$PWD/logger.h \
    $$PWD/time_util.h \
    $$PWD/app_path.h

# [说明书] 1.6 多线程 pthread —— 服务端与电桩模拟器需显式链接 pthread
unix: LIBS += -lpthread
