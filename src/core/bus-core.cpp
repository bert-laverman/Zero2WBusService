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

#include "bus-core.hpp"

#include <algorithm>


namespace nl::rakis::i2cbus {

namespace {

// What fits in a MAX7219 with 8 digits: the minus sign takes one of them.
constexpr int32_t maxNumber{ 99'999'999 };
constexpr int32_t minNumber{ -9'999'999 };

bool inRange(const Content& content)
{
    if (const auto* number = std::get_if<Number>(&content)) {
        return (number->value >= minNumber) && (number->value <= maxNumber);
    }
    return true;
}

} // namespace


std::string_view toString(Status status)
{
    switch (status) {
    case Status::Ok:             return "ok";
    case Status::UnknownDisplay: return "unknown display";
    case Status::NothingToDo:    return "nothing to change";
    case Status::OutOfRange:     return "value out of range";
    }
    return "?";
}


BusCore::BusCore(Config config, Sink& sink)
    : config_(std::move(config)), sink_(sink)
{
    // The entries point into config_, which stays where it is: BusCore cannot be copied or moved.
    for (const auto& board : config_.boards()) {
        boards_.push_back(BoardEntry{ &board });
    }
    for (const auto& display : config_.displays()) {
        Entry entry{ &display, {}, false, false };
        entry.state.brightness = display.brightness;        // what the configuration wants, sent when the board is there
        entry.brightnessDirty = display.brightness.has_value();
        displays_.push_back(std::move(entry));
    }
}

BusCore::Entry* BusCore::find(std::string_view name)
{
    auto it = std::ranges::find(displays_, name, [](const Entry& e) -> std::string_view { return e.config->name; });
    return (it == displays_.end()) ? nullptr : &*it;
}

BusCore::BoardEntry& BusCore::boardOf(const DisplayConfig& display)
{
    // Config::from() has checked that the board exists.
    return *std::ranges::find(boards_, display.board, [](const BoardEntry& b) { return b.config->name; });
}


Status BusCore::update(std::string_view display, const Update& change)
{
    auto* entry = find(display);
    if (entry == nullptr) {
        return Status::UnknownDisplay;
    }
    if (!change.content && !change.brightness) {
        return Status::NothingToDo;
    }
    // Check everything before changing anything: an update is applied as a whole or not at all.
    if ((change.content && !inRange(*change.content))
        || (change.brightness && (*change.brightness > DisplayConfig::maxBrightness))) {
        return Status::OutOfRange;
    }

    if (change.content && (entry->state.content != change.content)) {
        entry->state.content = change.content;
        entry->contentDirty = true;
        revision_++;
    }
    if (change.brightness && (entry->state.brightness != change.brightness)) {
        entry->state.brightness = change.brightness;
        entry->brightnessDirty = true;
        revision_++;
    }
    return Status::Ok;
}


void BusCore::boardOnline(std::string_view name)
{
    auto it = std::ranges::find(boards_, name, [](const BoardEntry& b) -> std::string_view { return b.config->name; });
    if (it == boards_.end()) {
        return;
    }
    it->online = true;
    events_.push_back(Event{ Event::Type::BoardOnline, it->config->name });

    // The board has forgotten what it showed, or has never been told. Send all we know.
    for (auto& entry : displays_) {
        if (entry.config->board == it->config->name) {
            entry.brightnessDirty = entry.state.brightness.has_value();
            entry.contentDirty = entry.state.content.has_value();
            entry.retryAt = {};
        }
    }
}

void BusCore::boardOffline(std::string_view name)
{
    auto it = std::ranges::find(boards_, name, [](const BoardEntry& b) -> std::string_view { return b.config->name; });
    if ((it == boards_.end()) || !it->online) {
        return;
    }
    it->online = false;
    events_.push_back(Event{ Event::Type::BoardOffline, it->config->name });
}


void BusCore::flush(Clock::time_point now)
{
    for (auto& entry : displays_) {
        if (!entry.contentDirty && !entry.brightnessDirty) {
            continue;
        }
        const auto& board = boardOf(*entry.config);
        if (!board.online || (now < entry.retryAt)) {
            continue;
        }
        // Brightness first: the number then appears at the right strength at once. If it does not go out, the number waits as
        // well: it would come out at the wrong strength.
        if (entry.brightnessDirty) {
            if (!sink_.setBrightness(*board.config, entry.config->index, *entry.state.brightness)) {
                entry.retryAt = now + retryAfter_;
                continue;
            }
            entry.brightnessDirty = false;
        }
        if (entry.contentDirty) {
            if (!sink_.show(*board.config, entry.config->index, *entry.state.content)) {
                entry.retryAt = now + retryAfter_;
                continue;
            }
            entry.contentDirty = false;
        }
    }
}


std::optional<DisplayState> BusCore::state(std::string_view display) const
{
    auto it = std::ranges::find(displays_, display, [](const Entry& e) -> std::string_view { return e.config->name; });
    if (it == displays_.end()) {
        return std::nullopt;
    }
    return it->state;
}

bool BusCore::online(std::string_view board) const
{
    auto it = std::ranges::find(boards_, board, [](const BoardEntry& b) -> std::string_view { return b.config->name; });
    return (it != boards_.end()) && it->online;
}

std::vector<Event> BusCore::takeEvents()
{
    std::vector<Event> events(events_.begin(), events_.end());
    events_.clear();
    return events;
}

} // namespace nl::rakis::i2cbus
