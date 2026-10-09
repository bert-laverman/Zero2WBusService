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

#include <algorithm>
#include <filesystem>
#include <functional>
#include <thread>
#include <fstream>

#include "api.hpp"
#include "bus-core.hpp"
#include "commands.hpp"
#include "event-log.hpp"
#include "log.hpp"
#include "json.hpp"
#include "state-store.hpp"
#include "config.hpp"
#include "ini-file.hpp"

using namespace nl::rakis::i2cbus;


static int failures{ 0 };
static int checks{ 0 };

// Variadic, so that a comma in the condition (an initializer list, a template argument) is not taken for a second argument.
#define CHECK(...) do { checks++; if (!(__VA_ARGS__)) { failures++; std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #__VA_ARGS__ "\n"; } } while (0)

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

static void testStateStoreAddresses()
{
    const auto dir = std::filesystem::temp_directory_path() / "i2cbus-core-test";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "state.ini").string();
    std::filesystem::remove(path);

    auto store = StateStore::load(path);                      // not there yet: empty
    CHECK(store.addresses().empty());
    CHECK(!store.address("e6614104-031c5032").has_value());
    CHECK(store.setAddress("e6614104-031c5032", 97));
    CHECK(store.setAddress("0a0b0c0d-01020304", 98));

    auto again = StateStore::load(path);                      // and back from the file
    CHECK(again.addresses().size() == 2);
    CHECK(again.address("e6614104-031c5032") == 97);
    CHECK(again.address("0a0b0c0d-01020304") == 98);
    CHECK(!std::filesystem::exists(path + ".tmp"));             // renamed, not left behind

    {   // A file that makes no sense is an error, not an empty store: that would hand out the addresses again.
        std::ofstream(path) << "[board:not-an-id]\naddress = 1\n";
        CHECK(throws<IniError>([&] { StateStore::load(path); }, "not a board id"));
        std::ofstream(path) << "[board:e6614104-031c5032]\naddress = 300\n";
        CHECK(throws<IniError>([&] { StateStore::load(path); }, "needs an 'address'"));
        std::ofstream(path) << "[board:e6614104-031c5032]\n";
        CHECK(throws<IniError>([&] { StateStore::load(path); }, "needs an 'address'"));
    }
    std::filesystem::remove_all(dir);

    StateStore memory;                                        // no file: for tests that do not need one
    CHECK(memory.setAddress("e6614104-031c5032", 97));
    CHECK(memory.address("e6614104-031c5032") == 97);

    StateStore unwritable = StateStore::load("/nonexistent-dir/state.ini");
    CHECK(!unwritable.setAddress("e6614104-031c5032", 97));            // the caller hears that it was not saved
}

static std::string fileText(const std::string& path)
{
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void testStateStoreDisplays()
{
    const auto dir = std::filesystem::temp_directory_path() / "i2cbus-core-test-displays";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "state.ini").string();
    std::filesystem::remove(path);

    auto store = StateStore::load(path);
    store.setAddress("e6614104-031c5032", 97);
    store.setDisplay("altitude", DisplayState{ Content{ Number{ -250 } }, std::nullopt });
    store.setDisplay("vs", DisplayState{ Content{ Blank{} }, uint8_t{ 9 } });
    store.setDisplay("heading", DisplayState{ std::nullopt, uint8_t{ 2 } });
    CHECK(store.dirty());                                       // the displays are noted, not written yet
    CHECK(fileText(path).find("display:") == std::string::npos);
    CHECK(store.save());
    CHECK(!store.dirty());

    auto again = StateStore::load(path);
    CHECK(again.addresses().size() == 1);
    CHECK(again.displays().size() == 3);
    CHECK(again.displays().at("altitude").content == Content{ Number{ -250 } });
    CHECK(!again.displays().at("altitude").brightness.has_value());
    CHECK(again.displays().at("vs").content == Content{ Blank{} });
    CHECK(again.displays().at("vs").brightness == 9);
    CHECK(!again.displays().at("heading").content.has_value());          // brightness alone is worth keeping

    // Nothing changed: nothing to write. A display with nothing left to remember goes out of the file.
    store.setDisplay("altitude", DisplayState{ Content{ Number{ -250 } }, std::nullopt });
    CHECK(!store.dirty());
    store.setDisplay("heading", DisplayState{});
    CHECK(store.dirty());
    CHECK(store.save());
    CHECK(StateStore::load(path).displays().size() == 2);

    // This file is ours, so what we do not understand is an error and not something to skip.
    auto bad = [&](const std::string& text, const std::string& message) {
        std::ofstream(path) << text;
        return throws<IniError>([&] { StateStore::load(path); }, message);
    };
    CHECK(bad("[display:altitude]\ncontent = lots\n", "must be a number or 'blank'"));
    CHECK(bad("[display:altitude]\ncontent = 99999999999\n", "must be a number or 'blank'"));
    CHECK(bad("[display:altitude]\nbrightness = 16\n", "0 to 15"));
    CHECK(bad("[display:altitude]\ncolour = red\n", "which we do not know"));
    CHECK(bad("[display:Not Valid]\ncontent = 1\n", "not a display name"));
    CHECK(bad("[something]\nx = 1\n", "not a [board:...] or [display:...]"));
    std::filesystem::remove_all(dir);
}

