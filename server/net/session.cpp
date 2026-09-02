#include "session.h"
#include <QDateTime>
#include <QRandomGenerator>

namespace ecp {

SessionTable &SessionTable::instance()
{
    static SessionTable s;
    return s;
}

static QString makeToken()
{
    // docs/protocol.md 第 6 节：32 位十六进制
    QString t;
    t.reserve(32);
    for (int i = 0; i < 4; ++i)
        t += QStringLiteral("%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
    return t;
}

QString SessionTable::create(int id, Role role)
{
    const QString token = makeToken();
    SessionInfo info{ id, role, QDateTime::currentSecsSinceEpoch() };

    pthread_rwlock_wrlock(&m_lock);
    m_map.insert(token, info);
    pthread_rwlock_unlock(&m_lock);
    return token;
}

bool SessionTable::validate(const QString &token, SessionInfo &out)
{
    if (token.isEmpty()) return false;
    const qint64 now = QDateTime::currentSecsSinceEpoch();

    pthread_rwlock_wrlock(&m_lock);          // 要刷新 lastActive，故取写锁
    auto it = m_map.find(token);
    if (it == m_map.end()) { pthread_rwlock_unlock(&m_lock); return false; }
    if (now - it->lastActive > m_ttl) {      // 过期
        m_map.erase(it);
        pthread_rwlock_unlock(&m_lock);
        return false;
    }
    it->lastActive = now;
    out = *it;
    pthread_rwlock_unlock(&m_lock);
    return true;
}

void SessionTable::remove(const QString &token)
{
    pthread_rwlock_wrlock(&m_lock);
    m_map.remove(token);
    pthread_rwlock_unlock(&m_lock);
}

void SessionTable::sweepExpired()
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    pthread_rwlock_wrlock(&m_lock);
    for (auto it = m_map.begin(); it != m_map.end(); ) {
        if (now - it->lastActive > m_ttl) it = m_map.erase(it);   // erase 返回下一个
        else                              ++it;                   // QHash 迭代器非随机访问，不能 it+1
    }
    pthread_rwlock_unlock(&m_lock);
}

int SessionTable::count() const
{
    pthread_rwlock_rdlock(&m_lock);
    const int n = m_map.size();
    pthread_rwlock_unlock(&m_lock);
    return n;
}

} // namespace ecp
