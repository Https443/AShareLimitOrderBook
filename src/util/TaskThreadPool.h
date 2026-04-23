#ifndef TASK_THREAD_POOL_H
#define TASK_THREAD_POOL_H
#include "logger.h"
#include <exception>
#include <stdexcept>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <utility>

class TaskThreadPool {
public:
    explicit TaskThreadPool(size_t numThreads)
        : stop(false), activeWorkers(0), exceptionPtr(nullptr)
    {
        if (numThreads == 0)
        {
            throw std::invalid_argument("TaskThreadPool numThreads must be > 0");
        }
        for (size_t i = 0; i < numThreads; ++i)
        {
            workers.emplace_back([this] { this->workerLoop(); });
        }
    }

    ~TaskThreadPool() 
    {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop = true;
        }
        queueCondition.notify_all();
        for (auto &worker : workers) 
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
        LOG_INFO(app_log::logger(), "release task pool");
    }

    TaskThreadPool(const TaskThreadPool &) = delete;
    TaskThreadPool &operator=(const TaskThreadPool &) = delete;

    // 等待所有已提交任务执行完成（不再依赖析构时机）
    void waitForAll()
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        doneCondition.wait(lock, [this] { return tasks.empty() && activeWorkers == 0; });
    }

    std::exception_ptr getException() 
    {
        waitForAll();
        std::lock_guard<std::mutex> lock(queueMutex);
        return exceptionPtr;
    }

    bool hasException() 
    {
        waitForAll();
        std::lock_guard<std::mutex> lock(queueMutex);
        return exceptionPtr != nullptr;
    }

    // 添加任务到线程池，无需返回值
    template <typename Func>
    void enqueue(Func &&func)
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (stop)
            {
                throw std::runtime_error("TaskThreadPool is stopping, reject new task.");
            }
            if (exceptionPtr)
            {
                throw std::runtime_error("A previous task has thrown an exception.");
            }
            tasks.emplace(std::forward<Func>(func));
        }
        queueCondition.notify_one();
    }

    // 修改工作线程循环
    void workerLoop() 
    {
        while (true) 
        {
            std::move_only_function<void()> task;
            bool taskThrows = false;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCondition.wait(lock, [this] { return stop || !tasks.empty(); });
                if (stop && tasks.empty()) return;
                task = std::move(tasks.front());
                tasks.pop();
                ++activeWorkers;
            }
            try {
                task();
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    if (!exceptionPtr)
                    {
                        exceptionPtr = std::current_exception();
                    }
                    // 出现异常后禁止继续提交新任务，现有队列任务继续被消费完成
                    stop = true;
                }
                taskThrows = true;
            }

            {
                std::lock_guard<std::mutex> lock(queueMutex);
                --activeWorkers;
                if (tasks.empty() && activeWorkers == 0)
                {
                    doneCondition.notify_all();
                }
            }

            if (taskThrows)
            {
                // 唤醒等待中的工作线程，让其尽快感知 stop 状态
                queueCondition.notify_all();
            }
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::move_only_function<void()>> tasks;
    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::condition_variable doneCondition;
    size_t activeWorkers;
    bool stop;
    std::exception_ptr exceptionPtr;
};

#endif
