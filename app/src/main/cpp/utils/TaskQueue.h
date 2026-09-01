/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <atomic>

namespace guitarrackcraft {

class TaskQueue {
public:
    using Task = std::function<void()>;

    void post(Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    // 同步执行：入队并等待完成。超时返回 false（任务可能仍会在队列
    // 线程稍后执行——promise 用 shared_ptr 持有，超时返回不会留下悬空引用）。
    // 防止队列线程已 stop/卡死时调用方永久阻塞。
    bool postAndWait(Task task, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        post([task = std::move(task), done]() {
            task();
            done->set_value();
        });
        return future.wait_for(timeout) == std::future_status::ready;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

    // Drain and execute all queued tasks. Returns true if any were executed.
    bool drain() {
        std::queue<Task> batch;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::swap(batch, tasks_);
        }
        bool any = !batch.empty();
        while (!batch.empty()) {
            batch.front()();
            batch.pop();
        }
        return any;
    }

    // Wait for tasks and drain. Returns false if stopped.
    bool waitAndDrain() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !tasks_.empty() || stopped_; });
        if (stopped_ && tasks_.empty()) return false;
        std::queue<Task> batch;
        std::swap(batch, tasks_);
        lock.unlock();
        while (!batch.empty()) {
            batch.front()();
            batch.pop();
        }
        return true;
    }

    bool isStopped() const { return stopped_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Task> tasks_;
    bool stopped_ = false;
};

} // namespace guitarrackcraft
