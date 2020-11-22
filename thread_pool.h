/*
 * =====================================================================================
 *
 *       Filename:  thread_pool.h
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  2018年06月15日 14时43分53秒
 *       Revision:  none
 *       Compiler:  g++
 *
 *         Author:  mail.yangjun@qq.com 
 *   Organization:  
 *
 * =====================================================================================
 */

#ifndef UTILITY_THREAD_POOL_H_
#define UTILITY_THREAD_POOL_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>

namespace utility {

class thread_pool {
public:
    thread_pool();
    ~thread_pool();

    thread_pool(const thread_pool& other) = delete;
    thread_pool& operator =(const thread_pool& other) = delete;

    template <class Function, class... Args>
    std::future<typename std::result_of<Function(Args...)>::type>
    async(Function&& f, Args&&... args);

    void start();
    void stop();

    void reserve_max_task(std::size_t size) {
        max_task_size_ = size;
    }

    void reserve_permanent_thread(std::size_t size) {
        permanent_thread_size_ = size;
    }

    void reserve_dynamic_thread(std::size_t size) {
        dynamic_thread_size_ = size;
    }

private:
    void run();

    std::deque<std::function<void()> > task_list_;
    std::condition_variable produce_task_cv_;
    std::condition_variable consume_task_cv_;
    std::mutex task_list_mutex_;

    std::map<std::thread::id, std::thread> thread_map_;
    std::mutex thread_map_mutex_;

    std::size_t max_task_size_;
    std::size_t permanent_thread_size_;
    std::size_t dynamic_thread_size_;

    std::atomic_size_t waiting_count_;
    bool is_stop_;
};

thread_pool::thread_pool()
    : max_task_size_(std::numeric_limits<std::size_t>::max()), 
      permanent_thread_size_(0), 
      dynamic_thread_size_(std::numeric_limits<std::size_t>::max()), 
      waiting_count_(0), 
      is_stop_(true) {
    start();
}

thread_pool::~thread_pool() {
    stop();
}

void thread_pool::start() {  
    std::lock_guard<std::mutex> lock(thread_map_mutex_);
    if (!is_stop_) {
        return;
    }

    std::thread thread(&thread_pool::run, this);
    thread_map_.emplace(thread.get_id(), std::move(thread));
    is_stop_ = false;
}

void thread_pool::stop() {
    std::map<std::thread::id, std::thread> thread_map;
    {
        std::lock_guard<std::mutex> lock(thread_map_mutex_);
        if (is_stop_) {
            return;
        }

        is_stop_ = true;
        thread_map.swap(thread_map_);
    }

    for (auto& pair : thread_map) {
        pair.second.join();
    }
}

template <class Function, class... Args>
std::future<typename std::result_of<Function(Args...)>::type>
thread_pool::async(Function&& f, Args&&... args) {
    auto task = std::make_shared<std::packaged_task<typename std::result_of<Function(Args...)>::type()> >(std::bind(f, args...));
    auto future = task->get_future();
    std::unique_lock<std::mutex> lock(task_list_mutex_);
    while (!is_stop_
            && !produce_task_cv_.wait_for(lock, 
                                          std::chrono::seconds(1), 
                                          [this]() {
                                              return task_list_.size() < max_task_size_;
                                          }));

    if (!is_stop_) {
        task_list_.emplace_back([task]() {
                                    (*task)();
                                }); 
        consume_task_cv_.notify_one();
    }
    return future;
}

void thread_pool::run() {
    while (!is_stop_) {
        std::function<void()> task;
        bool remaining_task = false;
        {
            std::unique_lock<std::mutex> lock(task_list_mutex_);
            ++waiting_count_;
            if (consume_task_cv_.wait_for(lock, 
                                          std::chrono::seconds(1), 
                                          [this]() {
                                              return !task_list_.empty();
                                          })) {
                task = task_list_.front();
                task_list_.pop_front();
                produce_task_cv_.notify_one();
            }
            --waiting_count_;

            remaining_task = !task_list_.empty();
        }
     
        if (remaining_task && 0 == waiting_count_) {
            std::lock_guard<std::mutex> lock(thread_map_mutex_);
            if (!is_stop_ 
                    && thread_map_.size() < permanent_thread_size_ + dynamic_thread_size_) {
                std::thread thread(&thread_pool::run, this);
                thread_map_.emplace(thread.get_id(), std::move(thread));
            }
        }

        if (task) {
            task();
        } else {
            std::lock_guard<std::mutex> lock(thread_map_mutex_);
            if (!is_stop_ 
                    && permanent_thread_size_ < thread_map_.size()
                    && !(0 == permanent_thread_size_ && 1 == thread_map_.size())) {
                auto it = thread_map_.find(std::this_thread::get_id()); 
                if (thread_map_.end() != it) {
                    it->second.detach();
                    thread_map_.erase(it);
                    break;
                }
            }
        }
    }
}

} // namespace utility

#endif // UTILITY_THREAD_POOL_H_
