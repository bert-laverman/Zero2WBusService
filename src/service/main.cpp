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

// The service: it owns the I2C bus, keeps the displays of the boards up to date, and (until there is an HTTP layer) takes the
// commands of `commands.hpp` on its standard input.
//
//   i2cbus [--log-level error|warning|info|debug] [config [state]]
//
// The configuration is `i2cbus.ini`, and the state file is where the addresses of the boards are kept. The log (with a time and
// a level on every line) goes to standard error, the answers to commands to standard output: `2>>i2cbus.log` keeps them apart.
// The level can also be set with the environment variable I2CBUS_LOG. The default is info; debug shows every Hello of every board.

#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string_view>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <zero2w.hpp>
#include <interfaces/pigpiod-i2c.hpp>
#include <interfaces/i2cdev-i2c.hpp>
#include <protocols/i2c-protocol-driver.hpp>
#include <protocols/i2c-bus-controller.hpp>
#include <util/message-queue.hpp>

#include "address-store.hpp"
#include "board-id.hpp"
#include "bus-core.hpp"
#include "bus-sink.hpp"
#include "commands.hpp"
#include "log.hpp"

using namespace nl::rakis::i2cbus;
namespace pi = nl::rakis::raspberrypi;


#if !defined(HAVE_I2C)
#error "The service needs I2C enabled"
#endif


namespace {

using Driver = pi::protocols::I2CProtocolDriver<pi::util::MessageQueue>;

constexpr uint8_t controllerAddress{ 0x0a };    // where boards send their Hello and their messages to
constexpr uint8_t firstBoardAddress{ 0x61 };    // the first address we hand out
constexpr uint8_t lastBoardAddress{ 0x77 };
constexpr auto tickInterval = std::chrono::milliseconds(10);


/**
 * Stop on Ctrl-C or a kill: leave the loop, so that the BSC slave is switched off on the way out. A program that is simply
 * killed in the middle of a transfer leaves the BSC slave holding SCL low, and then nobody can use the bus.
 */
volatile std::sig_atomic_t stopRequested{ 0 };
void onSignal(int) { stopRequested = 1; }


/**
 * The addresses we know of: those in the state file, and for a board of the configuration that is not in there, the address the
 * configuration wishes for. Two boards that want the same address are a mistake we do not pass on to the bus.
 */
std::map<std::string, uint8_t> knownAddresses(const Config& config, const AddressStore& store, const Log& log)
{
    std::map<std::string, uint8_t> known = store.all();
    for (const auto& board : config.boards()) {
        if (board.address && !known.contains(board.boardId)) {
            known[board.boardId] = *board.address;
        }
    }

    std::map<std::string, uint8_t> usable;
    std::map<uint8_t, std::string> owner;
    for (const auto& [boardId, address] : known) {
        if ((address < firstBoardAddress) || (address > lastBoardAddress)) {
            log.warning("Ignoring address 0x{:02x} of board {}: not from 0x{:02x} to 0x{:02x}.", address, boardId,
                        firstBoardAddress, lastBoardAddress);
        } else if (auto other = owner.find(address); other != owner.end()) {
            log.warning("Ignoring address 0x{:02x} of board {}: board {} has it.", address, boardId, other->second);
        } else {
            owner[address] = boardId;
            usable[boardId] = address;
        }
    }
    return usable;
}


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

} // namespace


