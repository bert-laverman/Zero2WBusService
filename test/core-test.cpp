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

// A plain test program, no framework: a failed CHECK is reported, and the exit code says whether any failed.

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <filesystem>
#include <fstream>

#include "address-store.hpp"
#include "bus-core.hpp"
#include "commands.hpp"
#include "config.hpp"
#include "ini-file.hpp"

using namespace nl::rakis::i2cbus;


static int failures{ 0 };

#define CHECK(cond) do { if (!(cond)) { failures++; std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond "\n"; } } while (0)

template <typename Error, typename F>
static bool throws(F&& f, const std::string& contains)
{
    try {
        f();
    } catch (const Error& e) {
        const bool found = std::string(e.what()).find(contains) != std::string::npos;
        if (!found) { std::cerr << "  wrong message: " << e.what() << "\n"; }
        return found;
    }
    return false;
}


// The shape of the old i2c-state.ini, with the keys and values it has.
static const char* const goodIni = R"(
[device:max-1]
interface=spi-0
type=max7219

[display:1]
name=altitude
brightness=10
decodemode=255
device=max-1
index=0
scanlimit=7
state=on
type=number

[display:2]
name=vs
brightness=5
device=max-1
index=1

[display:3]
name=heading
device=max-1
index=2
colour=blue

[interface:spi-0]
modules=3
controller=pico-1
miso=16

[board:pico-1]
boardId=e6614104-031c5032
address=97
)";

static Config configFrom(const std::string& text)
{
    std::istringstream in(text);
    return Config::from(IniFile::parse(in));
}

static std::string replaced(std::string text, const std::string& from, const std::string& to)
{
    const auto at = text.find(from);
    if (at == std::string::npos) { std::cerr << "test bug: '" << from << "' not in the text\n"; failures++; return text; }
    return text.replace(at, from.size(), to);
}


struct RecordingSink : Sink {
    std::vector<std::string> log;
    bool failing{ false };
    unsigned brightnessFailed{ 0 };         // calls that failed, by kind
    unsigned showFailed{ 0 };

    bool show(const BoardConfig& board, unsigned index, const Content& content) override {
        if (failing) { showFailed++; return false; }
        const auto* number = std::get_if<Number>(&content);
        log.push_back("show " + board.name + "/" + std::to_string(index) + " " + (number ? std::to_string(number->value) : "blank"));
        return true;
    }
    bool setBrightness(const BoardConfig& board, unsigned index, uint8_t level) override {
        if (failing) { brightnessFailed++; return false; }
        log.push_back("bright " + board.name + "/" + std::to_string(index) + " " + std::to_string(level));
        return true;
    }
};

static Update number(int32_t value) { return Update{ Content{ Number{ value } }, std::nullopt }; }


static void testIni()
{
    std::istringstream in("# comment\n[Foo]\nKey = a value \n; also a comment\n[bar]\nx=1\n");
    auto ini = IniFile::parse(in);
    CHECK(ini.sections().at("foo").at("key") == "a value");     // lower case section and key, trimmed value
    CHECK(ini.sections().at("bar").at("x") == "1");

    auto bad = [](const std::string& text, const std::string& message) {
        std::istringstream s(text);
        return throws<IniError>([&] { IniFile::parse(s); }, message);
    };
    CHECK(bad("[open\n", "line 1"));
    CHECK(bad("key=1\n", "outside of a section"));
    CHECK(bad("[a]\nnonsense\n", "line 2"));
    CHECK(bad("[a]\nx=1\nX=2\n", "twice"));
    CHECK(throws<IniError>([] { IniFile::load("/nonexistent/i2cbus.ini"); }, "cannot open"));
}

