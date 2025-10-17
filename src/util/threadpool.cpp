// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "util/threadpool.hpp"
#include <algorithm>

namespace coinbasechain {
namespace util {

ThreadPool::ThreadPool(size_t num_threads)
    : stop_(false)
{
    // If num_threads is 0, use hardware concurrency
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) {
            num_threads = 4;  // Fallback if hardware_concurrency() fails
        }
    }

    // Create worker threads
    workers_.reserve(num_threads);
    for(size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] {
            while(true) {
                std::function<void()> task;

                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex_);
                    this->condition_.wait(lock, [this]{
                        return this->stop_ || !this->tasks_.empty();
                    });

                    if(this->stop_ && this->tasks_.empty()) {
                        return;
                    }

                    task = std::move(this->tasks_.front());
                    this->tasks_.pop();
                }

                task();
            }
        });
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all();

    for(std::thread &worker: workers_) {
        worker.join();
    }
}

} // namespace util
} // namespace coinbasechain
