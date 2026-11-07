#pragma once

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

    // Заборона копіювання пулу
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Додавання нової задачі до черги виконання
    void enqueue(std::function<void()> task);

    // Блокуюче очікування повного завершення всіх доданих завдань
    void wait_done();

    // Отримання поточної кількості робочих потоків у пулі
    size_t size() const;

private:
    std::vector<std::thread> workers_; // Масив робочих потоків

    std::queue<std::function<void()>> tasks_; // Черга завдань для виконання
    
    mutable std::mutex queue_mutex_; // М'ютекс для захисту доступу до черги tasks_
    std::condition_variable cv_task_; // Умовна змінна для повідомлення потоків про нове завдання

    std::mutex wait_mutex; // М'ютекс для захисту лічильника активних завдань
    std::condition_variable cv_wait_; // Умовна змінна для сповіщення wait_done() про завершення задач
    std::atomic<size_t> active_tasks_; // Атомарний лічильник задач, які ще виконуються або в черзі

    bool stop_; // Прапорець зупинки пулу потоків
};
