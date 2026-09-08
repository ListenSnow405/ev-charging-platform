#pragma once
// -----------------------------------------------------------------------------
//  server/net/session.h  —  token 会话表
//  归属 L1。
//  跨线程共享，读多写少 → 用 pthread_rwlock 保护，可并发调用；。
//
//  使用说明：
//   - 接口自带 rwlock，调用方禁止自行加/解锁。
//   - validate 失败即 token 无效。
//   - 会话依靠token过期、登出、强制下线、sweepExpired来销毁，网络socket断开不会自动销毁会话，客户端断连重连可复用原有token。
//   - sweepExpired() 由外部定时每60s触发，释放过期token。
// -----------------------------------------------------------------------------
#include <pthread.h>
#include <QHash>
#include <QString>

namespace ecp {

enum Role { ROLE_USER = 0, ROLE_ADMIN = 1 };

struct SessionInfo {
    int     id   = 0;        // ROLE_USER → user_id；ROLE_ADMIN → admin_id
    Role    role = ROLE_USER;
    qint64  lastActive = 0;  // Unix 秒
};

class SessionTable
{
public:
    static SessionTable &instance();

    // 生成 token 并登记。返回 token。
    QString create(int id, Role role);

    // 校验 token 并刷新活跃时间。有效返回 true 并填充 out。
    bool validate(const QString &token, SessionInfo &out);

    void remove(const QString &token);

    // 按角色踢除指定身份的全部会话；返回被清除的数量。
    // 必须按 role 过滤——user_id 与 admin_id 是两套独立自增序列，
    // 同一个 id 值在两种角色下都存在，不过滤会误踢管理员会话。
    int invalidateSessions(int id, Role role);

    void sweepExpired();          // 清理过期会话，可由定时任务调用

    // 清空全部会话，所有 token 立即失效；仅供热重载/管理场景使用，运行中勿调。
    void clearAll();

    void setTtl(qint64 sec);      // 设置 token 有效期（秒）。TODO：待接入 t_sys_config.token_ttl_sec

private:
    SessionTable();
    ~SessionTable();
    mutable pthread_rwlock_t   m_lock;
    QHash<QString, SessionInfo> m_map;
    qint64 m_ttl = 7200;          // t_sys_config.token_ttl_sec
};

} // namespace ecp
