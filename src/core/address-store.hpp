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
#include <map>
#include <optional>
#include <string>
#include <string_view>


namespace nl::rakis::i2cbus {

/**
 * @brief The I2C address that each board has been given, so that a board gets the same address after a restart.
 *
 * It lives in its own file, which only the service writes: `i2cbus.ini` is edited by hand and the service must never rewrite
 * it. A section per board, named by its board id (`e6614104-031c5032`, as in `BoardConfig::boardId`):
 *
 *     [board:e6614104-031c5032]
 *     address = 97
 *
 * Boards that are not in `i2cbus.ini` are kept too: they were given an address, and another board must not get it.
 */
class AddressStore {
    std::string path_;
    std::map<std::string, uint8_t> addresses_;

public:
    /** Read the file. A file that is not there is an empty store; one that cannot be understood is an error (IniError). */
    static AddressStore load(const std::string& path);

    std::optional<uint8_t> address(std::string_view boardId) const;
    const std::map<std::string, uint8_t>& all() const { return addresses_; }

    /** Remember an address, and write the file if that changes anything. Returns false if the file could not be written. */
    bool set(const std::string& boardId, uint8_t address);

    /** The text of the file, for tests and for save(). */
    std::string toString() const;
};

} // namespace nl::rakis::i2cbus
