#include "thread_pool.h"

ThreadPool::ThreadPool(size_t num_threads) : active_tasks_(0), stop_(false) {
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] {
            while (true) {
                std::function<void()> task;

                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex_);

                    // Потік спить, доки пул не зупинять або в черзі не з'явиться нове завдання
                    this->cv_task_.wait(lock, [this]() {
                        return this->stop_ || !this->tasks_.empty();
                    });

                    // Якщо пул зупинено і черга порожня — потік завершує свою роботу
                    if (this->stop_ && this->tasks_.empty()) {
                        return;
                    }

                    // Витягуємо завдання з черги
                    task = std::move(this->tasks_.front());
                    this->tasks_.pop();
                }

                task();

                {
                    std::unique_lock<std::mutex> lock(this->wait_mutex);
                    --active_tasks_;
                    if (active_tasks_ == 0) {
                        cv_wait_.notify_all(); // Будимо wait_done(), якщо всі задачі завершено
                    }
                }
            }
        });
    }
}

// Додавання нового завдання в пул
void ThreadPool::enqueue(std::function<void()> task) {
    // Спочатку збільшуємо лічильник активних завдань
    {
        std::unique_lock<std::mutex> lock(wait_mutex);
        ++active_tasks_;
    }

    // Кладемо нове завдання в чергу
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        tasks_.push(std::move(task));
    }

    // Сповіщаємо один із вільних потоків, що з'явилась робота
    cv_task_.notify_one();
}

// Блокує викликний потік, поки всі активні задачі не закінчаться
void ThreadPool::wait_done() {
    std::unique_lock<std::mutex> lock(wait_mutex);
    cv_wait_.wait(lock, [this]() {
        return active_tasks_ == 0;
    });
}

// Повертає кількість робочих потоків
size_t ThreadPool::size() const {
    return workers_.size();
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }

    // Будимо абсолютно всі потоки, щоб вони завершили роботу
    cv_task_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}