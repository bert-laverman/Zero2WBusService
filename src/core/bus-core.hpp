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
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "config.hpp"


namespace nl::rakis::i2cbus {

/**
 * @brief What a display shows. This is a variant so that segments and text can be added later, when the bus has a message for
 * them; clients and the bus code only ever look at the alternatives they know.
 */
struct Blank {
    bool operator==(const Blank&) const = default;
};
struct Number {
    int32_t value{ 0 };
    bool operator==(const Number&) const = default;
};
using Content = std::variant<Blank, Number>;

/**
 * @brief A change to a display. Only what is set is changed.
 */
struct Update {
    std::optional<Content> content;
    std::optional<uint8_t> brightness;
};

/**
 * @brief What we know of a display, and the answer to "what does it show".
 */
struct DisplayState {
    std::optional<Content> content;                 // empty as long as nobody has said what to show
    std::optional<uint8_t> brightness;
};

/**
 * @brief Where the core sends things. The bus implements this; a test can record what arrives.
 *
 * Both return false when the message did not go out, and the core then tries again at the next `flush()`.
 */
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool show(const BoardConfig& board, unsigned index, const Content& content) = 0;
    virtual bool setBrightness(const BoardConfig& board, unsigned index, uint8_t level) = 0;
};

enum class Status {
    Ok,
    UnknownDisplay,
    NothingToDo,        // an update that changes nothing
    OutOfRange,         // a number the display cannot show, or a brightness above 15
};

std::string_view toString(Status status);

/**
 * @brief Something that happened on the bus, for clients that want to know.
 */
struct Event {
    enum class Type { BoardOnline, BoardOffline };
    Type type;
    std::string board;
};

/**
 * @brief The heart of the service: it knows what every display should show, and sees to it that the boards get it.
 *
 * It does not know about the bus, nor about HTTP. The bus tells it which boards are there (`boardOnline()`,
 * `boardOffline()`), clients tell it what to show (`update()`), and a regular `flush()` sends what has to go out:
 *
 * * **Merging**: `update()` only stores. If a display gets 100 values between two flushes, one goes out: the last.
 * * **Sending again**: a Pico forgets everything when it restarts. When a board comes (back) online, everything we know about
 *   its displays is sent again, whether we sent it before or not.
 * * A display on a board that is offline keeps its state, and gets it when the board is back.
 *
 * Not thread safe: the caller serializes the calls (the bus loop, and the HTTP layer through a queue or a lock).
 */
class BusCore {
public:
    using Clock = std::chrono::steady_clock;

private:
    struct Entry {
        const DisplayConfig* config;
        DisplayState state;
        bool contentDirty{ false };
        bool brightnessDirty{ false };
        Clock::time_point retryAt{};                    // after a message did not go out: not before this moment
    };
    struct BoardEntry {
        const BoardConfig* config;
        bool online{ false };
    };

    Config config_;
    Sink& sink_;
    std::vector<Entry> displays_;
    std::vector<BoardEntry> boards_;
    std::deque<Event> events_;
    Clock::duration retryAfter_{ std::chrono::milliseconds(500) };
    uint64_t revision_{ 0 };

    Entry* find(std::string_view name);
    BoardEntry& boardOf(const DisplayConfig& display);

public:
    BusCore(Config config, Sink& sink);

    // Not copyable: the entries point into our own copy of the configuration.
    BusCore(const BusCore&) = delete;
    BusCore& operator=(const BusCore&) = delete;

    const Config& config() const { return config_; }

    /** Change a display. The change is sent at the next `flush()`, if the board is online. */
    Status update(std::string_view display, const Update& update);

    /** The bus saw a board appear. All its displays will be sent again. An unknown name is ignored (a board we do not use). */
    void boardOnline(std::string_view board);
    void boardOffline(std::string_view board);

    /**
     * Send what is waiting, to the boards that are online. Call this often; it does nothing when there is nothing to do.
     * A display whose message did not go out is left alone for `retryAfter()`, so a bus that refuses is not hammered.
     */
    void flush(Clock::time_point now = Clock::now());

    Clock::duration retryAfter() const { return retryAfter_; }
    void retryAfter(Clock::duration interval) { retryAfter_ = interval; }

    std::optional<DisplayState> state(std::string_view display) const;

    /** Goes up each time `update()` changes what a display should show. Whoever saves the state can tell whether to. */
    uint64_t revision() const { return revision_; }
    bool online(std::string_view board) const;

    /** Hand over the events since the last call. */
    std::vector<Event> takeEvents();
};

} // namespace nl::rakis::i2cbus
