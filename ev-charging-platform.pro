# =============================================================================
#  ev-charging-platform.pro  —  顶层工程（一键构建全部 C++ 子工程）
#
#  用法：mkdir -p build && cd build && qmake6 .. && make -j$(nproc)
#        或直接跑 bash scripts/build-all.sh
#
#  dataviz/（Web 大屏）与 ml/（Python）不参与 qmake 构建。
# =============================================================================
TEMPLATE = subdirs
CONFIG  += ordered

SUBDIRS = \
    server \
    admin-client \
    user-client \
    tools/pile-simulator

server.file            = server/server.pro
admin-client.file      = admin-client/admin-client.pro
user-client.file       = user-client/user-client.pro
tools/pile-simulator.file = tools/pile-simulator/pile-simulator.pro
