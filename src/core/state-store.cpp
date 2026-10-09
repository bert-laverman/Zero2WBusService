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

#include "state-store.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <filesystem>
#include <format>
#include <fstream>

#include "config.hpp"
#include "ini-file.hpp"


namespace nl::rakis::i2cbus {

namespace {

constexpr std::string_view boardPrefix{ "board:" };
constexpr std::string_view displayPrefix{ "display:" };

/** A whole-text number between two limits, or nothing. */
std::optional<long> parseNumber(const std::string& text, long min, long max)
{
    long value{ 0 };
    const auto end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if ((ec != std::errc{}) || (ptr != end) || (value < min) || (value > max)) {
        return std::nullopt;
    }
    return value;
}

/** Keys we do not know are an error: this file is ours, and a key we skip would be lost at the next save. */
void onlyKeys(const std::string& path, const std::string& sectionName, const IniFile::Section& section,
              std::initializer_list<std::string_view> known)
{
    for (const auto& [key, value] : section) {
        if (std::ranges::find(known, key) == known.end()) {
            throw IniError(std::format("{}: [{}] has '{}', which we do not know", path, sectionName, key));
        }
    }
}

void loadBoard(std::map<std::string, uint8_t>& addresses, const std::string& path, const std::string& sectionName,
               const IniFile::Section& section)
{
    const std::string boardId = sectionName.substr(boardPrefix.size());
    if (!Config::validBoardId(boardId)) {
        throw IniError(std::format("{}: [{}] is not a board id", path, sectionName));
    }
    onlyKeys(path, sectionName, section, { "address" });
    auto it = section.find("address");
    const auto address = (it == section.end()) ? std::nullopt : parseNumber(it->second, 1, 0x7f);
    if (!address) {
        throw IniError(std::format("{}: [{}] needs an 'address' from 1 to 127", path, sectionName));
    }
    addresses[boardId] = static_cast<uint8_t>(*address);
}

DisplayState loadDisplay(const std::string& path, const std::string& sectionName, const IniFile::Section& section)
{
    onlyKeys(path, sectionName, section, { "content", "brightness" });
    DisplayState state;
    if (auto it = section.find("content"); it != section.end()) {
        if (it->second == "blank") {
            state.content = Blank{};
        } else if (auto value = parseNumber(it->second, INT32_MIN, INT32_MAX)) {
            state.content = Number{ static_cast<int32_t>(*value) };
        } else {
            throw IniError(std::format("{}: [{}] 'content' must be a number or 'blank', not '{}'", path, sectionName, it->second));
        }
    }
    if (auto it = section.find("brightness"); it != section.end()) {
        auto level = parseNumber(it->second, 0, DisplayConfig::maxBrightness);
        if (!level) {
            throw IniError(std::format("{}: [{}] 'brightness' must be a number from 0 to {}, not '{}'", path, sectionName,
                                       DisplayConfig::maxBrightness, it->second));
        }
        state.brightness = static_cast<uint8_t>(*level);
    }
    return state;
}

} // namespace


StateStore StateStore::load(const std::string& path)
{
    StateStore store;
    store.path_ = path;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return store;                           // the first run
    }

    const IniFile ini = IniFile::load(path);            // a name: the loop must not run over a temporary
    for (const auto& [sectionName, section] : ini.sections()) {
        if (sectionName.starts_with(boardPrefix)) {
            loadBoard(store.addresses_, path, sectionName, section);
        } else if (sectionName.starts_with(displayPrefix)) {
            const std::string name = sectionName.substr(displayPrefix.size());
            if (!Config::validName(name)) {
                throw IniError(std::format("{}: [{}] is not a display name", path, sectionName));
            }
            store.displays_[name] = loadDisplay(path, sectionName, section);
        } else {
            throw IniError(std::format("{}: [{}] is not a [board:...] or [display:...] section", path, sectionName));
        }
    }
    return store;
}


std::optional<uint8_t> StateStore::address(std::string_view boardId) const
{
    auto it = addresses_.find(std::string(boardId));
    return (it == addresses_.end()) ? std::nullopt : std::optional<uint8_t>(it->second);
}


bool StateStore::setAddress(const std::string& boardId, uint8_t address)
{
    if (auto it = addresses_.find(boardId); (it != addresses_.end()) && (it->second == address)) {
        return true;                            // already saved
    }
    addresses_[boardId] = address;
    dirty_ = true;
    return save();
}


void StateStore::setDisplay(const std::string& name, const DisplayState& state)
{
    auto it = displays_.find(name);
    if (!state.content && !state.brightness) {
        if (it != displays_.end()) {
            displays_.erase(it);
            dirty_ = true;
        }
    } else if ((it == displays_.end()) || (it->second.content != state.content) || (it->second.brightness != state.brightness)) {
        displays_[name] = state;
        dirty_ = true;
    }
}


std::string StateStore::toString() const
{
    std::string text{ "# Written by i2cbus. Do not edit while it runs.\n" };
    for (const auto& [boardId, address] : addresses_) {
        text += std::format("\n[board:{}]\naddress = {}\n", boardId, address);
    }
    for (const auto& [name, state] : displays_) {
        text += std::format("\n[display:{}]\n", name);
        if (state.content) {
            if (const auto* number = std::get_if<Number>(&*state.content)) {
                text += std::format("content = {}\n", number->value);
            } else {
                text += "content = blank\n";
            }
        }
        if (state.brightness) {
            text += std::format("brightness = {}\n", static_cast<unsigned>(*state.brightness));
        }
    }
    return text;
}


bool StateStore::save()
{
    if (!dirty_) {
        return true;
    }
    if (path_.empty()) {
        dirty_ = false;                         // an in-memory store, for tests
        return true;
    }

    // Write beside the file and rename, so a power cut leaves the old file or the new one, never half of one.
    const std::string temp = path_ + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << toString();
        out.flush();
        if (!out) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path_, ec);
    if (ec) {
        return false;
    }
    dirty_ = false;
    return true;
}


std::vector<std::pair<std::string, Status>> restoreDisplays(BusCore& core, const StateStore& store)
{
    std::vector<std::pair<std::string, Status>> failed;
    for (const auto& [name, state] : store.displays()) {
        const auto status = core.update(name, Update{ state.content, state.brightness });
        if ((status != Status::Ok) && (status != Status::NothingToDo)) {
            failed.emplace_back(name, status);
        }
    }
    return failed;
}


void snapshotDisplays(const BusCore& core, StateStore& store)
{
    for (const auto& display : core.config().displays()) {
        const auto state = core.state(display.name);
        if (!state) {
            continue;
        }
        DisplayState keep{ state->content, std::nullopt };
        if (state->brightness != display.brightness) {
            keep.brightness = state->brightness;
        }
        store.setDisplay(display.name, keep);
    }
}

} // namespace nl::rakis::i2cbus
