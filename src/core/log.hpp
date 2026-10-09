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

#include <chrono>
#include <ctime>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>


namespace nl::rakis::i2cbus {

/**
 * @brief The log of the service: lines with a time and a level on standard error, and only those at or above the chosen level.
 *
 * The answers to commands go to standard output, so they are not drowned in the log when the two are kept apart
 * (`2>>i2cbus.log`).
 */
class Log {
public:
    enum class Level { Error, Warning, Info, Debug };

private:
    Level level_{ Level::Info };

    static const char* name(Level level) {
        switch (level) {
        case Level::Error:   return "ERROR";
        case Level::Warning: return "WARN ";
        case Level::Info:    return "INFO ";
        case Level::Debug:   return "DEBUG";
        }
        return "?    ";
    }

public:
    static std::optional<Level> parse(std::string_view text) {
        if (text == "error")   { return Level::Error; }
        if (text == "warning") { return Level::Warning; }
        if (text == "info")    { return Level::Info; }
        if (text == "debug")   { return Level::Debug; }
        return std::nullopt;
    }

    Level level() const { return level_; }
    std::string_view levelName() const {
        switch (level_) {
        case Level::Error:   return "error";
        case Level::Warning: return "warning";
        case Level::Info:    return "info";
        case Level::Debug:   return "debug";
        }
        return "?";
    }
    void level(Level level) { level_ = level; }
    bool enabled(Level level) const { return level <= level_; }

    void write(Level level, std::string_view text) const {
        if (!enabled(level)) {
            return;
        }
        while (!text.empty() && (text.back() == '\n')) {
            text.remove_suffix(1);
        }
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
        localtime_r(&now, &local);
        char when[16];
        std::strftime(when, sizeof(when), "%H:%M:%S", &local);
        std::cerr << when << ' ' << name(level) << ' ' << text << '\n';
    }

    template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const { write(Level::Error, std::format(fmt, std::forward<Args>(args)...)); }
    template <typename... Args>
    void warning(std::format_string<Args...> fmt, Args&&... args) const { write(Level::Warning, std::format(fmt, std::forward<Args>(args)...)); }
    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const { write(Level::Info, std::format(fmt, std::forward<Args>(args)...)); }
    template <typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) const { write(Level::Debug, std::format(fmt, std::forward<Args>(args)...)); }

    /**
     * A line from the bus controller, which has no levels of its own but a habit: `* ...` is something wrong, `- ...` is
     * something that happened, and the rest is the chatter, like the "Received Hello message" for every Hello of every board.
     */
    void controller(std::string_view line) const {
        if (line.starts_with('*')) {
            write(Level::Warning, line);
        } else if (line.starts_with('-')) {
            write(Level::Info, line);
        } else {
            write(Level::Debug, line);
        }
    }
};

} // namespace nl::rakis::i2cbus
