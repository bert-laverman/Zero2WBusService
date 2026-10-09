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

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ini-file.hpp"


namespace nl::rakis::i2cbus {

/**
 * @brief The configuration is wrong. The message says where.
 */
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief A Pico, known by a name of our own and the id of its flash (`e6614104-031c5032`).
 */
struct BoardConfig {
    std::string name;
    std::string boardId;                                    // lower case, so it can be compared and used as a key
    std::optional<uint8_t> address;                         // a wish from the file, used when the state file has none
};

/**
 * @brief A display, as clients know it: a name, and the place where it sits.
 */
struct DisplayConfig {
    static constexpr unsigned digitsPerModule{ 8 };         // a MAX7219 drives 8 digits
    static constexpr unsigned maxBrightness{ 15 };

    std::string name;                                       // what clients use, `[a-z0-9_-]+`
    std::string board;                                      // the name of a BoardConfig
    unsigned index{ 0 };                                    // the module on that board, counting from 0
    std::optional<uint8_t> brightness;                      // at most maxBrightness
};

/**
 * @brief What `i2cbus.ini` says: which boards and displays there are.
 *
 * The file keeps the shape of the old `i2c-state.ini`. A display points to a device, a device to an interface, and an interface
 * to the board that controls it; here that chain is resolved, so the rest of the program only sees displays and boards.
 * The addresses of the boards are handed out by the bus controller and live in the state file (`AddressStore`). An `address`
 * in a board section, as the old `i2c-state.ini` has, is only used for a board that the state file does not know yet.
 *
 * A mistake is an error (`ConfigError`), not something we skip: a display that silently does not exist is hard to find.
 * Keys we do not use are not an error (the old file has `decodemode`, `scanlimit`, ...), but they are listed in `warnings()`.
 */
class Config {
    std::vector<BoardConfig> boards_;
    std::vector<DisplayConfig> displays_;
    std::vector<std::string> warnings_;

public:
    static Config from(const IniFile& ini);
    static Config load(const std::string& path) { return from(IniFile::load(path)); }

    static bool validName(std::string_view name);
    static bool validBoardId(std::string_view id);

    const std::vector<BoardConfig>& boards() const { return boards_; }
    const std::vector<DisplayConfig>& displays() const { return displays_; }
    const std::vector<std::string>& warnings() const { return warnings_; }

    const BoardConfig* board(std::string_view name) const;
    const BoardConfig* boardById(std::string_view boardId) const;      // the id in lower case, as BoardConfig::boardId
    const DisplayConfig* display(std::string_view name) const;
};

} // namespace nl::rakis::i2cbus
