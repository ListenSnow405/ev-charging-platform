#include "thread_pool.h"
#include "logger.h"

namespace ecp {

ThreadPool::~ThreadPool() { stop(); }

void *ThreadPool::entry(void *arg)
{
    static_cast<ThreadPool *>(arg)->loop();
    return nullptr;
}

bool ThreadPool::start(int n)
{
    if (n <= 0) return false;
    pthread_mutex_lock(&m_mtx);
    m_running = true;
    pthread_mutex_unlock(&m_mtx);

    for (int i = 0; i < n; ++i) {
        pthread_t tid;
        if (pthread_create(&tid, nullptr, &ThreadPool::entry, this) != 0) {
            LOG_E(QStringLiteral("pthread_create 失败，已创建 %1 个线程").arg(m_threads.size()));
            stop();
            return false;
        }
        m_threads.push_back(tid);
    }
    LOG_I(QStringLiteral("线程池启动，工作线程数 %1").arg(n));
    return true;
}

bool ThreadPool::post(Task task)
{
    pthread_mutex_lock(&m_mtx);
    if (!m_running) { pthread_mutex_unlock(&m_mtx); return false; }
    m_tasks.push(std::move(task));
    pthread_cond_signal(&m_cv);          // 唤醒一个空闲线程
    pthread_mutex_unlock(&m_mtx);
    return true;
}

void ThreadPool::loop()
{
    for (;;) {
        pthread_mutex_lock(&m_mtx);
        // 必须用 while 而非 if：防止虚假唤醒（spurious wakeup）
        while (m_running && m_tasks.empty())
            pthread_cond_wait(&m_cv, &m_mtx);

        if (!m_running && m_tasks.empty()) {     // 停止且已清空 → 退出
            pthread_mutex_unlock(&m_mtx);
            return;
        }
        Task task = std::move(m_tasks.front());
        m_tasks.pop();
        pthread_mutex_unlock(&m_mtx);            // 执行任务时不持锁

        task();
    }
}

void ThreadPool::stop()
{
    pthread_mutex_lock(&m_mtx);
    if (!m_running) { pthread_mutex_unlock(&m_mtx); return; }
    m_running = false;
    pthread_cond_broadcast(&m_cv);               // 唤醒全部，让它们看到 running=false
    pthread_mutex_unlock(&m_mtx);

    for (pthread_t tid : m_threads)
        pthread_join(tid, nullptr);
    m_threads.clear();
    LOG_I(QStringLiteral("线程池已停止"));
}

} // namespace ecp
