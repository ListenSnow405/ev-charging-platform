#pragma once
// -----------------------------------------------------------------------------
//  server/net/device_registry.h  —  充电桩设备‑连接映射表
//  归属 L1。
//  本表跨线程共享，由 mutex 保护。注册发生在 9001 上线（业务线程），注销发生在连接关闭（IO 线程）。
//
//  使用说明：
//   - 2112 用 pushToDevice(pileCode, frame)；9001 用 registerDevice(pileCode, req.conn)。
//   - 禁止直接修改内部哈希表，禁止传入无效或已经关闭的ConnectionCtx。
//
//  依赖：conn_ctx.h；内部自带 mutex锁
//  线程：全部对外接口线程安全，任意线程可调用
// -----------------------------------------------------------------------------
#include <memory>
#include <mutex>

#include <QByteArray>
#include <QHash>
#include <QString>

namespace ecp {

class ConnectionCtx;

class DeviceRegistry
{
public:
    static DeviceRegistry &instance();

    // 注册/更新映射。同一 pileCode 重复注册会覆盖旧连接。
    void registerDevice(const QString &pileCode, std::shared_ptr<ConnectionCtx> ctx);

    // 注销：仅当当前映射就是 ctx 时才移除，避免旧连接误删新注册。
    void unregisterDevice(const QString &pileCode, const ConnectionCtx *ctx);

    // 查找在线设备连接。空表示不在线；返回 shared_ptr 保证 ctx 存活。
    std::shared_ptr<ConnectionCtx> find(const QString &pileCode) const;

private:
    DeviceRegistry() = default;
    mutable std::mutex m_mtx;
    QHash<QString, std::shared_ptr<ConnectionCtx>> m_map;
};

// 便捷接口：向在线电桩推送一帧（服务端→设备，如 9003）。离线返回 false。
bool pushToDevice(const QString &pileCode, const QByteArray &frame);

} // namespace ecp
