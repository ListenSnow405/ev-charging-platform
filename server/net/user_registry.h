#pragma once
// -----------------------------------------------------------------------------
//  server/net/user_registry.h  —  用户连接映射表
//  归属 L1。userId → 连接 的映射，供服务端向用户主动推送（1208）。
//  登记发生在鉴权成功（dispatcher），注销发生在连接关闭（IO 线程）。
//
//  使用说明：业务层（L2）用 pushToUser(userId, frame) 向在线用户推送；
//  一个 userId 只保留一条连接（后登记的覆盖旧的）。
//
//  依赖：conn_ctx.h；内部自带 mutex 锁
//  线程：全部对外接口线程安全，任意线程可调用
// -----------------------------------------------------------------------------
#include <memory>
#include <mutex>

#include <QByteArray>
#include <QHash>

namespace ecp {

class ConnectionCtx;

class UserRegistry
{
public:
    static UserRegistry &instance();

    // 登记用户连接。同一 userId 重复登记覆盖旧连接（一个用户一条连接）。
    void registerUser(int userId, std::shared_ptr<ConnectionCtx> ctx);

    // 注销：仅当当前映射就是 ctx 时才移除，避免旧连接误删新注册。
    void unregisterUser(int userId, const ConnectionCtx *ctx);

    // 查找用户连接。空表示不在线。
    std::shared_ptr<ConnectionCtx> find(int userId) const;

private:
    UserRegistry() = default;
    mutable std::mutex m_mtx;
    QHash<int, std::shared_ptr<ConnectionCtx>> m_map;
};

// 向指定用户推送一帧（如 1208）。离线/无连接返回 false。
bool pushToUser(int userId, const QByteArray &frame);

} // namespace ecp
