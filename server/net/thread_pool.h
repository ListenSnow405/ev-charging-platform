#pragma once
// -----------------------------------------------------------------------------
//  server/net/thread_pool.h  —  pthread 线程池
//  归属 L1。
//  实现多线程结构，创建默认 8 个工作线程。

//  使用说明：
//   - 禁止并发调用 start() 和 stop()。
//   - 禁止在工作任务 Task 内部调用 stop()。
//   - Task运行在原生 pthread 允许使用 QString/QByteArray/QJson 等值类型，禁止直接操作 QObject 子类实例（信号槽对象）。
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

    ThreadPool();   // 构造：pthread_mutex_init / pthread_cond_init
    ~ThreadPool();  // 析构：stop() 后 pthread_mutex_destroy / pthread_cond_destroy

    // 禁止拷贝：析构要 destroy 锁，拷贝会双重 destroy
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // 启动 n 个工作线程。n 取自 t_sys_config.thread_pool_size，默认 8。
    // 禁止重复启动：已启动时返回 false。
    bool start(int n);

    // 投递任务。线程池已停止时直接丢弃并返回 false。
    bool post(Task task);

    // 停止线程池：不再接受新任务，排空队列中已入队的任务后，等待全部线程退出并 join。
    // 可重复调用。
    void stop();

private:
    static void *entry(void *arg);   // pthread 入口，必须是 static
    void loop();                     // 工作线程主循环

    std::vector<pthread_t> m_threads;
    std::queue<Task>       m_tasks;

    // [说明书] 1.6 多线程：任务队列由 mutex + cond 保护
    pthread_mutex_t m_mtx;   // 构造时 pthread_mutex_init，析构时 pthread_mutex_destroy
    pthread_cond_t  m_cv;    // 构造时 pthread_cond_init，析构时 pthread_cond_destroy
    bool            m_running = false;
};

} // namespace ecp
