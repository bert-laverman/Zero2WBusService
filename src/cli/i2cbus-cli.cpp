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

// A small command line program to try the core without a bus: the "bus" is a sink that prints what it is asked to send.
//
//   i2cbus-cli i2cbus.ini
//   > online pico-1
//   > set altitude 35000
//   > flush

#include <iostream>
#include <string>

#include "bus-core.hpp"
#include "commands.hpp"

using namespace nl::rakis::i2cbus;


class PrintingSink : public Sink {
public:
    bool show(const BoardConfig& board, unsigned index, const Content& content) override {
        std::cout << "  -> " << board.name << " (" << board.boardId << ") module " << index << " shows ";
        if (const auto* number = std::get_if<Number>(&content)) { std::cout << number->value; } else { std::cout << "nothing"; }
        std::cout << "\n";
        return true;
    }
    bool setBrightness(const BoardConfig& board, unsigned index, uint8_t level) override {
        std::cout << "  -> " << board.name << " module " << index << " brightness " << static_cast<unsigned>(level) << "\n";
        return true;
    }
};

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: i2cbus-cli <i2cbus.ini>\n";
        return 2;
    }
    try {
        auto config = Config::load(argv[1]);
        for (const auto& warning : config.warnings()) { std::cerr << "Warning: " << warning << "\n"; }
        PrintingSink sink;
        BusCore core(std::move(config), sink);

        std::cout << core.config().boards().size() << " board(s), " << core.config().displays().size() << " display(s). Type 'help'.\n";
        std::string line;
        while (std::cout << "> ", std::getline(std::cin, line)) {
            if (!runCommand(core, line, std::cout)) {
                break;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
