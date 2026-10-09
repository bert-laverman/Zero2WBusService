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

#include "http-server.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "httplib.h"


namespace nl::rakis::i2cbus {

namespace {

constexpr size_t workerThreads{ 4 };            // requests handled at the same time
constexpr size_t queuedConnections{ 16 };       // and waiting for a thread; more are turned away
constexpr time_t ioTimeoutSeconds{ 5 };
constexpr int unixSocketPort{ 80 };             // ignored for a Unix socket, but not 0

/** Is somebody listening on this socket? */
bool somebodyListens(const std::string& path)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return true;                            // cannot tell: do not remove it
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const bool listening = connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    const int error = errno;
    close(fd);
    return listening || (error != ECONNREFUSED);
}

} // namespace


struct HttpServer::Impl {
    Api& api;
    EventLog& events;
    const Log& log;
    httplib::Server server;
    std::thread thread;
    std::string path;
    std::atomic<bool> finished{ false };        // listen_after_bind() has returned
    bool started{ false };

    Impl(Api& a, EventLog& e, const Log& l) : api(a), events(e), log(l) {}

    void handle(const httplib::Request& request, httplib::Response& response) {
        ApiRequest apiRequest{ request.method, request.path, {}, request.body };
        for (const auto& [key, value] : request.params) {
            apiRequest.query.emplace(key, value);
        }
        const auto answer = api.handle(apiRequest);
        log.debug("{} {} -> {}", request.method, request.path, answer.status);

        response.status = answer.status;
        response.set_content(answer.body, "application/json");
        for (const auto& [name, value] : answer.headers) {
            response.set_header(name, value);
        }
    }
};


HttpServer::HttpServer(Api& api, EventLog& events, const Log& log) : impl_(std::make_unique<Impl>(api, events, log))
{
    auto& server = impl_->server;
    server.set_read_timeout(ioTimeoutSeconds);
    server.set_write_timeout(ioTimeoutSeconds);
    server.set_payload_max_length(Api::maxBody);
    server.new_task_queue = [] { return new httplib::ThreadPool(workerThreads, workerThreads, queuedConnections); };

    // One route for everything: the Api knows the paths and the methods, and says 404 and 405 in its own words. (A pre-routing
    // handler would be simpler, but it runs before the body of a PUT has been read.)
    auto handler = [this](const httplib::Request& request, httplib::Response& response) { impl_->handle(request, response); };
    server.Get(".*", handler);
    server.Put(".*", handler);
    server.Delete(".*", handler);
    server.Post(".*", handler);
    server.Patch(".*", handler);
    server.Options(".*", handler);
}

HttpServer::~HttpServer()
{
    stop();
}


std::string HttpServer::start(const std::string& socketPath, mode_t mode)
{
    auto& impl = *impl_;
    if (impl.started) {
        return "already started";
    }
    if (socketPath.empty() || (socketPath.front() == '@')) {
        return "the socket must be a path in the file system, not '" + socketPath + "'";
    }
    if (socketPath.find('\0') != std::string::npos || socketPath.size() >= sizeof(sockaddr_un::sun_path)) {
        return std::format("the socket path is too long (at most {} characters)", sizeof(sockaddr_un::sun_path) - 1);
    }

    // A socket that nobody listens on was left by a program that was killed. Anything else is not ours to remove.
    struct stat info{};
    if (lstat(socketPath.c_str(), &info) == 0) {
        if (!S_ISSOCK(info.st_mode)) {
            return socketPath + " exists and is not a socket";
        }
        if (somebodyListens(socketPath)) {
            return "somebody is listening on " + socketPath + " already";
        }
        if (unlink(socketPath.c_str()) != 0) {
            return std::format("cannot remove the old socket {}: {}", socketPath, std::strerror(errno));
        }
        impl.log.info("Removed the socket {}, which nobody listened on.", socketPath);
    }

    // Nobody may connect in the moment between creating the socket and setting its mode: create it closed, then open it as far
    // as asked. (The umask belongs to the whole process; we are still starting up.)
    impl.server.set_address_family(AF_UNIX);
    // (The port is not used for a Unix socket, but it must not be 0: that asks for "any free port", which the library then looks
    // up as if it were an internet address, and fails.)
    const mode_t oldMask = umask(0177);
    const bool bound = impl.server.bind_to_port(socketPath, unixSocketPort);
    const int bindError = errno;
    umask(oldMask);
    if (!bound) {
        unlink(socketPath.c_str());             // there is nothing of ours to keep: we removed the old one, or there was none
        return std::format("cannot create the socket {}: {}", socketPath, std::strerror(bindError));
    }
    if (chmod(socketPath.c_str(), mode) != 0) {
        const std::string why = std::strerror(errno);
        unlink(socketPath.c_str());
        return std::format("cannot set the mode of {}: {}", socketPath, why);
    }

    impl.path = socketPath;
    impl.finished = false;
    impl.thread = std::thread([&impl] {
        impl.server.listen_after_bind();
        impl.finished = true;
    });
    impl.started = true;

    // Wait until it listens (a moment), or has given up.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!impl.server.is_running() && !impl.finished && (std::chrono::steady_clock::now() < until)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!impl.server.is_running()) {
        stop();
        return "the server did not start listening on " + socketPath;
    }
    impl.log.info("Listening on {} (mode {:04o}).", socketPath, static_cast<unsigned>(mode));
    return {};
}


void HttpServer::stop()
{
    auto& impl = *impl_;
    if (!impl.started) {
        return;
    }
    impl.started = false;
    impl.events.shutdown();                     // clients that wait for events would hold up the stop
    impl.server.stop();
    if (impl.thread.joinable()) {
        impl.thread.join();
    }
    unlink(impl.path.c_str());
}

} // namespace nl::rakis::i2cbus
