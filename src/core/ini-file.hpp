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

#include <istream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>


namespace nl::rakis::i2cbus {

/**
 * @brief A line in an INI file that we cannot make sense of, or a file that cannot be read.
 */
class IniError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief A parsed INI file: sections of keys and values.
 *
 * Section names and keys are made lower case, like `I2CState` does. Values are kept as they are. A line that is not blank,
 * a comment (`#` or `;`), a `[section]` or a `key = value` is an error: we do not guess.
 */
class IniFile {
public:
    using Section = std::map<std::string, std::string>;

private:
    std::map<std::string, Section> sections_;

public:
    static IniFile parse(std::istream& in);
    static IniFile load(const std::string& path);

    const std::map<std::string, Section>& sections() const { return sections_; }
};

} // namespace nl::rakis::i2cbus
