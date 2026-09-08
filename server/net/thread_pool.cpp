#include "thread_pool.h"
#include "logger.h"

namespace ecp {

ThreadPool::ThreadPool()
{
    pthread_mutex_init(&m_mtx, nullptr);
    pthread_cond_init(&m_cv, nullptr);
}

ThreadPool::~ThreadPool()
{
    stop();
    pthread_mutex_destroy(&m_mtx);
    pthread_cond_destroy(&m_cv);
}

void *ThreadPool::entry(void *arg)
{
    static_cast<ThreadPool *>(arg)->loop();
    return nullptr;
}

bool ThreadPool::start(int n)
{
    if (n <= 0) return false;
    pthread_mutex_lock(&m_mtx);
    if (m_running) {   // 已启动，禁止重复 start
        const size_t count = m_threads.size();
        pthread_mutex_unlock(&m_mtx);
        LOG_E(QStringLiteral("线程池已经启动，请勿重复启动，当前工作线程数 %1").arg(count));
        return false;
    }
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

        try {
            task();
        } catch (...) {
            // C++ 异常无法穿越 pthread 入口（entry 是 C 函数指针），
            // 不捕获会触发 std::terminate 杀死整个进程，故在此兜底吞掉。
            LOG_E(QStringLiteral("线程池任务抛出异常，已忽略，线程继续运行"));
        }
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
