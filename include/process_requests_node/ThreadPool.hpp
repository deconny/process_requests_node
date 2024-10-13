#ifndef THREAD_POOL_HPP
#define THREAD_POOL_HPP

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// 任务结构
struct Task
{
    std::function<void()> func;

    Task(std::function<void()> f)
        : func(std::move(f))
    {}
};

class ThreadPool
{
public:
    // 默认构造函数，默认线程数为4
    explicit ThreadPool(size_t num_threads = 4)
        : stop_(false)
        , completed_tasks_(0)
    {
        resize(num_threads);
    }

    ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (std::thread &worker : workers_)
        {
            if (worker.joinable())
                worker.join();
        }
    }

    // 动态调整线程池大小
    void resize(size_t new_size)
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (new_size == workers_.size())
            return;

        if (new_size > workers_.size())
        {
            for (size_t i = workers_.size(); i < new_size; ++i)
            {
                workers_.emplace_back([this]() {
                    while (true)
                    {
                        Task task([] {});
                        {
                            std::unique_lock<std::mutex> lock(this->queue_mutex_);
                            this->condition_.wait(lock, [this]() { return this->stop_ || !this->tasks_.empty(); });

                            if (this->stop_ && this->tasks_.empty())
                                return;

                            task = std::move(this->tasks_.front());
                            this->tasks_.pop();
                        }
                        try
                        {
                            task.func();
                            completed_tasks_++; // 任务完成计数
                        }
                        catch (const std::exception &e)
                        {
                            std::cerr << "Task threw an exception: " << e.what() << std::endl;
                        }
                        catch (...)
                        {
                            std::cerr << "Task threw an unknown exception." << std::endl;
                        }
                    }
                });
            }
        }
        else
        {
            for (size_t i = new_size; i < workers_.size(); ++i)
            {
                workers_[i].detach();
            }
            workers_.resize(new_size);
        }
    }

    // 提交任务
    template<class F, class... Args>
    auto enqueue(F &&f, Args &&...args) -> std::future<typename std::invoke_result<F, Args...>::type>
    {
        using return_type = typename std::invoke_result<F, Args...>::type;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_)
                throw std::runtime_error("Enqueue on stopped ThreadPool");

            tasks_.emplace(Task([task]() { (*task)(); }));
        }
        condition_.notify_one();

        // 动态调整线程数，如果任务数量超过当前线程数则增加线程
        if (get_task_count() > workers_.size())
        {
            // 限制最大线程数，比如最大20个线程
            size_t max_threads = 20;
            if (workers_.size() < max_threads)
            {
                resize(std::min(workers_.size() + 1, max_threads));
            }
        }
        return res;
    }

    // 获取当前任务数量
    size_t get_task_count()
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        return tasks_.size();
    }

    // 获取完成的任务数量
    size_t get_completed_task_count() const
    {
        return completed_tasks_;
    }

private:
    std::vector<std::thread> workers_;    // 工作线程
    std::queue<Task> tasks_;              // 任务队列
    std::mutex queue_mutex_;              // 队列互斥锁
    std::condition_variable condition_;   // 条件变量
    std::atomic<bool> stop_;              // 停止标志
    std::atomic<size_t> completed_tasks_; // 已完成任务计数
};

#endif // THREAD_POOL_HPP