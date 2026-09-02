#pragma once
// -----------------------------------------------------------------------------
//  server/net/session.h  —  会话表（token → 身份）
//  归属 L1。 docs/protocol.md 第 6 节：token 32 位十六进制，有效期 7200 秒
//
//  会话表跨线程共享，读多写少 → 用 pthread_rwlock 保护。
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
    void sweepExpired();          // 清理过期会话，可由定时任务调用
    int  count() const;

    void setTtl(qint64 sec) { m_ttl = sec; }

private:
    SessionTable() = default;
    mutable pthread_rwlock_t   m_lock = PTHREAD_RWLOCK_INITIALIZER;
    QHash<QString, SessionInfo> m_map;
    qint64 m_ttl = 7200;          // t_sys_config.token_ttl_sec
};

} // namespace ecp
