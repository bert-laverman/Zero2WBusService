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

// The service without the bus, to try the HTTP side on a PC: what the boards would be sent is printed, and boards come and go with
// the commands `online <board>` and `offline <board>` on the standard input (see `help`).
//
//   i2cbus-sim [--log-level LEVEL] --socket /tmp/i2cbus.sock i2cbus.ini
//   curl --unix-socket /tmp/i2cbus.sock http://localhost/displays

#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api.hpp"
#include "bus-core.hpp"
#include "commands.hpp"
#include "event-log.hpp"
#include "http-server.hpp"
#include "log.hpp"
#include "stdin-lines.hpp"

using namespace nl::rakis::i2cbus;


namespace {

volatile std::sig_atomic_t stopRequested{ 0 };
void onSignal(int) { stopRequested = 1; }

class PrintingSink : public Sink {
public:
    bool show(const BoardConfig& board, unsigned index, const Content& content) override {
        std::cout << "  -> " << board.name << " module " << index << " shows ";
        if (const auto* number = std::get_if<Number>(&content)) { std::cout << number->value; } else { std::cout << "nothing"; }
        std::cout << std::endl;
        return true;
    }
    bool setBrightness(const BoardConfig& board, unsigned index, uint8_t level) override {
        std::cout << "  -> " << board.name << " module " << index << " brightness " << static_cast<unsigned>(level) << std::endl;
        return true;
    }
};

} // namespace


int main(int argc, char* argv[])
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Log log;
    std::string socket;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        const std::string arg{ argv[i] };
        if ((arg == "--log-level") && (i + 1 < argc) && Log::parse(argv[i + 1])) {
            log.level(*Log::parse(argv[++i]));
        } else if ((arg == "--socket") && (i + 1 < argc)) {
            socket = argv[++i];
        } else if (arg.starts_with("--")) {
            std::cerr << "Usage: i2cbus-sim [--log-level LEVEL] --socket PATH config\n";
            return 2;
        } else {
            files.push_back(arg);
        }
    }
    if (socket.empty() || (files.size() != 1)) {
        std::cerr << "Usage: i2cbus-sim [--log-level LEVEL] --socket PATH config\n";
        return 2;
    }

    try {
        auto config = Config::load(files[0]);
        for (const auto& warning : config.warnings()) { log.warning("{}", warning); }

        PrintingSink sink;
        BusCore core(std::move(config), sink);
        std::mutex mutex;
        EventLog events;
        Api api(core, mutex, events);
        HttpServer http(api, events, log);
        if (const auto problem = http.start(socket); !problem.empty()) {
            log.error("{}", problem);
            return 1;
        }
        std::cout << "Simulating " << core.config().boards().size() << " board(s). Type 'help'; Ctrl-C stops." << std::endl;

        StdinLines input;
        std::string line;
        while (!stopRequested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::lock_guard lock(mutex);
            while (input.next(line)) {
                if (!runCommand(core, line, std::cout)) {
                    stopRequested = 1;
                }
                std::cout.flush();
            }
            core.flush();
            drainEvents(core, events);
        }
        log.info("Shutting down.");
    } catch (const std::exception& e) {
        log.error("{}", e.what());
        return 1;
    }
    return 0;
}
