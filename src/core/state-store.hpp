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
#include <utility>
#include <vector>

#include "bus-core.hpp"


namespace nl::rakis::i2cbus {

/**
 * @brief What the service remembers between runs: the address of every board, and what every display shows.
 *
 * It lives in its own file, which only the service writes: `i2cbus.ini` is edited by hand and the service must never rewrite
 * it. A section per board, named by its board id (`e6614104-031c5032`, as in `BoardConfig::boardId`), and a section per display
 * that has something to remember, named by the name of the display:
 *
 *     [board:e6614104-031c5032]
 *     address = 97
 *
 *     [display:altitude]
 *     content = 35000                  (or `blank`; left out if nobody has said what to show)
 *     brightness = 8                   (only if it is not what the configuration says)
 *
 * Boards that are not in `i2cbus.ini` are kept too: they were given an address, and another board must not get it.
 *
 * Addresses are written at once, because a board that does not get its address back is a board that cannot be found. The
 * contents of displays change many times per second; `setDisplay()` only notes them, and `save()` writes.
 */
class StateStore {
    std::string path_;
    std::map<std::string, uint8_t> addresses_;
    std::map<std::string, DisplayState> displays_;
    bool dirty_{ false };

public:
    /** Read the file. A file that is not there is an empty store; one that cannot be understood is an error (IniError). */
    static StateStore load(const std::string& path);

    std::optional<uint8_t> address(std::string_view boardId) const;
    const std::map<std::string, uint8_t>& addresses() const { return addresses_; }

    /** Remember an address, and write the file if that changes anything. Returns false if the file could not be written. */
    bool setAddress(const std::string& boardId, uint8_t address);

    const std::map<std::string, DisplayState>& displays() const { return displays_; }

    /** Note what a display shows (nothing at all removes it). The file is written by the next `save()`. */
    void setDisplay(const std::string& name, const DisplayState& state);

    /** Write the file if anything changed since it was read or written. Returns false if that failed. */
    bool save();
    bool dirty() const { return dirty_; }

    /** The text of the file, for tests and for save(). */
    std::string toString() const;
};


/**
 * @brief Give the core back what the displays showed when the service stopped. Goes through `BusCore::update()`, like a client
 * would, so that it is sent to the boards when they come online.
 *
 * @return the displays that could not be restored, with the reason: they are no longer in the configuration, or the value no
 * longer fits. The rest has been restored.
 */
std::vector<std::pair<std::string, Status>> restoreDisplays(BusCore& core, const StateStore& store);

/** Note in the store what the core says the displays show. A brightness that is the configured one is not worth keeping. */
void snapshotDisplays(const BusCore& core, StateStore& store);

} // namespace nl::rakis::i2cbus
