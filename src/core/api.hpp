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

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "bus-core.hpp"
#include "event-log.hpp"


namespace nl::rakis::i2cbus {

/**
 * @brief A request for the API, with nothing in it that belongs to a transport (HTTP over a socket, ...).
 */
struct ApiRequest {
    std::string method;                                 // GET, PUT, DELETE
    std::string path;                                   // /displays/altitude
    std::map<std::string, std::string> query;           // after=12&wait=5
    std::string body;                                   // JSON
};

struct ApiResponse {
    int status{ 200 };
    std::string body;                                   // JSON
    std::vector<std::pair<std::string, std::string>> headers;
};

/**
 * @brief What clients can ask of the service. Every request comes in as an `ApiRequest` and goes out as an `ApiResponse`, so the
 * routes can be tested without a socket, and another transport can be put in front later.
 *
 * | Request | Meaning |
 * |---|---|
 * | `GET /displays`, `GET /displays/<name>` | what the displays should show, and whether their board is online |
 * | `PUT /displays/<name>` `{"value": 35000, "brightness": 3}` | show a number, set the brightness; either or both |
 * | `DELETE /displays/<name>` | blank the display |
 * | `GET /boards` | the boards, and whether they are online |
 * | `GET /events?after=<seq>&wait=<seconds>` | what happened to the boards after `seq`; with `wait`, the call waits for the next one |
 *
 * Answers are JSON. A mistake is an answer with a status (400 the request makes no sense, 404 there is no such thing, 405 the
 * method does not fit, 413 too long, 422 a value the display cannot show, 503 too many clients are waiting) and
 * `{"error": "..."}`. Everything in a request is checked: a field that is not known is an error, not ignored.
 *
 * Safe to call from several threads: the core is used under `mutex`, which the caller shares with whoever else uses it.
 */
class Api {
    BusCore& core_;
    std::mutex& mutex_;
    EventLog& events_;

public:
    static constexpr size_t maxBody{ 4096 };
    static constexpr unsigned maxWaitSeconds{ 30 };

    Api(BusCore& core, std::mutex& mutex, EventLog& events) : core_(core), mutex_(mutex), events_(events) {}

    ApiResponse handle(const ApiRequest& request);
};

} // namespace nl::rakis::i2cbus