static void testRestoreAndSnapshot()
{
    RecordingSink sink;
    BusCore core(configFrom(goodIni), sink);
    CHECK(core.revision() == 0);

    // Saved by an earlier run: a number, a brightness of a client, a display that is gone, and a number that no longer fits.
    StateStore store;
    store.setDisplay("altitude", DisplayState{ Content{ Number{ 35000 } }, uint8_t{ 8 } });
    store.setDisplay("vs", DisplayState{ Content{ Blank{} }, std::nullopt });
    store.setDisplay("removed", DisplayState{ Content{ Number{ 1 } }, std::nullopt });
    store.setDisplay("heading", DisplayState{ Content{ Number{ 100'000'000 } }, std::nullopt });
    const auto failed = restoreDisplays(core, store);
    CHECK(failed.size() == 2);
    using Failure = std::pair<std::string, Status>;
    CHECK(failed.size() == 2 && failed[0] == Failure("heading", Status::OutOfRange));
    CHECK(failed.size() == 2 && failed[1] == Failure("removed", Status::UnknownDisplay));
    CHECK(core.state("altitude")->content == Content{ Number{ 35000 } });
    CHECK(core.state("altitude")->brightness == 8);                      // the client's choice beats the configuration's
    CHECK(core.state("vs")->brightness == 5);                            // nothing saved: the configuration's
    CHECK(!core.state("heading")->content.has_value());
    CHECK(core.revision() > 0);

    // Nothing goes out before the board is there, and everything does when it is. 'heading' and 'vs' were never given a
    // number (or were blanked): the board is not blanked behind our back.
    core.flush();
    CHECK(sink.log.empty());
    core.boardOnline("pico-1");
    core.flush();
    CHECK((std::ranges::find(sink.log, "show pico-1/0 35000") != sink.log.end()));
    CHECK((std::ranges::find(sink.log, "bright pico-1/0 8") != sink.log.end()));
    CHECK((std::ranges::find(sink.log, "show pico-1/1 blank") != sink.log.end()));
    CHECK(std::none_of(sink.log.begin(), sink.log.end(), [](const std::string& l) { return l.starts_with("show pico-1/2"); }));

    // And back: a brightness that is the configured one is not kept, one that differs is. A revision only moves on a change.
    StateStore after;
    snapshotDisplays(core, after);
    CHECK(after.displays().at("altitude").content == Content{ Number{ 35000 } });
    CHECK(after.displays().at("altitude").brightness == 8);
    CHECK(after.displays().at("vs").content == Content{ Blank{} });
    CHECK(!after.displays().at("vs").brightness.has_value());
    CHECK(after.displays().count("heading") == 0);                       // nothing to remember

    const auto revision = core.revision();
    core.update("altitude", number(35000));                              // the same again
    core.update("nope", number(1));                                      // refused
    CHECK(core.revision() == revision);
    core.update("altitude", number(35001));
    CHECK(core.revision() == revision + 1);
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
    CHECK(run("\xC2\xA0set heading 5").second == "  ok\n");        // a non-breaking space from a copied command
    CHECK(run("se\xC2\xA7t x 1").second == "  unknown command 'se\\xc2\\xa7t'; try 'help'\n");    // and an odd character is shown
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

static std::string logged(const Log& log, const std::function<void(const Log&)>& what)
{
    std::ostringstream captured;
    auto* old = std::cerr.rdbuf(captured.rdbuf());
    what(log);
    std::cerr.rdbuf(old);
    return captured.str();
}

static void testLog()
{
    CHECK(Log::parse("debug") == Log::Level::Debug);
    CHECK(!Log::parse("verbose").has_value());

    // The bus controller has no levels; its lines are sorted by their first character. The default level (info) is quiet
    // about the Hello of every board, every few seconds, and says what happened.
    Log log;
    auto fromController = [](const std::string& line) { return [line](const Log& l) { l.controller(line); }; };
    CHECK(logged(log, fromController("Received Hello message from 0x61, board with Id e6614104-031a8938\n")).empty());
    CHECK(logged(log, fromController("- Board announced itself on address 0x61.\n")).find("INFO  - Board announced") != std::string::npos);
    CHECK(logged(log, fromController("* No free address left.\n")).find("WARN  * No free address left.") != std::string::npos);

    log.level(Log::Level::Debug);
    CHECK(logged(log, fromController("Received Hello message from 0x61, board with Id x\n")).find("DEBUG Received Hello") != std::string::npos);

    // One line per message, however many newlines it came with; a time in front, 'HH:MM:SS '.
    const auto line = logged(log, [](const Log& l) { l.info("{} board(s)", 2); });
    CHECK(line.size() > 9 && line[2] == ':' && line[5] == ':' && line[8] == ' ');
    CHECK(line.substr(9) == "INFO  2 board(s)\n");

    log.level(Log::Level::Error);
    CHECK(logged(log, [](const Log& l) { l.warning("nobody hears this"); }).empty());
    CHECK(logged(log, [](const Log& l) { l.error("this is heard"); }).find("ERROR this is heard") != std::string::npos);
}

struct ApiFixture {
    RecordingSink sink;
    BusCore core{ configFrom(goodIni), sink };
    std::mutex mutex;
    EventLog events;
    Api api{ core, mutex, events };

    struct Answer {
        int status;
        nlohmann::json body;
        std::string raw;
        std::vector<std::pair<std::string, std::string>> headers;
    };

    Answer call(const std::string& method, const std::string& path, const std::string& body = "",
                std::map<std::string, std::string> query = {}) {
        const auto response = api.handle(ApiRequest{ method, path, std::move(query), body });
        return Answer{ response.status, nlohmann::json::parse(response.body, nullptr, false), response.body, response.headers };
    }
};

static void testApi()
{
    ApiFixture f;
    using json = nlohmann::json;

    // What is there.
    auto list = f.call("GET", "/displays");
    CHECK(list.status == 200);
    CHECK(list.body["displays"].size() == 3);
    CHECK(list.body["displays"][0]["name"] == "altitude");
    CHECK(list.body["displays"][0]["content"].is_null());
    CHECK(list.body["displays"][0]["brightness"] == 10);
    CHECK(list.body["displays"][0]["online"] == false);
    auto one = f.call("GET", "/displays/vs");
    CHECK(one.status == 200 && one.body["board"] == "pico-1" && one.body["module"] == 1);
    CHECK(f.call("GET", "/displays/nope").status == 404);
    CHECK(f.call("GET", "/displays/Not Valid").status == 404);          // not even a name
    CHECK(f.call("GET", "/displays/altitude/x").status == 404);
    CHECK(f.call("GET", "/displays/").status == 404);
    CHECK(f.call("GET", "/nothing").status == 404);
    CHECK(f.call("GET", "/boards").body["boards"][0]["boardId"] == "e6614104-031c5032");

    // Show a number, with a brightness, in one request.
    auto put = f.call("PUT", "/displays/altitude", R"({"value": 35000, "brightness": 3})");
    CHECK(put.status == 200);
    CHECK((put.body["content"] == json{ { "type", "number" }, { "value", 35000 } }));
    CHECK(put.body["brightness"] == 3);
    CHECK(f.call("PUT", "/displays/altitude", R"({"brightness": 4})").body["content"]["value"] == 35000);   // only what is given
    CHECK(f.call("PUT", "/displays/altitude", R"({"value": -250})").body["brightness"] == 4);
    CHECK(f.core.state("altitude")->content == Content{ Number{ -250 } });
    CHECK(f.call("DELETE", "/displays/altitude").body["content"] == json{ { "type", "blank" } });
    CHECK(f.core.state("altitude")->content == Content{ Blank{} });

    // Everything is checked, and a request that is refused changes nothing.
    f.core.update("vs", number(7));
    struct Bad { const char* body; int status; const char* message; };
    const Bad bad[] = {
        { "", 400, "not valid JSON" },
        { "{", 400, "not valid JSON" },
        { "[1]", 400, "must be a JSON object" },
        { "35000", 400, "must be a JSON object" },
        { "{}", 400, "nothing to change" },
        { R"({"valeu": 1})", 400, "unknown field 'valeu'" },
        { R"({"value": 1, "colour": "red"})", 400, "unknown field 'colour'" },
        { R"({"value": "35000"})", 400, "'value' must be an integer" },
        { R"({"value": 35000.0})", 400, "'value' must be an integer" },
        { R"({"value": 3.5e4})", 400, "'value' must be an integer" },
        { R"({"value": true})", 400, "'value' must be an integer" },
        { R"({"value": null})", 400, "'value' must be an integer" },
        { R"({"value": 100000000})", 422, "out of range" },
        { R"({"value": -10000000})", 422, "out of range" },
        { R"({"value": 18446744073709551615})", 422, "out of range" },
        { R"({"value": 99999999999999999999999})", 400, "'value' must be an integer" },
        { R"({"brightness": 16})", 422, "from 0 to 15" },
        { R"({"brightness": -1})", 422, "from 0 to 15" },
        { R"({"brightness": "bright"})", 400, "'brightness' must be an integer" },
        { R"({"value": 5, "brightness": 99})", 422, "from 0 to 15" },
    };
    for (const auto& b : bad) {
        const auto answer = f.call("PUT", "/displays/vs", b.body);
        const bool ok = (answer.status == b.status) && answer.raw.find(b.message) != std::string::npos;
        if (!ok) { std::cerr << "  for body '" << b.body << "': " << answer.status << " " << answer.raw << "\n"; }
        CHECK(ok);
    }
    CHECK(f.core.state("vs")->content == Content{ Number{ 7 } });          // none of those changed anything
    CHECK(f.core.state("vs")->brightness == 5);
    CHECK(f.call("PUT", "/displays/vs", std::string(Api::maxBody + 1, ' ')).status == 413);
    CHECK(f.call("PUT", "/displays/nope", R"({"value": 1})").status == 404);

    // The right method.
    auto notAllowed = f.call("POST", "/displays/vs", R"({"value": 1})");
    CHECK(notAllowed.status == 405);
    CHECK(!notAllowed.headers.empty() && notAllowed.headers[0] == std::pair<std::string, std::string>("Allow", "GET, PUT, DELETE"));
    CHECK(f.call("PUT", "/displays", "{}").status == 405);
    CHECK(f.call("DELETE", "/boards").status == 405);
    CHECK(f.call("PUT", "/events").status == 405);

    // Online shows.
    f.core.boardOnline("pico-1");
    CHECK(f.call("GET", "/displays/vs").body["online"] == true);
    CHECK(f.call("GET", "/boards").body["boards"][0]["online"] == true);
}

static void testEventLog()
{
    using std::chrono::milliseconds;
    EventLog log(3);

    auto empty = log.since(0);
    CHECK(empty && empty->entries.empty());
    const auto start = empty->next;

    log.append(Event{ Event::Type::BoardOnline, "a" });
    log.append(Event{ Event::Type::BoardOffline, "a" });
    auto all = log.since(0);
    CHECK(all && all->entries.size() == 2);
    CHECK(all && all->entries[0].seq == start + 1 && all->entries[1].seq == start + 2 && all->next == start + 2);
    auto later = log.since(all->entries[0].seq);
    CHECK(later && later->entries.size() == 1 && later->entries[0].board == "a" && later->entries[0].type == Event::Type::BoardOffline);
    CHECK(log.since(all->next)->entries.empty());
    CHECK(log.since(all->next)->next == all->next);

    // Only the last few are kept, and the numbers go on.
    for (int i = 0; i < 5; i++) { log.append(Event{ Event::Type::BoardOnline, "b" }); }
    auto kept = log.since(0);
    CHECK(kept && kept->entries.size() == 3 && kept->next == start + 7);

    // A number from before a restart is lower than all of ours: the client gets everything and no number comes twice.
    CHECK(log.since(1)->entries.size() == 3);
    // A number from the future (a clock that went back) gets nothing, and the number it gave back.
    CHECK(log.since(start + 1000)->entries.empty() && log.since(start + 1000)->next == start + 1000);

    // Waiting: no wait gives an answer at once; a wait ends when something arrives.
    const auto before = std::chrono::steady_clock::now();
    CHECK(log.since(kept->next, milliseconds(0))->entries.empty());
    CHECK(std::chrono::steady_clock::now() - before < milliseconds(200));

    std::optional<EventLog::Batch> waited;
    std::thread waiter([&] { waited = log.since(kept->next, std::chrono::seconds(10)); });
    std::this_thread::sleep_for(milliseconds(100));
    log.append(Event{ Event::Type::BoardOffline, "c" });
    waiter.join();
    CHECK(waited && waited->entries.size() == 1 && waited->entries[0].board == "c");
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(5));

    // A wait that times out gives an empty answer.
    CHECK(log.since(waited->next, milliseconds(50))->entries.empty());

    // Too many waiting: the next one is refused, and shutdown lets them all go.
    std::optional<EventLog::Batch> w1, w2;
    std::thread t1([&] { w1 = log.since(waited->next, std::chrono::seconds(10)); });
    std::thread t2([&] { w2 = log.since(waited->next, std::chrono::seconds(10)); });
    std::this_thread::sleep_for(milliseconds(150));
    CHECK(!log.since(waited->next, std::chrono::seconds(10)).has_value());
    log.shutdown();
    t1.join();
    t2.join();
    CHECK(w1 && w1->entries.empty() && w2 && w2->entries.empty());
    CHECK(log.since(waited->next, std::chrono::seconds(10))->entries.empty());      // after shutdown nobody waits
}

