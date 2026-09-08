#pragma once
// -----------------------------------------------------------------------------
//  server/net/device_registry.h  —  充电桩设备‑连接映射表
//  归属 L1。
//  本表跨线程共享，由 mutex 保护。注册发生在 9001 上线（业务线程），注销发生在连接关闭（IO 线程）。
//
//  使用说明：
//   - 2112 用 pushToDevice；9001 用 registerDevice；1203/1204 用 startCharging/stopCharging。
//   - 1204 结算取电量用 lastReport(pileCode) 拿整数 kwhX100；判在线用 isOnline(pileCode)。
//   - 禁止直接修改内部哈希表，禁止传入无效或已经关闭的 ConnectionCtx。
//
//  依赖：conn_ctx.h；内部自带 mutex 锁
//  线程：全部对外接口线程安全，任意线程可调用
// -----------------------------------------------------------------------------
#include <memory>
#include <mutex>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>

namespace ecp {

class ConnectionCtx;

// 电桩最近一次 9002 上报的数据。无上报时 status=闲置、kwhX100=0。
struct DeviceReport {
    int    status  = 1;      // PILE_IDLE（见 protocol.h 的 PileStatus）
    qint64 kwhX100 = 0;      // 本次订单累计电量 ×100，单位 0.01 kWh
    double power   = 0.0;    // 当前功率 kW
};

class DeviceRegistry
{
public:
    static DeviceRegistry &instance();

    // 注册/更新映射。同一 pileCode 重复注册会覆盖旧连接（保留旧电量数据）。
    void registerDevice(const QString &pileCode, std::shared_ptr<ConnectionCtx> ctx);

    // 注销：仅当当前映射就是 ctx 时才移除，避免旧连接误删新注册。
    void unregisterDevice(const QString &pileCode, const ConnectionCtx *ctx);

    // 查找在线设备连接。空表示不在线；返回 shared_ptr 保证 ctx 存活。
    std::shared_ptr<ConnectionCtx> find(const QString &pileCode) const;

    // 电桩是否在线（已 9001 注册且连接未断开）。线程安全。
    bool isOnline(const QString &pileCode) const;

    // 更新电桩最近一次上报（9002 调用）。未注册时忽略。
    void updateReport(const QString &pileCode, const DeviceReport &report);

    // 取电桩最近一次上报；离线/无数据返回 false。
    bool lastReport(const QString &pileCode, DeviceReport &out) const;

private:
    DeviceRegistry() = default;

    struct Entry {
        std::shared_ptr<ConnectionCtx> conn;
        DeviceReport                   report;
    };

    mutable std::mutex m_mtx;
    QHash<QString, Entry> m_map;
};

// 便捷接口：向在线电桩推送一帧（服务端→设备）。离线返回 false。
bool pushToDevice(const QString &pileCode, const QByteArray &frame);

// 便捷重载：直接传命令字 + 桩号 + data，内部 encodeFrame(buildPush(...))。
bool pushToDevice(int cmd, const QString &pileCode, const QJsonObject &data = QJsonObject());

// 语义化接口：下发「开始充电」(9005) / 「结束充电」(9006)。离线返回 false。
bool startCharging(const QString &pileCode);
bool stopCharging(const QString &pileCode);

} // namespace ecp