static void testConfig()
{
    auto config = configFrom(goodIni);
    CHECK(config.boards().size() == 1);
    CHECK(config.displays().size() == 3);
    const auto* altitude = config.display("altitude");
    CHECK(altitude != nullptr);
    CHECK(altitude && altitude->board == "pico-1" && altitude->index == 0 && altitude->brightness == 10);
    CHECK(config.display("heading") && !config.display("heading")->brightness);
    CHECK(config.board("pico-1") && config.board("pico-1")->boardId == "e6614104-031c5032");
    CHECK(config.display("nothing") == nullptr);
    CHECK(config.boardById("e6614104-031c5032") == config.board("pico-1"));
    CHECK(config.boardById("e6614104-031c5033") == nullptr);

    // 'colour' is not something we use, but not worth stopping for; the old keys and the saved address are known.
    CHECK(config.warnings().size() == 1);
    CHECK(!config.warnings().empty() && config.warnings()[0].find("colour") != std::string::npos);

    auto bad = [](const std::string& text, const std::string& message) {
        return throws<ConfigError>([&] { configFrom(text); }, message);
    };
    CHECK(bad(replaced(goodIni, "name=vs", "name=altitude"), "used twice"));
    CHECK(bad(replaced(goodIni, "name=vs", "name=Not Valid"), "not a valid name"));
    CHECK(bad(replaced(goodIni, "index=1", "index=0"), "both module 0"));
    CHECK(bad(replaced(goodIni, "index=2", "index=3"), "only 3 module"));
    CHECK(bad(replaced(goodIni, "index=2", "index=two"), "must be a number"));
    CHECK(bad(replaced(goodIni, "brightness=10", "brightness=16"), "0 to 15"));
    CHECK(bad(replaced(goodIni, "controller=pico-1", "controller=pico-9"), "no [board:pico-9]"));
    CHECK(bad(replaced(goodIni, "interface=spi-0", "interface=spi-9"), "[interface:spi-9] is missing"));
    CHECK(bad(replaced(goodIni, "type=max7219", "type=ws2812"), "only max7219"));
    CHECK(bad(replaced(goodIni, "e6614104-031c5032", "e6614104031c5032"), "must look like"));
    CHECK(bad(replaced(goodIni, "name=vs\n", ""), "needs 'name'"));
    CHECK(bad(replaced(goodIni, "device=max-1\nindex=0", "device=max-2\nindex=0"), "[device:max-2] is missing"));
}

static void testBoardAddress()
{
    // The old files have 'address=97' in the board section: a wish for a board that the state file does not know. The id is
    // made lower case, because section names in the state file are.
    auto config = configFrom(replaced(goodIni, "e6614104-031c5032", "E6614104-031C5032"));
    CHECK(config.board("pico-1")->boardId == "e6614104-031c5032");
    CHECK(config.board("pico-1")->address == 97);
    CHECK(throws<ConfigError>([&] { configFrom(replaced(goodIni, "address=97", "address=5")); }, "from 97 to 119"));
    CHECK(throws<ConfigError>([&] { configFrom(replaced(goodIni, "address=97", "address=200")); }, "0 to 119"));
}

static void testAddressStore()
{
    const auto dir = std::filesystem::temp_directory_path() / "i2cbus-core-test";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "state.ini").string();
    std::filesystem::remove(path);

    auto store = AddressStore::load(path);                      // not there yet: empty
    CHECK(store.all().empty());
    CHECK(!store.address("e6614104-031c5032").has_value());
    CHECK(store.set("e6614104-031c5032", 97));
    CHECK(store.set("0a0b0c0d-01020304", 98));

    auto again = AddressStore::load(path);                      // and back from the file
    CHECK(again.all().size() == 2);
    CHECK(again.address("e6614104-031c5032") == 97);
    CHECK(again.address("0a0b0c0d-01020304") == 98);
    CHECK(!std::filesystem::exists(path + ".tmp"));             // renamed, not left behind

    {   // A file that makes no sense is an error, not an empty store: that would hand out the addresses again.
        std::ofstream(path) << "[board:not-an-id]\naddress = 1\n";
        CHECK(throws<IniError>([&] { AddressStore::load(path); }, "not a board id"));
        std::ofstream(path) << "[board:e6614104-031c5032]\naddress = 300\n";
        CHECK(throws<IniError>([&] { AddressStore::load(path); }, "needs an 'address'"));
        std::ofstream(path) << "[board:e6614104-031c5032]\n";
        CHECK(throws<IniError>([&] { AddressStore::load(path); }, "needs an 'address'"));
    }
    std::filesystem::remove_all(dir);

    AddressStore memory;                                        // no file: for tests that do not need one
    CHECK(memory.set("e6614104-031c5032", 97));
    CHECK(memory.address("e6614104-031c5032") == 97);

    AddressStore unwritable = AddressStore::load("/nonexistent-dir/state.ini");
    CHECK(!unwritable.set("e6614104-031c5032", 97));            // the caller hears that it was not saved
}

