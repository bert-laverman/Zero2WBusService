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

#include <memory>
#include <string>

#include <sys/types.h>

#include "api.hpp"
#include "event-log.hpp"
#include "log.hpp"


namespace nl::rakis::i2cbus {

/**
 * @brief HTTP over a Unix domain socket in front of the `Api`: a thin layer that turns an HTTP request into an `ApiRequest` and
 * the `ApiResponse` back into an HTTP answer. Everything that decides what a request means is in `Api`.
 *
 * Who may talk to the service is decided by the file system: the socket is created with mode 0660 (or what you ask for), so
 * only its owner and its group can connect. There are no passwords or certificates. The service runs a few threads of its
 * own (four requests at a time, sixteen waiting), reads and writes with a timeout, and refuses a body longer than
 * `Api::maxBody`.
 *
 * It is a separate class, with the HTTP library hidden in its source, so that the rest does not have to compile it.
 */
class HttpServer {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    HttpServer(Api& api, EventLog& events, const Log& log);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /**
     * Create the socket and start serving, in a thread of its own.
     *
     * A socket that was left behind by a program that was killed is replaced; a socket that somebody still listens on, or a file
     * that is not a socket, is not touched and is an error. A path that starts with '@' (a socket without a file, and so
     * without access rights) is refused.
     *
     * @return empty if it worked, otherwise what went wrong
     */
    std::string start(const std::string& socketPath, mode_t mode = 0660);

    /** Stop serving, let waiting clients go, and remove the socket. Also done by the destructor. */
    void stop();
};

} // namespace nl::rakis::i2cbus
