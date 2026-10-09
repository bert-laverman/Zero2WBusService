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

#include <string>

#include <poll.h>
#include <unistd.h>


namespace nl::rakis::i2cbus {

/**
 * Lines from the standard input, without ever waiting for one: the bus has to be served every 10 ms. When the input is closed
 * (as it is for a service that systemd starts) it stays quiet.
 */
class StdinLines {
    std::string pending_;
    bool open_{ true };

public:
    /** The next complete line, if there is one. */
    bool next(std::string& line) {
        for (;;) {
            if (auto eol = pending_.find('\n'); eol != std::string::npos) {
                line = pending_.substr(0, eol);
                pending_.erase(0, eol + 1);
                return true;
            }
            if (!open_) {
                return false;
            }
            pollfd fd{ STDIN_FILENO, POLLIN, 0 };
            if ((poll(&fd, 1, 0) <= 0) || !(fd.revents & (POLLIN | POLLHUP))) {
                return false;
            }
            char buffer[256];
            const auto n = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (n <= 0) {
                open_ = false;
                return false;
            }
            pending_.append(buffer, static_cast<size_t>(n));
        }
    }
};

} // namespace nl::rakis::i2cbus
