#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void enqueue(std::function<void()> task);

    void wait_done();

    size_t size() const;

private:
    std::vector<std::thread> workers_;

    std::queue<std::function<void()>> tasks_;
    
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_task_;

    std::mutex wait_mutex;
    std::condition_variable cv_wait_;
    std::atomic<size_t> active_tasks_;

    bool stop_;
};
