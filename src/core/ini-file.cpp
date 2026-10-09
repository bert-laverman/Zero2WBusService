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

#include "ini-file.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>


namespace nl::rakis::i2cbus {

static std::string trim(std::string_view s)
{
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && isSpace(s.front())) { s.remove_prefix(1); }
    while (!s.empty() && isSpace(s.back())) { s.remove_suffix(1); }
    return std::string(s);
}

static std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

IniFile IniFile::parse(std::istream& in)
{
    IniFile ini;
    std::string line;
    std::string section;        // empty until the first header: a key before that is an error
    unsigned lineNo{ 0 };

    while (std::getline(in, line)) {
        lineNo++;
        line = trim(line);
        if (line.empty() || (line[0] == '#') || (line[0] == ';')) {
            continue;
        }
        if (line.front() == '[') {
            if (line.back() != ']') {
                throw IniError(std::format("line {}: section header without ']'", lineNo));
            }
            section = lower(trim(line.substr(1, line.size() - 2)));
            if (section.empty()) {
                throw IniError(std::format("line {}: empty section name", lineNo));
            }
            ini.sections_[section];
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            throw IniError(std::format("line {}: expected 'key = value'", lineNo));
        }
        if (section.empty()) {
            throw IniError(std::format("line {}: key outside of a section", lineNo));
        }
        const auto key = lower(trim(line.substr(0, eq)));
        if (key.empty()) {
            throw IniError(std::format("line {}: empty key", lineNo));
        }
        if (!ini.sections_[section].emplace(key, trim(line.substr(eq + 1))).second) {
            throw IniError(std::format("line {}: '{}' appears twice in [{}]", lineNo, key, section));
        }
    }
    return ini;
}

IniFile IniFile::load(const std::string& path)
{
    std::ifstream in(path);
    if (!in) {
        throw IniError(std::format("cannot open '{}'", path));
    }
    try {
        return parse(in);
    } catch (const IniError& e) {
        throw IniError(std::format("{}: {}", path, e.what()));
    }
}

} // namespace nl::rakis::i2cbus
