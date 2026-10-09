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

#include "commands.hpp"

#include <charconv>
#include <sstream>
#include <string>


namespace nl::rakis::i2cbus {

namespace {

bool toInt(const std::string& text, int32_t& value)
{
    const auto end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return (ec == std::errc{}) && (ptr == end);
}

void help(std::ostream& out)
{
    out << "online <board> | offline <board>     a board appears or disappears\n"
           "set <display> <number>|blank         show a number, or nothing\n"
           "brightness <display> <0-15>\n"
           "flush                                send what is waiting\n"
           "show                                 what the displays should show\n"
           "events                               what happened to the boards\n"
           "quit\n";
}

void show(BusCore& core, std::ostream& out)
{
    for (const auto& display : core.config().displays()) {
        const auto state = *core.state(display.name);
        out << "  " << display.name << " (" << display.board << ", module " << display.index << ", "
            << (core.online(display.board) ? "online" : "offline") << "): ";
        if (!state.content) {
            out << "-";
        } else if (const auto* n = std::get_if<Number>(&*state.content)) {
            out << n->value;
        } else {
            out << "blank";
        }
        if (state.brightness) {
            out << ", brightness " << static_cast<unsigned>(*state.brightness);
        }
        out << "\n";
    }
}

} // namespace


bool runCommand(BusCore& core, std::string_view line, std::ostream& out)
{
    std::istringstream words{ std::string(line) };
    std::string command, arg1, arg2;
    words >> command >> arg1 >> arg2;

    if (command.empty()) {
        return true;
    }
    if (command == "quit") {
        return false;
    }
    if (command == "help") {
        help(out);
    } else if (command == "online") {
        core.boardOnline(arg1);
    } else if (command == "offline") {
        core.boardOffline(arg1);
    } else if (command == "flush") {
        core.flush();
    } else if (command == "set") {
        int32_t value{ 0 };
        Update update;
        if (arg2 == "blank") {
            update.content = Blank{};
        } else if (toInt(arg2, value)) {
            update.content = Number{ value };
        } else {
            out << "  '" << arg2 << "' is not a number\n";
            return true;
        }
        out << "  " << toString(core.update(arg1, update)) << "\n";
    } else if (command == "brightness") {
        int32_t level{ 0 };
        if (!toInt(arg2, level) || (level < 0) || (level > static_cast<int32_t>(DisplayConfig::maxBrightness))) {
            out << "  '" << arg2 << "' is not a number from 0 to " << DisplayConfig::maxBrightness << "\n";
            return true;
        }
        out << "  " << toString(core.update(arg1, Update{ std::nullopt, static_cast<uint8_t>(level) })) << "\n";
    } else if (command == "show") {
        show(core, out);
    } else if (command == "events") {
        for (const auto& event : core.takeEvents()) {
            out << "  " << ((event.type == Event::Type::BoardOnline) ? "online " : "offline ") << event.board << "\n";
        }
    } else {
        out << "  unknown command '" << command << "'; try 'help'\n";
    }
    return true;
}

} // namespace nl::rakis::i2cbus