static void testCommands()
{
    RecordingSink sink;
    BusCore core(configFrom(goodIni), sink);
    auto run = [&](const std::string& line) {
        std::ostringstream out;
        const bool more = runCommand(core, line, out);
        return std::pair{ more, out.str() };
    };

    CHECK(run("set altitude 123").second == "  ok\n");
    CHECK(run("set altitude blank").second == "  ok\n");
    CHECK(run("set altitude abc").second.find("not a number") != std::string::npos);
    CHECK(run("set nope 1").second == "  unknown display\n");
    CHECK(run("set altitude 100000000").second == "  value out of range\n");
    CHECK(run("brightness vs 16").second.find("0 to 15") != std::string::npos);
    CHECK(run("brightness vs 4").second == "  ok\n");
    CHECK(core.state("vs")->brightness == 4);
    CHECK(run("bogus").second.find("unknown command") != std::string::npos);
    CHECK(run("").first);
    CHECK(!run("quit").first);

    run("online pico-1");
    run("flush");
    CHECK(run("show").second.find("altitude (pico-1, module 0, online): blank") != std::string::npos);
    CHECK(run("events").second == "  online pico-1\n");
}

static void testMergingAndResend()
{
    RecordingSink sink;
    BusCore core(configFrom(goodIni), sink);

    // Nothing goes out for a board that is not there, but the state is kept.
    CHECK(core.update("altitude", number(100)) == Status::Ok);
    core.flush();
    CHECK(sink.log.empty());
    CHECK(core.state("altitude")->content == Content{ Number{ 100 } });

    // The board appears: brightness from the configuration, then the number.
    core.boardOnline("pico-1");
    core.flush();
    CHECK((sink.log == std::vector<std::string>{ "bright pico-1/0 10", "show pico-1/0 100", "bright pico-1/1 5" }));

    // Many updates between two flushes: one goes out, the last.
    sink.log.clear();
    for (int i = 1; i <= 50; i++) { core.update("altitude", number(35000 + i)); }
    core.flush();
    CHECK((sink.log == std::vector<std::string>{ "show pico-1/0 35050" }));

    // The same value again is not sent again, nor is a flush with nothing to do.
    sink.log.clear();
    CHECK(core.update("altitude", number(35050)) == Status::Ok);
    core.flush();
    core.flush();
    CHECK(sink.log.empty());

    // The Pico restarts: it comes back online, and everything is sent again. 'heading' has nothing to show, so nothing goes out.
    core.boardOffline("pico-1");
    core.update("altitude", Update{ Content{ Blank{} }, std::nullopt });
    core.flush();
    CHECK(sink.log.empty());                    // offline: held back
    core.boardOnline("pico-1");
    core.flush();
    CHECK((sink.log == std::vector<std::string>{ "bright pico-1/0 10", "show pico-1/0 blank", "bright pico-1/1 5" }));

    // Online again without us noticing it was gone (a second Hello): sent again too.
    sink.log.clear();
    core.boardOnline("pico-1");
    core.flush();
    CHECK(sink.log.size() == 3);
}

