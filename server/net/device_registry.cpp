#include "device_registry.h"
#include "conn_ctx.h"

namespace ecp {

DeviceRegistry &DeviceRegistry::instance()
{
    static DeviceRegistry d;
    return d;
}

void DeviceRegistry::registerDevice(const QString &pileCode, std::shared_ptr<ConnectionCtx> ctx)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_map.insert(pileCode, std::move(ctx));
}

void DeviceRegistry::unregisterDevice(const QString &pileCode, const ConnectionCtx *ctx)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_map.constFind(pileCode);
    if (it != m_map.constEnd() && it.value().get() == ctx)
        m_map.erase(it);
}

std::shared_ptr<ConnectionCtx> DeviceRegistry::find(const QString &pileCode) const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_map.constFind(pileCode);
    return it == m_map.constEnd() ? std::shared_ptr<ConnectionCtx>() : it.value();
}

bool pushToDevice(const QString &pileCode, const QByteArray &frame)
{
    std::shared_ptr<ConnectionCtx> ctx = DeviceRegistry::instance().find(pileCode);
    if (!ctx) return false;
    return ctx->enqueueSend(frame);
}

} // namespace ecp
