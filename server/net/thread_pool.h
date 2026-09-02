#pragma once
// -----------------------------------------------------------------------------
//  server/net/thread_pool.h  —  pthread 线程池
//  归属 L1。 [说明书] 1.6 「程序的主框架应该是一个多线程结构」「多线程(pthread)编程」
//
//  ⚠ 这是说明书点名的考核点，禁止改用 QThread 替代（CLAUDE.md 技术基线）。
// -----------------------------------------------------------------------------
#include <pthread.h>
#include <functional>
#include <queue>
#include <vector>

namespace ecp {

class ThreadPool
{
public:
    using Task = std::function<void()>;

    ~ThreadPool();

    // 启动 n 个工作线程。n 取自 t_sys_config.thread_pool_size，默认 8。
    bool start(int n);

    // 投递任务。线程池已停止时直接丢弃并返回 false。
    bool post(Task task);

    // 通知所有线程退出并 join。可重复调用。
    void stop();

    int size() const { return static_cast<int>(m_threads.size()); }

private:
    static void *entry(void *arg);   // pthread 入口，必须是 static
    void loop();                     // 工作线程主循环

    std::vector<pthread_t> m_threads;
    std::queue<Task>       m_tasks;

    // [说明书] 1.6 多线程：任务队列由 mutex + cond 保护
    pthread_mutex_t m_mtx = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  m_cv  = PTHREAD_COND_INITIALIZER;
    bool            m_running = false;
};

} // namespace ecp
