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

#include "api.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <limits>

#include "json.hpp"


namespace nl::rakis::i2cbus {

namespace {

using json = nlohmann::json;

ApiResponse reply(int status, const json& body)
{
    return ApiResponse{ status, body.dump(-1, ' ', false, json::error_handler_t::replace), {} };
}

ApiResponse error(int status, const std::string& message)
{
    return reply(status, json{ { "error", message } });
}

ApiResponse notAllowed(const std::string& allow)
{
    auto response = error(405, "method not allowed");
    response.headers.emplace_back("Allow", allow);
    return response;
}

std::string isoTime(std::chrono::system_clock::time_point time)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

/** What a display shows, as a client sees it. A new kind of content gets a new `type`; clients only look at the ones they know. */
json contentJson(const std::optional<Content>& content)
{
    if (!content) {
        return nullptr;
    }
    if (const auto* number = std::get_if<Number>(&*content)) {
        return json{ { "type", "number" }, { "value", number->value } };
    }
    return json{ { "type", "blank" } };
}

/** The core's lock must be held. */
json displayJson(const BusCore& core, const DisplayConfig& display)
{
    const auto state = *core.state(display.name);
    json out{ { "name", display.name }, { "board", display.board }, { "module", display.index },
              { "online", core.online(display.board) }, { "content", contentJson(state.content) },
              { "brightness", nullptr } };
    if (state.brightness) {
        out["brightness"] = static_cast<unsigned>(*state.brightness);
    }
    return out;
}

enum class Integer { Ok, NotAnInteger, TooBig };

/** A JSON number is an integer only if it is written as one: 35000.0 and 3.5e4 are not. */
Integer asInteger(const json& value, int64_t& out)
{
    if (value.is_number_unsigned()) {
        const auto unsignedValue = value.get<uint64_t>();
        if (unsignedValue > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return Integer::TooBig;
        }
        out = static_cast<int64_t>(unsignedValue);
        return Integer::Ok;
    }
    if (value.is_number_integer()) {
        out = value.get<int64_t>();
        return Integer::Ok;
    }
    return Integer::NotAnInteger;
}

/** A whole-text number from the query, or nothing. */
bool queryNumber(const std::map<std::string, std::string>& query, const std::string& key, uint64_t& out, bool& present)
{
    auto it = query.find(key);
    present = (it != query.end());
    if (!present) {
        return true;
    }
    const auto end = it->second.data() + it->second.size();
    const auto [ptr, ec] = std::from_chars(it->second.data(), end, out);
    return (ec == std::errc{}) && (ptr == end) && !it->second.empty();
}

ApiResponse statusError(Status status)
{
    switch (status) {
    case Status::UnknownDisplay: return error(404, "no such display");
    case Status::OutOfRange:     return error(422, "value out of range");
    case Status::NothingToDo:    return error(400, "nothing to change: give 'value' and/or 'brightness'");
    case Status::Ok:             break;
    }
    return error(500, "internal error");
}

} // namespace


ApiResponse Api::handle(const ApiRequest& request)
{
    const std::string& path = request.path;
    const std::string& method = request.method;

    if (path == "/displays") {
        if (method != "GET") { return notAllowed("GET"); }
        std::lock_guard lock(mutex_);
        json list = json::array();
        for (const auto& display : core_.config().displays()) {
            list.push_back(displayJson(core_, display));
        }
        return reply(200, json{ { "displays", list } });
    }

    if (path.starts_with("/displays/")) {
        const std::string name = path.substr(std::string_view("/displays/").size());
        const DisplayConfig* display = Config::validName(name) ? core_.config().display(name) : nullptr;
        if (display == nullptr) {
            return error(404, "no such display");
        }

        if (method == "GET") {
            std::lock_guard lock(mutex_);
            return reply(200, displayJson(core_, *display));
        }

        Update update;
        if (method == "DELETE") {
            update.content = Blank{};
        } else if (method == "PUT") {
            if (request.body.size() > maxBody) {
                return error(413, "the body is too long");
            }
            const json body = json::parse(request.body, nullptr, false);
            if (body.is_discarded()) {
                return error(400, "the body is not valid JSON");
            }
            if (!body.is_object()) {
                return error(400, "the body must be a JSON object");
            }
            for (const auto& [key, field] : body.items()) {
                int64_t number{ 0 };
                if (key == "value") {
                    switch (asInteger(field, number)) {
                    case Integer::NotAnInteger: return error(400, "'value' must be an integer");
                    case Integer::TooBig:       return error(422, "'value' is out of range");
                    case Integer::Ok:           break;
                    }
                    if ((number < std::numeric_limits<int32_t>::min()) || (number > std::numeric_limits<int32_t>::max())) {
                        return error(422, "'value' is out of range");
                    }
                    update.content = Number{ static_cast<int32_t>(number) };
                } else if (key == "brightness") {
                    switch (asInteger(field, number)) {
                    case Integer::NotAnInteger: return error(400, "'brightness' must be an integer");
                    case Integer::TooBig:       return error(422, "'brightness' must be from 0 to 15");
                    case Integer::Ok:           break;
                    }
                    if ((number < 0) || (number > static_cast<int64_t>(DisplayConfig::maxBrightness))) {
                        return error(422, "'brightness' must be from 0 to 15");
                    }
                    update.brightness = static_cast<uint8_t>(number);
                } else {
                    return error(400, std::format("unknown field '{}'", key));
                }
            }
        } else {
            return notAllowed("GET, PUT, DELETE");
        }

        std::lock_guard lock(mutex_);
        const auto status = core_.update(display->name, update);
        if (status != Status::Ok) {
            return statusError(status);
        }
        return reply(200, displayJson(core_, *display));
    }

    if (path == "/boards") {
        if (method != "GET") { return notAllowed("GET"); }
        std::lock_guard lock(mutex_);
        json list = json::array();
        for (const auto& board : core_.config().boards()) {
            list.push_back(json{ { "name", board.name }, { "boardId", board.boardId }, { "online", core_.online(board.name) } });
        }
        return reply(200, json{ { "boards", list } });
    }

    if (path == "/events") {
        if (method != "GET") { return notAllowed("GET"); }
        uint64_t after{ 0 };
        uint64_t waitSeconds{ 0 };
        bool present{ false };
        if (!queryNumber(request.query, "after", after, present)) {
            return error(400, "'after' must be a number");
        }
        if (!queryNumber(request.query, "wait", waitSeconds, present) || (waitSeconds > maxWaitSeconds)) {
            return error(400, std::format("'wait' must be a number of seconds from 0 to {}", maxWaitSeconds));
        }

        // No lock on the core: waiting must not hold anybody up.
        const auto batch = events_.since(after, std::chrono::milliseconds(waitSeconds * 1000));
        if (!batch) {
            return error(503, "too many clients are waiting for events");
        }
        json list = json::array();
        for (const auto& entry : batch->entries) {
            list.push_back(json{ { "seq", entry.seq }, { "time", isoTime(entry.time) },
                                 { "type", (entry.type == Event::Type::BoardOnline) ? "boardOnline" : "boardOffline" },
                                 { "board", entry.board } });
        }
        return reply(200, json{ { "events", list }, { "next", batch->next } });
    }

    return error(404, "not found");
}

} // namespace nl::rakis::i2cbus
