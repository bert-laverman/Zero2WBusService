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

// The HTTP side with a real server on a real Unix socket, and a client of the same library: the sockets (who may connect, what is
// left behind, a second instance), the limits, and stopping with clients waiting. The meaning of the requests is tested in
// core-test; here only that they arrive.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "httplib.h"
#include "json.hpp"

#include "api.hpp"
#include "bus-core.hpp"
#include "config.hpp"
#include "event-log.hpp"
#include "http-server.hpp"
#include "ini-file.hpp"
#include "log.hpp"

using namespace nl::rakis::i2cbus;
using json = nlohmann::json;


static int failures{ 0 };
static int checks{ 0 };

// Variadic, so that a comma in the condition is not taken for a second argument.
#define CHECK(...) do { checks++; if (!(__VA_ARGS__)) { failures++; std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #__VA_ARGS__ "\n"; } } while (0)


static const char* const iniText = R"(
[board:pico-1]
boardid = e6614104-031c5032
[interface:spi-0]
modules = 2
controller = pico-1
[device:max-1]
interface = spi-0
type = max7219
[display:1]
name = altitude
device = max-1
index = 0
[display:2]
name = vs
device = max-1
index = 1
)";

struct NullSink : Sink {
    bool show(const BoardConfig&, unsigned, const Content&) override { return true; }
    bool setBrightness(const BoardConfig&, unsigned, uint8_t) override { return true; }
};

struct Service {
    NullSink sink;
    std::mutex mutex;
    EventLog events;
    BusCore core;
    Api api{ core, mutex, events };
    Log log;
    HttpServer http{ api, events, log };

    static Config config() {
        std::istringstream in(iniText);
        return Config::from(IniFile::parse(in));
    }
    Service() : core(config(), sink) { log.level(Log::Level::Error); }
};

static httplib::Client clientFor(const std::string& socket)
{
    httplib::Client client(socket);
    client.set_address_family(AF_UNIX);
    client.set_read_timeout(10, 0);
    client.set_connection_timeout(2, 0);
    return client;
}

static mode_t modeOf(const std::string& path)
{
    struct stat info{};
    return (lstat(path.c_str(), &info) == 0) ? (info.st_mode & 07777) : 0;
}

static bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }


int main()
{
    char pattern[] = "/tmp/i2cbus-http-test-XXXXXX";       // short: the path of a socket has at most 107 characters
    if (mkdtemp(pattern) == nullptr) {
        std::cerr << "no temporary directory\n";
        return 2;
    }
    const std::string dir{ pattern };
    const std::string socket = dir + "/i2cbus.sock";

    {   // Start: the socket is there, closed to everybody but its owner and group, and it answers.
        Service s;
        CHECK(s.http.start(socket).empty());
        CHECK(modeOf(socket) == 0660);

        auto client = clientFor(socket);
        auto list = client.Get("/displays");
        CHECK(list && list->status == 200);
        CHECK(list && json::parse(list->body)["displays"].size() == 2);

        auto put = client.Put("/displays/altitude", R"({"value": 35000})", "application/json");
        CHECK(put && put->status == 200 && json::parse(put->body)["content"]["value"] == 35000);
        CHECK(s.core.state("altitude")->content == Content{ Number{ 35000 } });      // it got to the core, with the body
        CHECK(client.Put("/displays/altitude", R"({"value": "x"})", "application/json")->status == 400);
        CHECK(client.Get("/displays/nope")->status == 404);
        CHECK(client.Post("/displays/altitude", "{}", "application/json")->status == 405);
        CHECK(client.Delete("/displays/altitude")->status == 200);
        auto events = client.Get("/events?wait=0");
        CHECK(events && events->status == 200 && json::parse(events->body)["events"].is_array());

        // A body that is too long is refused on the way in, before the API sees it.
        auto big = client.Put("/displays/altitude", std::string(Api::maxBody * 4, ' '), "application/json");
        CHECK(!big || big->status == 413);
        CHECK(s.core.state("altitude")->content == Content{ Blank{} });

        // A second instance does not take the socket from the first, and does not break it.
        Service second;
        const auto problem = second.http.start(socket);
        CHECK(contains(problem, "listening"));
        CHECK(clientFor(socket).Get("/boards")->status == 200);

        s.http.stop();
        CHECK(!std::filesystem::exists(socket));            // taken away again
        s.http.stop();                                      // and stopping twice is harmless
    }

    {   // Starting again where a program left its socket (it was killed): nobody listens, so it is replaced.
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket.c_str());
        CHECK(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        close(fd);                                          // the file stays, nobody listens
        CHECK(std::filesystem::exists(socket));

        Service s;
        CHECK(s.http.start(socket).empty());
        CHECK(clientFor(socket).Get("/displays")->status == 200);
        s.http.stop();
    }

    {   // What is not a socket is not ours to remove; and a socket without a file is refused.
        std::ofstream(socket) << "important";
        Service s;
        CHECK(contains(s.http.start(socket), "not a socket"));
        std::ifstream in(socket);
        std::string text;
        in >> text;
        CHECK(text == "important");
        std::filesystem::remove(socket);

        CHECK(contains(s.http.start("@abstract"), "path in the file system"));
        CHECK(contains(s.http.start(""), "path in the file system"));
        CHECK(contains(s.http.start(dir + "/" + std::string(200, 'x')), "too long"));
        CHECK(contains(s.http.start(dir + "/no-such-dir/i2cbus.sock"), "cannot create"));
        CHECK(!std::filesystem::exists(dir + "/no-such-dir"));
    }

    {   // Another mode, if asked.
        Service s;
        CHECK(s.http.start(socket, 0600).empty());
        CHECK(modeOf(socket) == 0600);
        s.http.stop();
    }

    {   // Several clients at once: all of them are answered, and the state is in one piece.
        Service s;
        CHECK(s.http.start(socket).empty());
        std::atomic<int> good{ 0 };
        std::vector<std::thread> clients;
        for (int c = 0; c < 4; c++) {
            clients.emplace_back([&, c] {
                auto client = clientFor(socket);
                for (int i = 0; i < 50; i++) {
                    auto answer = client.Put("/displays/vs", "{\"value\": " + std::to_string(c * 1000 + i) + "}", "application/json");
                    if (answer && answer->status == 200) { good++; }
                }
            });
        }
        for (auto& t : clients) { t.join(); }
        CHECK(good == 200);
        const auto last = std::get<Number>(*s.core.state("vs")->content).value;
        CHECK((last % 1000) == 49);                         // the last of one client's 50 is the last of all
        s.http.stop();
    }

    {   // Stopping lets a client that waits for events go at once, instead of at the end of its wait.
        Service s;
        CHECK(s.http.start(socket).empty());
        std::atomic<int> status{ 0 };
        const auto before = std::chrono::steady_clock::now();
        std::thread waiter([&] {
            auto client = clientFor(socket);
            auto answer = client.Get("/events?after=99999999999999&wait=30");
            status = answer ? answer->status : -1;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        s.http.stop();
        waiter.join();
        CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(5));
        CHECK(status == 200 || status == -1);               // an empty answer, or a connection that closed with the server
    }

    std::filesystem::remove_all(dir);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All " << checks << " checks passed.\n";
    return 0;
}
