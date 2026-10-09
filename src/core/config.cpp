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

#include "config.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <format>
#include <initializer_list>


namespace nl::rakis::i2cbus {

namespace {

constexpr std::string_view boardPrefix{ "board:" };
constexpr std::string_view displayPrefix{ "display:" };
constexpr std::string_view devicePrefix{ "device:" };
constexpr std::string_view interfacePrefix{ "interface:" };

constexpr std::string_view max7219{ "max7219" };
constexpr unsigned minBoardAddress{ 0x61 };         // the first address the bus controller hands out

const IniFile::Section& sectionOf(const IniFile& ini, const std::string& name, std::string_view why)
{
    auto it = ini.sections().find(name);
    if (it == ini.sections().end()) {
        throw ConfigError(std::format("[{}] is missing: {}", name, why));
    }
    return it->second;
}

const std::string& required(const IniFile::Section& section, const std::string& sectionName, std::string_view key)
{
    auto it = section.find(std::string(key));
    if ((it == section.end()) || it->second.empty()) {
        throw ConfigError(std::format("[{}] needs '{}'", sectionName, key));
    }
    return it->second;
}

unsigned number(const std::string& sectionName, const std::string& key, const std::string& text, unsigned max)
{
    unsigned value{ 0 };
    const auto end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if ((ec != std::errc{}) || (ptr != end) || (value > max)) {
        throw ConfigError(std::format("[{}] '{}' must be a number from 0 to {}, not '{}'", sectionName, key, max, text));
    }
    return value;
}

void warnUnknown(std::vector<std::string>& warnings, const IniFile::Section& section, const std::string& sectionName,
                 std::initializer_list<std::string_view> used, std::initializer_list<std::string_view> ignored)
{
    for (const auto& [key, value] : section) {
        auto in = [&](auto list) { return std::ranges::find(list, key) != list.end(); };
        if (!in(used) && !in(ignored)) {
            warnings.push_back(std::format("[{}] has '{}', which we do not use", sectionName, key));
        }
    }
}

} // namespace


bool Config::validName(std::string_view name)
{
    return !name.empty() && (name.size() <= 32)
        && std::ranges::all_of(name, [](unsigned char c) { return std::islower(c) || std::isdigit(c) || (c == '_') || (c == '-'); });
}

bool Config::validBoardId(std::string_view id)
{
    // Two groups of four bytes in hex, with a dash in between: e6614104-031c5032
    return (id.size() == 17) && (id[8] == '-')
        && std::ranges::all_of(id, [i = 0](unsigned char c) mutable { return (i++ == 8) || std::isxdigit(c); });
}


Config Config::from(const IniFile& ini)
{
    Config config;

    for (const auto& [sectionName, section] : ini.sections()) {
        if (!sectionName.starts_with(boardPrefix)) { continue; }
        const std::string name = sectionName.substr(boardPrefix.size());
        if (!validName(name)) {
            throw ConfigError(std::format("[{}]: '{}' is not a valid name (a-z, 0-9, '_' and '-')", sectionName, name));
        }
        std::string id = required(section, sectionName, "boardid");
        if (!validBoardId(id)) {
            throw ConfigError(std::format("[{}] 'boardid' must look like e6614104-031c5032, not '{}'", sectionName, id));
        }
        std::ranges::transform(id, id.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (auto same = std::ranges::find(config.boards_, id, &BoardConfig::boardId); same != config.boards_.end()) {
            throw ConfigError(std::format("[{}] has the same boardid as board '{}'", sectionName, same->name));
        }
        std::optional<uint8_t> address;
        if (auto it = section.find("address"); it != section.end()) {
            // Addresses below 0x61 are not ours to give (0x0a is the controller's), and 0x77 is the last one.
            const unsigned value = number(sectionName, "address", it->second, 0x77);
            if (value < minBoardAddress) {
                throw ConfigError(std::format("[{}] 'address' must be from {} to {}, not {}", sectionName, minBoardAddress, 0x77, value));
            }
            address = static_cast<uint8_t>(value);
        }
        warnUnknown(config.warnings_, section, sectionName, { "boardid", "address" }, {});
        config.boards_.push_back(BoardConfig{ name, id, address });
    }

    for (const auto& [sectionName, section] : ini.sections()) {
        if (!sectionName.starts_with(displayPrefix)) { continue; }

        DisplayConfig display;
        display.name = required(section, sectionName, "name");
        if (!validName(display.name)) {
            throw ConfigError(std::format("[{}] '{}' is not a valid name (a-z, 0-9, '_' and '-')", sectionName, display.name));
        }
        if (config.display(display.name) != nullptr) {
            throw ConfigError(std::format("[{}] name '{}' is used twice", sectionName, display.name));
        }

        const auto& deviceName = required(section, sectionName, "device");
        const auto deviceSection = std::string(devicePrefix) + deviceName;
        const auto& device = sectionOf(ini, deviceSection, std::format("[{}] uses it", sectionName));
        if (required(device, deviceSection, "type") != max7219) {
            throw ConfigError(std::format("[{}] 'type' is '{}': only {} is supported", deviceSection, device.at("type"), max7219));
        }

        const auto& interfaceName = required(device, deviceSection, "interface");
        const auto interfaceSection = std::string(interfacePrefix) + interfaceName;
        const auto& interface = sectionOf(ini, interfaceSection, std::format("[{}] uses it", deviceSection));

        display.board = required(interface, interfaceSection, "controller");
        if (config.board(display.board) == nullptr) {
            throw ConfigError(std::format("[{}] 'controller' is '{}', but there is no [board:{}]", interfaceSection, display.board, display.board));
        }

        // Several modules on one board are chained: the interface says how many, the display says which one it is.
        const unsigned modules = number(interfaceSection, "modules", required(interface, interfaceSection, "modules"), 255);
        display.index = number(sectionName, "index", required(section, sectionName, "index"), 255);
        if (display.index >= modules) {
            throw ConfigError(std::format("[{}] 'index' is {}, but [{}] has only {} module(s)", sectionName, display.index, interfaceSection, modules));
        }
        for (const auto& other : config.displays_) {
            if ((other.board == display.board) && (other.index == display.index)) {
                throw ConfigError(std::format("[{}] and display '{}' are both module {} of board '{}'", sectionName, other.name, display.index, display.board));
            }
        }

        if (auto it = section.find("brightness"); it != section.end()) {
            display.brightness = static_cast<uint8_t>(number(sectionName, "brightness", it->second, DisplayConfig::maxBrightness));
        }
        warnUnknown(config.warnings_, section, sectionName, { "name", "device", "index", "brightness" },
                    { "type", "decodemode", "scanlimit", "state" });
        config.displays_.push_back(std::move(display));
    }

    return config;
}


const BoardConfig* Config::board(std::string_view name) const
{
    auto it = std::ranges::find(boards_, name, &BoardConfig::name);
    return (it == boards_.end()) ? nullptr : &*it;
}

const BoardConfig* Config::boardById(std::string_view boardId) const
{
    auto it = std::ranges::find(boards_, boardId, &BoardConfig::boardId);
    return (it == boards_.end()) ? nullptr : &*it;
}

const DisplayConfig* Config::display(std::string_view name) const
{
    auto it = std::ranges::find(displays_, name, &DisplayConfig::name);
    return (it == displays_.end()) ? nullptr : &*it;
}

} // namespace nl::rakis::i2cbus
