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

#include <protocols/messages.hpp>
#include <protocols/max7219-messages.hpp>

#include "bus-core.hpp"


namespace nl::rakis::i2cbus {

/**
 * @brief Sends what the core wants shown to the boards on the I2C bus, as `Max7219` messages.
 *
 * It does not use `RemoteMAX7219`: that keeps its own idea of what the display shows and only sends what is new, and it does
 * not say whether a message went out. The core already keeps the state, and sends everything again when a board returns, so
 * here every call is one message, and the answer of the driver is passed on.
 *
 * The addresses come and go with the boards: `attach()` when the bus controller says a board appeared, `detach()` when it is
 * gone. A board without an address cannot be sent to, and the call says so (false).
 *
 * @tparam Driver the protocol driver, which provides `bool sendMessage(Command, uint8_t address, Msg&)`
 */
template <typename Driver>
class BusSink : public Sink {
    using Command = raspberrypi::protocols::Command;
    using Max7219Command = raspberrypi::protocols::Max7219Command;
    using MsgMax7219 = raspberrypi::protocols::MsgMax7219;

    Driver& driver_;
    std::map<std::string, uint8_t> addressByBoard_;
    std::map<uint8_t, std::string> boardByAddress_;

    bool send(const BoardConfig& board, unsigned index, Max7219Command command, int32_t value = 0) {
        auto it = addressByBoard_.find(board.name);
        if (it == addressByBoard_.end()) {
            return false;
        }
        MsgMax7219 msg{};
        msg.module = static_cast<uint8_t>(index);
        msg.command = raspberrypi::protocols::toValue(command);
        msg.value = value;
        return driver_.sendMessage(Command::Max7219, it->second, msg);
    }

public:
    explicit BusSink(Driver& driver) : driver_(driver) {}

    /** The board is on this address now. (A board that comes back may have another one.) */
    void attach(const std::string& board, uint8_t address) {
        if (auto old = addressByBoard_.find(board); old != addressByBoard_.end()) {
            boardByAddress_.erase(old->second);
        }
        addressByBoard_[board] = address;
        boardByAddress_[address] = board;
    }

    /** The board on this address is gone. Returns its name, or nothing if we did not know it. */
    std::optional<std::string> detach(uint8_t address) {
        auto it = boardByAddress_.find(address);
        if (it == boardByAddress_.end()) {
            return std::nullopt;
        }
        std::string board = std::move(it->second);
        boardByAddress_.erase(it);
        addressByBoard_.erase(board);
        return board;
    }

    bool show(const BoardConfig& board, unsigned index, const Content& content) override {
        if (const auto* number = std::get_if<Number>(&content)) {
            return send(board, index, Max7219Command::SetValue, number->value);
        }
        return send(board, index, Max7219Command::ClearDisplay);
    }

    bool setBrightness(const BoardConfig& board, unsigned index, uint8_t level) override {
        return send(board, index, Max7219Command::SetBrightness, level);
    }
};

} // namespace nl::rakis::i2cbus
