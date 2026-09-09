#pragma once
// =============================================================================
//  user-client/webengine_env.h  —  QtWebEngine 运行期可用性探测　归属 L4
//
//  [说明书] 1.4 一键导航用 QWebEngineView 加载腾讯地图。
//  WebEngine 除了动态库，还需要辅助进程可执行文件 QtWebEngineProcess
//  （Ubuntu 22.04 由 libqt6webenginecore6-bin 提供，可以单独缺失）。
//  找不到它时 WebEngine 内部直接 qFatal 中止进程，无法 try/catch 兜住，
//  所以必须在 new QWebEngineView 之前先探测，缺失就退回占位页。
// =============================================================================

namespace ecp {

// 探测 QtWebEngineProcess：找到则写入 QTWEBENGINEPROCESS_PATH 并返回 true。
// 结果首次调用时缓存，可在任意位置重复调用。
bool webEngineAvailable();

} // namespace ecp