static void testApiEvents()
{
    ApiFixture f;
    f.events.append(Event{ Event::Type::BoardOnline, "pico-1" });
    auto got = f.call("GET", "/events");
    CHECK(got.status == 200 && got.body["events"].size() == 1);
    CHECK(got.body["events"][0]["type"] == "boardOnline" && got.body["events"][0]["board"] == "pico-1");
    const std::string time = got.body["events"][0]["time"];
    CHECK(time.size() == 20 && time[4] == '-' && time[10] == 'T' && time.back() == 'Z');

    const auto next = got.body["next"].get<uint64_t>();
    CHECK(got.body["events"][0]["seq"].get<uint64_t>() == next);
    CHECK(f.call("GET", "/events", "", { { "after", std::to_string(next) } }).body["events"].empty());

    f.events.append(Event{ Event::Type::BoardOffline, "pico-1" });
    auto more = f.call("GET", "/events", "", { { "after", std::to_string(next) }, { "wait", "5" } });
    CHECK(more.status == 200 && more.body["events"].size() == 1 && more.body["events"][0]["type"] == "boardOffline");

    CHECK(f.call("GET", "/events", "", { { "after", "abc" } }).status == 400);
    CHECK(f.call("GET", "/events", "", { { "after", "" } }).status == 400);
    CHECK(f.call("GET", "/events", "", { { "after", "-1" } }).status == 400);
    CHECK(f.call("GET", "/events", "", { { "wait", "31" } }).status == 400);
    CHECK(f.call("GET", "/events", "", { { "wait", "x" } }).status == 400);
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
    testStateStoreAddresses();
    testStateStoreDisplays();
    testRestoreAndSnapshot();
    testCommands();
    testMergingAndResend();
    testValidationAndRetry();
    testBrightnessBeforeNumber();
    testEvents();
    testApi();
    testApiEvents();
    testEventLog();
    testLog();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All " << checks << " checks passed.\n";
    return 0;
}
