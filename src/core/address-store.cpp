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

#include "address-store.hpp"

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>

#include "config.hpp"
#include "ini-file.hpp"


namespace nl::rakis::i2cbus {

namespace {
constexpr std::string_view boardPrefix{ "board:" };
}


AddressStore AddressStore::load(const std::string& path)
{
    AddressStore store;
    store.path_ = path;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return store;                           // the first run
    }

    const IniFile ini = IniFile::load(path);            // a name: the loop must not run over a temporary
    for (const auto& [sectionName, section] : ini.sections()) {
        if (!sectionName.starts_with(boardPrefix)) {
            throw IniError(std::format("{}: [{}] is not a [board:...] section", path, sectionName));
        }
        const std::string boardId = sectionName.substr(boardPrefix.size());
        if (!Config::validBoardId(boardId)) {
            throw IniError(std::format("{}: [{}] is not a board id", path, sectionName));
        }
        auto it = section.find("address");
        unsigned address{ 0 };
        if (it != section.end()) {
            const auto end = it->second.data() + it->second.size();
            const auto [ptr, err] = std::from_chars(it->second.data(), end, address);
            if ((err != std::errc{}) || (ptr != end) || (address == 0) || (address > 0x7f)) {
                address = 0;
            }
        }
        if (address == 0) {
            throw IniError(std::format("{}: [{}] needs an 'address' from 1 to 127", path, sectionName));
        }
        store.addresses_[boardId] = static_cast<uint8_t>(address);
    }
    return store;
}


std::optional<uint8_t> AddressStore::address(std::string_view boardId) const
{
    auto it = addresses_.find(std::string(boardId));
    return (it == addresses_.end()) ? std::nullopt : std::optional<uint8_t>(it->second);
}


std::string AddressStore::toString() const
{
    std::string text{ "# Written by i2cbus. Do not edit while it runs.\n" };
    for (const auto& [boardId, address] : addresses_) {
        text += std::format("\n[board:{}]\naddress = {}\n", boardId, address);
    }
    return text;
}


bool AddressStore::set(const std::string& boardId, uint8_t address)
{
    if (auto it = addresses_.find(boardId); (it != addresses_.end()) && (it->second == address)) {
        return true;                            // already saved
    }
    addresses_[boardId] = address;
    if (path_.empty()) {
        return true;                            // an in-memory store, for tests
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
    return !ec;
}

} // namespace nl::rakis::i2cbus