int main(int argc, char* argv[])
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Log log;
    std::vector<std::string> files;
    auto chooseLevel = [&log](std::string_view text) {
        const auto level = Log::parse(text);
        if (level) {
            log.level(*level);
        }
        return level.has_value();
    };
    if (const char* fromEnvironment = std::getenv("I2CBUS_LOG"); (fromEnvironment != nullptr) && !chooseLevel(fromEnvironment)) {
        std::cerr << "I2CBUS_LOG must be error, warning, info or debug, not '" << fromEnvironment << "'.\n";
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        const std::string_view arg{ argv[i] };
        if (arg == "--log-level") {
            if ((i + 1 >= argc) || !chooseLevel(argv[++i])) {
                std::cerr << "--log-level must be followed by error, warning, info or debug.\n";
                return 2;
            }
        } else if (arg.starts_with("--")) {
            std::cerr << "Unknown option '" << arg << "'. Usage: i2cbus [--log-level LEVEL] [config [state]]\n";
            return 2;
        } else {
            files.emplace_back(arg);
        }
    }
    if (files.size() > 2) {
        std::cerr << "Usage: i2cbus [--log-level LEVEL] [config [state]]\n";
        return 2;
    }
    const std::string configFile{ (files.size() >= 1) ? files[0] : "/etc/i2cbus/i2cbus.ini" };
    const std::string stateFile{ (files.size() >= 2) ? files[1] : "/var/lib/i2cbus/i2cbus-state.ini" };

    try {
        auto config = Config::load(configFile);
        for (const auto& warning : config.warnings()) {
            log.warning("{}", warning);
        }
        auto store = AddressStore::load(stateFile);
        const auto addresses = knownAddresses(config, store, log);

        auto pi2picoBus = std::make_shared<pi::interfaces::I2CDevI2C>("/dev/i2c-1");
        auto pico2piBus = std::make_shared<pi::interfaces::PigpiodBSCI2C>();

        Driver driver;
        driver.addInterface(pi2picoBus);
        driver.addInterface(pico2piBus);

        BusSink<Driver> sink(driver);
        BusCore core(std::move(config), sink);

        // The bus controller hands out the addresses; we keep them, so a board has the same one after a restart.
        pi::protocols::I2CBusController controller(driver);
        controller.logger([&log](const std::string& line) { log.controller(line); });
        controller.addressRange(firstBoardAddress, lastBoardAddress);
        for (const auto& [boardId, address] : addresses) {
            controller.addKnown(parseBoardId(boardId), address);
        }
        controller.onConfirmed([&store, &log](const pi::protocols::BoardId& id, uint8_t address) {
            if (store.set(boardIdString(id), address)) {
                log.info("Saved address 0x{:02x} of board {}.", address, boardIdString(id));
            } else {
                log.error("Cannot save the address 0x{:02x} of board {}: it will not survive a restart.", address, boardIdString(id));
            }
        });

        // A board that appears has lost whatever it showed (also one that restarted before we noticed it was gone): the core
        // sends everything again. A board that is not in the configuration is not ours to show anything on.
        controller.onBoardAppeared([&core, &sink, &log](const pi::protocols::BoardId& id, uint8_t address) {
            const auto boardId = boardIdString(id);
            const auto* board = core.config().boardById(boardId);
            if (board == nullptr) {
                log.warning("Board {} on address 0x{:02x} appeared, but is not in the configuration.", boardId, address);
                return;
            }
            log.info("Board '{}' ({}) appeared on address 0x{:02x}.", board->name, boardId, address);
            sink.attach(board->name, address);
            core.boardOnline(board->name);
        });
        controller.onBoardGone([&core, &sink, &log](const pi::protocols::BoardId& id, uint8_t address) {
            if (auto name = sink.detach(address)) {
                log.info("Board '{}' ({}) on address 0x{:02x} is gone.", *name, boardIdString(id), address);
                core.boardOffline(*name);
            }
        });
        controller.registerHandlers();

        driver.listenAddress(controllerAddress);
        driver.startListening();
        log.info("{} board(s) and {} display(s) in {}, {} address(es) known. Listening (log level {}).",
                 core.config().boards().size(), core.config().displays().size(), configFile, addresses.size(),
                 log.levelName());

        StdinLines input;
        std::string line;
        while (!stopRequested) {
            controller.tick();                              // says Hello once per second, repeats addresses, notices boards gone
            std::this_thread::sleep_for(tickInterval);
            driver.processIncoming();

            while (input.next(line)) {
                log.debug("Command: {}", line);
                if (!runCommand(core, line, std::cout)) {
                    stopRequested = 1;
                }
                std::cout.flush();
            }
            core.flush();
        }

        log.info("Shutting down.");
        driver.stopListening();
        driver.close();
    } catch (const std::exception& e) {
        log.error("{}", e.what());
        return 1;
    }
    return 0;
}
