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

#include <format>
#include <string>
#include <string_view>

#include <protocols/messages.hpp>

#include "config.hpp"


namespace nl::rakis::i2cbus {

/** `e6614104-031c5032`: the bytes of the id in hex, in two groups of four. Lower case, like `BoardConfig::boardId`. */
inline std::string boardIdString(const raspberrypi::protocols::BoardId& id)
{
    return std::format("{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}",
                       id.bytes[0], id.bytes[1], id.bytes[2], id.bytes[3], id.bytes[4], id.bytes[5], id.bytes[6], id.bytes[7]);
}

/** The other way around, for a text that `Config::validBoardId()` accepts (and that is lower case). */
inline raspberrypi::protocols::BoardId parseBoardId(std::string_view text)
{
    auto nibble = [](char c) -> uint8_t { return static_cast<uint8_t>((c <= '9') ? (c - '0') : (c - 'a' + 10)); };
    raspberrypi::protocols::BoardId id{ .id = 0 };
    for (unsigned i = 0; i < 8; i++) {
        const unsigned at = (i < 4) ? (i * 2) : (i * 2 + 1);        // skip the '-'
        id.bytes[i] = static_cast<uint8_t>((nibble(text[at]) << 4) | nibble(text[at + 1]));
    }
    return id;
}

} // namespace nl::rakis::i2cbus
