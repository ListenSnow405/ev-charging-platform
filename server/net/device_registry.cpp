#include "device_registry.h"

#include <QJsonObject>

#include "conn_ctx.h"
#include "frame.h"
#include "protocol.h"

namespace ecp {

DeviceRegistry &DeviceRegistry::instance()
{
    static DeviceRegistry d;
    return d;
}

void DeviceRegistry::registerDevice(const QString &pileCode, std::shared_ptr<ConnectionCtx> ctx)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_map.find(pileCode);
    if (it == m_map.end()) {
        m_map.insert(pileCode, Entry{ std::move(ctx), DeviceReport{} });
    } else {
        it->conn = std::move(ctx);   // 重连：换连接，保留旧电量数据
    }
}

void DeviceRegistry::unregisterDevice(const QString &pileCode, const ConnectionCtx *ctx)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_map.constFind(pileCode);
    if (it != m_map.constEnd() && it->conn.get() == ctx)
        m_map.erase(it);
}

std::shared_ptr<ConnectionCtx> DeviceRegistry::find(const QString &pileCode) const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_map.constFind(pileCode);
    return it == m_map.constEnd() ? std::shared_ptr<ConnectionCtx>() : it->conn;
}

bool DeviceRegistry::isOnline(const QString &pileCode) const
{
    return find(pileCode) != nullptr;
}

void DeviceRegistry::updateReport(const QString &pileCode, const DeviceReport &report)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_map.find(pileCode);
    if (it != m_map.end()) it->report = report;
}

bool DeviceRegistry::lastReport(const QString &pileCode, DeviceReport &out) const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_map.constFind(pileCode);
    if (it == m_map.constEnd()) return false;
    out = it->report;
    return true;
}

bool pushToDevice(const QString &pileCode, const QByteArray &frame)
{
    std::shared_ptr<ConnectionCtx> ctx = DeviceRegistry::instance().find(pileCode);
    if (!ctx) return false;
    return ctx->enqueueSend(frame);
}

bool pushToDevice(int cmd, const QString &pileCode, const QJsonObject &data)
{
    QJsonObject d = data;
    d["pileCode"] = pileCode;   // 设备推送统一带 pileCode
    return pushToDevice(pileCode, encodeFrame(buildPush(cmd, d)));
}

bool startCharging(const QString &pileCode)
{
    return pushToDevice(CMD_DEV_START, pileCode);
}

bool stopCharging(const QString &pileCode)
{
    return pushToDevice(CMD_DEV_STOP, pileCode);
}

} // namespace ecp