static void testValidationAndRetry()
{
    RecordingSink sink;
    BusCore core(configFrom(goodIni), sink);
    core.boardOnline("pico-1");
    core.flush();
    sink.log.clear();

    CHECK(core.update("nope", number(1)) == Status::UnknownDisplay);
    CHECK(core.update("altitude", Update{}) == Status::NothingToDo);
    CHECK(core.update("altitude", number(100'000'000)) == Status::OutOfRange);
    CHECK(core.update("altitude", number(-10'000'000)) == Status::OutOfRange);
    CHECK(core.update("altitude", number(99'999'999)) == Status::Ok);
    CHECK(core.update("altitude", number(-9'999'999)) == Status::Ok);

    // An update is applied as a whole: a bad brightness keeps the good number out as well.
    CHECK(core.update("vs", Update{ Content{ Number{ 7 } }, uint8_t{ 16 } }) == Status::OutOfRange);
    CHECK(!core.state("vs")->content.has_value());
    CHECK(core.state("vs")->brightness == 5);

    // A message that does not go out stays waiting.
    sink.log.clear();
    sink.failing = true;
    core.update("heading", number(270));
    const auto t0 = BusCore::Clock::now();
    core.flush(t0);
    CHECK(sink.log.empty());
    CHECK(sink.showFailed == 2);                                // 'altitude' still had a number waiting, too
    sink.failing = false;
    core.flush(t0 + std::chrono::milliseconds(100));            // too soon: left alone, not even tried
    CHECK(sink.showFailed == 2);
    CHECK(sink.log.empty());
    core.flush(t0 + std::chrono::milliseconds(600));
    CHECK((std::ranges::find(sink.log, "show pico-1/2 270") != sink.log.end()));

    // A board that appears again is tried at once, whatever went wrong before.
    sink.failing = true;
    core.update("heading", number(271));
    core.flush(t0 + std::chrono::seconds(10));
    sink.failing = false;
    sink.log.clear();
    core.boardOnline("pico-1");
    core.flush(t0 + std::chrono::seconds(10));
    CHECK((std::ranges::find(sink.log, "show pico-1/2 271") != sink.log.end()));
}

static void testBrightnessBeforeNumber()
{
    RecordingSink sink;
    BusCore core(configFrom(goodIni), sink);
    core.update("vs", Update{ Content{ Number{ 7 } }, uint8_t{ 9 } });
    core.boardOnline("pico-1");
    sink.failing = true;
    const auto t0 = BusCore::Clock::now();
    core.flush(t0);
    CHECK(sink.showFailed == 0);                                // never tried: it would show at the wrong strength
    CHECK(sink.brightnessFailed == 2);                          // 'altitude' and 'vs' have a brightness waiting
    sink.failing = false;
    core.flush(t0 + std::chrono::seconds(1));
    CHECK((std::ranges::find(sink.log, "bright pico-1/1 9") < std::ranges::find(sink.log, "show pico-1/1 7")));
}

static void testEvents()
{
    RecordingSink sink;
    BusCore core(configFrom(goodIni), sink);
    core.boardOnline("unknown-board");              // not ours: ignored
    core.boardOffline("pico-1");                    // was never online: no event
    CHECK(core.takeEvents().empty());

    core.boardOnline("pico-1");
    CHECK(core.online("pico-1"));
    core.boardOffline("pico-1");
    CHECK(!core.online("pico-1"));
    auto events = core.takeEvents();
    CHECK(events.size() == 2);
    CHECK(events.size() == 2 && events[0].type == Event::Type::BoardOnline && events[1].type == Event::Type::BoardOffline);
    CHECK(events.size() == 2 && events[1].board == "pico-1");
    CHECK(core.takeEvents().empty());
}


int main()
{
    testIni();
    testConfig();
    testBoardAddress();
    testAddressStore();
    testCommands();
    testMergingAndResend();
    testValidationAndRetry();
    testBrightnessBeforeNumber();
    testEvents();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All checks passed.\n";
    return 0;
}
