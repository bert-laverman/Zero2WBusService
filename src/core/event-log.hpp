#pragma once
/*
 * Copyright (c) 2026 by Bert Laverman. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "bus-core.hpp"


namespace nl::rakis::i2cbus {

/**
 * @brief The recent events, for any number of clients that each ask "what happened after the last one I saw".
 *
 * `BusCore::takeEvents()` hands each event to one caller; this keeps the last few hundred, numbered, so that every client can
 * read them at its own pace, and wait for the next one. The numbers (`seq`) only go up, also over a restart of the service:
 * they start at the time of the start, in milliseconds. A client that asks for what came after a number from before the
 * restart gets everything, and no number is ever reused.
 *
 * Thread safe, and independent of the core's lock: a client that waits does not hold anything up.
 */
class EventLog {
public:
    using SystemClock = std::chrono::system_clock;

    struct Entry {
        uint64_t seq;
        SystemClock::time_point time;
        Event::Type type;
        std::string board;
    };

    struct Batch {
        std::vector<Entry> entries;
        uint64_t next;          // what to give as `after` the next time
    };

    /** How many clients may wait at the same time; the next one is refused. */
    static constexpr unsigned maxWaiters{ 2 };

private:
    const size_t capacity_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Entry> entries_;
    uint64_t nextSeq_;
    unsigned waiters_{ 0 };
    bool shutdown_{ false };

    uint64_t latest() const { return nextSeq_ - 1; }

public:
    explicit EventLog(size_t capacity = 256, SystemClock::time_point start = SystemClock::now())
        : capacity_(capacity),
          nextSeq_(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(start.time_since_epoch()).count())) {}

    void append(const Event& event, SystemClock::time_point time = SystemClock::now()) {
        {
            std::lock_guard lock(mutex_);
            entries_.push_back(Entry{ nextSeq_++, time, event.type, event.board });
            while (entries_.size() > capacity_) {
                entries_.pop_front();
            }
        }
        changed_.notify_all();
    }

    /**
     * The events after `after` (0 for all there are). If there are none and `wait` is more than zero, wait until there is one,
     * or the time is up, or `shutdown()`. Returns nothing if too many clients are waiting already.
     */
    std::optional<Batch> since(uint64_t after, std::chrono::milliseconds wait = std::chrono::milliseconds::zero()) {
        std::unique_lock lock(mutex_);
        auto newer = [&] { return !entries_.empty() && (entries_.back().seq > after); };
        if (!newer() && (wait > std::chrono::milliseconds::zero()) && !shutdown_) {
            if (waiters_ >= maxWaiters) {
                return std::nullopt;
            }
            waiters_++;
            changed_.wait_for(lock, wait, [&] { return newer() || shutdown_; });
            waiters_--;
        }
        Batch batch;
        for (const auto& entry : entries_) {
            if (entry.seq > after) {
                batch.entries.push_back(entry);
            }
        }
        batch.next = std::max(after, latest());
        return batch;
    }

    /** Wake everybody who waits, and let nobody wait again. For stopping the server. */
    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            shutdown_ = true;
        }
        changed_.notify_all();
    }
};

/** Hand what happened to the boards (see `BusCore::takeEvents()`) to the log that clients read. The core's lock must be held. */
inline void drainEvents(BusCore& core, EventLog& log)
{
    for (const auto& event : core.takeEvents()) {
        log.append(event);
    }
}

} // namespace nl::rakis::i2cbus
