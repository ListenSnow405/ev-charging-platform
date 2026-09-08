#include "user_registry.h"
#include "conn_ctx.h"

namespace ecp {

UserRegistry &UserRegistry::instance()
{
    static UserRegistry u;
    return u;
}

void UserRegistry::registerUser(int userId, std::shared_ptr<ConnectionCtx> ctx)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_map.insert(userId, std::move(ctx));
}

void UserRegistry::unregisterUser(int userId, const ConnectionCtx *ctx)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_map.constFind(userId);
    if (it != m_map.constEnd() && it.value().get() == ctx)
        m_map.erase(it);
}

std::shared_ptr<ConnectionCtx> UserRegistry::find(int userId) const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_map.constFind(userId);
    return it == m_map.constEnd() ? std::shared_ptr<ConnectionCtx>() : it.value();
}

bool pushToUser(int userId, const QByteArray &frame)
{
    std::shared_ptr<ConnectionCtx> ctx = UserRegistry::instance().find(userId);
    if (!ctx) return false;
    return ctx->enqueueSend(frame);
}

} // namespace ecp
