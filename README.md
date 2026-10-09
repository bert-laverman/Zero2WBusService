# Zero2WBusService

A service that keeps running on a Raspberry Pi Zero 2 W, owns the I2C bus to the Picos, and lets other programs ask it to pass
things on to them. Status: **the core (step 1) and the adapter to the bus (step 1b) have run on berry-1 with two Picos (9 October 2026); the HTTP layer (step 2) has run on berry-1 as well (see below).**

It uses [CppRaspberry](https://github.com/bert-laverman/CppRaspberry). Read `docs/i2c-bus.md` there first: it describes the bus,
the address assignment, what has been tested, and the environment.

## Why

`Zero2WTestI2C` is a test program: it runs for a number of seconds and counts on a display. Real use needs something that
stays up, and that other programs (scripts, a bridge to a flight simulator, ...) can talk to without knowing about the bus:
"show 35000 on the display called `altitude`", and "tell me when this button is pressed".

## What it does

* **Names instead of addresses.** A client uses `altitude`; the service knows which board, which display, which module.
  The shape of that configuration exists already, in the old `i2c-state.ini` (`[display:1] name=altitude device=max-1 index=0`,
  `[device:max-1] interface=spi-0 type=max7219`, `[board:pico-1] boardId=... address=...`).
* **Remembers the state of every display**, and sends it again when a board appears (again): a Pico forgets everything
  when it restarts.
* **Merges updates**: a simulator delivers a value many times a second, only the last one per display needs to go over the bus.
* **Passes events back**: a button on a Pico, a board that appears or disappears.
* Runs as a systemd service, starts by itself, logs.
* Hands out the addresses (`I2CBusController`) and saves them, as `Zero2WTestI2C` does.

## How clients talk to it

First clients: scripts and small programs on the command line, on the Zero itself.

1. **HTTP with JSON, over a Unix socket** (`/run/i2cbus.sock`). A client is `curl --unix-socket /run/i2cbus.sock ...`. Only those
   in a group that owns the socket can reach it; no passwords or certificates. A header-only HTTP library
   ([cpp-httplib](https://github.com/yhirose/cpp-httplib)) and `nlohmann-json` do the work, which is light enough for a Zero.
   (Unix sockets are, as far as I know, supported by cpp-httplib: check.)
2. Later, when the Windows PC wants in: **the same API on a TCP port**, only on the local network, with a token in every
   request (a file with mode `600`). TLS only if the network is not trusted.

An API sketch (to be changed):

| Request | Meaning |
|---|---|
| `PUT /displays/altitude` with `{"value": 35000}` | show a number |
| `PUT /displays/altitude` with `{"brightness": 3}` | set brightness |
| `DELETE /displays/altitude` | blank |
| `GET /displays`, `GET /boards` | what is there, and what it shows |
| `GET /events` | a stream of events (board online/offline, button) |

Other transports (gRPC, MQTT) can be put on the same core later. The core has to be built so the transport is a thin layer.

## Safety

Only the Unix socket at first. For a TCP listener: bind to a fixed address on the local network (never `0.0.0.0`), a token, a
maximum size for requests and a limit on the number of connections, checks on all input (only known names, numbers within
range), and the service runs as its own user, without privileges, with the systemd restrictions (`NoNewPrivileges`,
`ProtectSystem=strict`, ...), and only the access to the I2C bus and `pigpiod` that it needs.

## Where it runs

On the Zero that owns the bus (here: `berry-1`). A Raspberry Pi 5 is used to cross-compile and flash (see `cppr-deploy` in
CppRaspberry); it has no part in the service.

## Names

The service is called `i2cbus`: `/etc/i2cbus/i2cbus.ini` (what the displays are called and where they sit, edited by hand), a
state file that the service writes itself (the addresses), `/run/i2cbus.sock`, `i2cbus.service`, and a user and group `i2cbus`.

## Plan

1. **The core, without network:** the configuration with names, the state per display, sending it again when a board appears,
   merging updates. Tested with a small command line program. **Done**, see below.
2. **The HTTP layer over the Unix socket** on that core. **Built**, see below.
3. **The systemd service**, and a short guide.
4. Then a TCP listener with a token, for the Windows PC.

## The core (step 1)

The core (`src/core`) only needs the standard library, so it builds and is tested on any machine. The bus is behind one
interface, `Sink`; the adapter that implements it with `RemoteMAX7219` and `I2CBusController` is not written yet.

* `IniFile`, `Config`: reads `i2cbus.ini`, which has the shape of the old `i2c-state.ini` (display, device, interface and board
  sections). Mistakes (a double name, a module that does not exist, an unknown board) are errors that say where; keys that are
  not used give a warning.
* `BusCore`: the state per display, **merging** (only the last value per display goes out at a `flush()`), **sending again**
  when a board comes online, and a queue of events (board online/offline).
* `StateStore`: what the service remembers, in a file that only the service writes (the configuration is never rewritten): the
  address of every board, and what every display shows. Addresses are saved at once; the displays at most every 10 seconds, and
  when the service stops (a power cut loses at most the last 10 seconds). After a restart the displays get their last value
  back as soon as their boards are there. A brightness is only kept if it differs from the configuration's.
* `runCommand`: the text commands (`set altitude 35000`, `show`, ...) of the command line tool, which the service also takes on
  its standard input until there is an HTTP layer.
* What a display shows is a `variant` (`Blank`, `Number`), so segments and text can be added.

```bash
cmake -S . -B build && cmake --build build && build/tools/core-test && build/tools/http-test    # the tests
build/tools/i2cbus-cli ../Zero2WTestI2C/i2c-state.ini                   # try it by hand, type 'help'
```

## The adapter (step 1b)

`src/service` is the part that only builds for the Zero: `main.cpp` runs the loop (every 10 ms: the bus controller's `tick()`,
`processIncoming()`, the commands on stdin, and `BusCore::flush()`), and `BusSink` sends what the core wants as `Max7219`
messages. It does not use `RemoteMAX7219`, which keeps its own copy of the display state and so would not send a value again to
a board that restarted. The bus controller's `onBoardAppeared()` and `onBoardGone()` become `boardOnline()` and `boardOffline()`.
A message the bus refuses is tried again after half a second. The service stops on SIGINT and SIGTERM, with `driver.close()`.

The log has a time and a level on every line, on standard error (`--log-level error|warning|info|debug`, or `I2CBUS_LOG`;
default `info`). The bus controller has no levels of its own, so its lines are sorted by their first character: `* ...` is a
warning, `- ...` is info, and the rest, the "Received Hello" of every board every few seconds, is debug. The answers to commands
go to standard output: run with `2>>i2cbus.log` to keep them apart. A line that is pasted without its newline waits for Enter.

Only one bus controller can run at a time: `Zero2WTestI2C` and this service both want the BSC slave at address `0x0a`.

### Trying it on berry-1

`cppr-deploy Zero2WBusService` puts `~/i2cbus` on the Zero. Copy `i2cbus.ini.example` to `~/i2cbus.ini` there, put the real
board ids in (from `~/i2c-state.ini`), and make sure `test-i2c` is not running.

```bash
./i2cbus ~/i2cbus.ini ~/i2cbus-state.ini
```

Type commands on its input: `show`, `set altitude 12345`, `set altitude blank`, `brightness altitude 8`. Then restart a Pico
from the Pi 5 (`picotool reboot -f --bus B --address A`, with the numbers from `cppr-deploy --list-picos`): after about 13 seconds
the service logs that the board is gone and appeared again, and its display should show the last value without anything being
typed. Stop with Ctrl-C and check that the bus still works (the next start finds the boards again).

### What was tested on berry-1 (9 October 2026)

Two Picos (`PicoTestI2C`, a MAX7219 each, 0x61 and 0x62), commands typed on the service's input, restarts from the Pi 5:

| Test | Result |
|---|---|
| Start with both boards already running | Both `appeared` within 2 seconds, no restart needed |
| `set`, `blank`, `brightness` on both displays | Displays follow; a number out of range is refused (`value out of range`) |
| Board A restarted (`picotool reboot -f`) | `gone` after 10 s, `appeared` and address saved at 11 to 13 s; the display showed its last value again without a command |
| Board B in BOOTSEL, a value set while it was gone, then started | `gone` 10 s after BOOTSEL; the value set while offline showed after `appeared` |
| Ctrl-C, then start again at once (twice) | `Shutting down.`; both boards found again within 2 seconds |
| Values set, 10 s later `i2cbus-state.ini` | `[display:...]` sections with the content, and a brightness only where it differs from the configuration |
| `set` and Ctrl-C within 10 s | The value is in the state file: stopping saves it |
| Service stopped, board A restarted, service started | Display A showed its last value again (777), display B too (-250), without a command |

The HTTP API, with `curl --unix-socket` on berry-1 (same day, `i2cbus --socket ~/i2cbus.sock`):

| Test | Result |
|---|---|
| The socket | Mode `srw-rw----` |
| `GET /displays`, `/boards`, `/displays/<name>` | The values restored from the state file, with `online` true for both boards |
| `PUT /displays/altitude` with a value and a brightness | 200, the display as it is then |
| A float, an unknown field, a number out of range, an unknown display, `POST` | 400, 400, 422, 404, 405 with `Allow` |
| Board B restarted | Events `boardOffline` 7 to 10 s after the restart, `boardOnline` after 12 s, matching the log |
| `GET /events?after=<next>&wait=30` while board A restarted | The call returned when `boardOffline` came (9 s after the restart), not after 30 s; `boardOnline` then followed with the new `next` |

Not tested yet on berry-1: removing the socket when the service stops, replacing a socket that a killed program left behind,
and stopping while a client waits for events (all three are tested on a PC, in `http-test`).

Not tested yet: stopping with Ctrl-C or `kill` in the middle of a burst of messages, a failing bus (the retry delay), and
many updates at once on the real bus.

The values of the displays are saved and restored (see `StateStore`). A board that has never been given a value is left as it is, so after a restart it shows its own address: new boards are not
blanked (decided on 9 October 2026).

## The HTTP API (step 2)

`i2cbus --socket PATH` serves the API over HTTP on a Unix socket. The socket is created with mode `0660`, so only its owner and
group can connect: that is the whole access control, there are no passwords or certificates. (The default path of the installed
service will be `/run/i2cbus.sock`, from its systemd unit; without `--socket` there is no socket.) A client is `curl`:

```bash
curl --unix-socket /run/i2cbus.sock http://localhost/displays
curl --unix-socket /run/i2cbus.sock -X PUT -d '{"value": 35000, "brightness": 3}' http://localhost/displays/altitude
```

| Request | Meaning |
|---|---|
| `GET /displays`, `GET /displays/<name>` | what each display should show, its brightness, and whether its board is online |
| `PUT /displays/<name>` with `{"value": 35000, "brightness": 3}` | show a number and/or set the brightness; at least one of the two |
| `DELETE /displays/<name>` | blank the display |
| `GET /boards` | the boards, and whether they are online |
| `GET /events?after=<seq>&wait=<seconds>` | what happened to the boards after `seq`; waits up to `wait` seconds (at most 30) for the next one |

An answer is JSON. A display looks like `{"name": "altitude", "board": "pico-a", "module": 0, "online": true, "content":
{"type": "number", "value": 35000}, "brightness": 3}`; `content` is `null` as long as nobody has said what to show, and
`{"type": "blank"}` for a blank display (other kinds of content can follow, so look at `type`). `PUT` and `DELETE` answer with the
display as it is then. They change what the display *should* show: it goes to the board at once if it is online, and when it
comes online if it is not (`online` says which), and many updates in a short time are merged into the last.

A request is checked in full, and one that is refused changes nothing:

| Status | When |
|---|---|
| 400 | the body is not JSON or not an object, a field is not known (`valeu`), `value` or `brightness` is not an integer (`35000.0` is not), or there is nothing to change |
| 404 | no such display, or no such path |
| 405 | the method does not fit the path (`Allow` says which do) |
| 413 | the body is longer than 4096 bytes |
| 422 | a number the display cannot show (-9999999 to 99999999) or a brightness outside 0 to 15 |
| 503 | too many clients are waiting for events (two at most) |

`GET /events` answers `{"events": [{"seq": 1791532948277, "time": "2026-10-09T08:02:29Z", "type": "boardOnline", "board":
"pico-a"}], "next": 1791532948277}`. Keep `next` and give it as `after` the next time; that way you miss nothing and see nothing
twice. The numbers only go up, also over a restart of the service (they start at the time of the start, in milliseconds), the last 256
events are kept, and with `wait` the call returns at once when something happens. When the service stops, a waiting call returns.

The server runs four threads and queues sixteen more connections, reads and writes with a timeout of 5 seconds, and takes
no more than 4096 bytes of body. A socket that a killed program left behind is replaced; one that somebody listens on, or a file
that is not a socket, is not touched and stops the start. A path that starts with `@` (a socket without a file, so without rights)
is refused.

How it is built: `Api` (`src/core/api.cpp`) turns an `ApiRequest` into an `ApiResponse` and knows nothing of sockets, so the
routes are tested in `core-test`; `HttpServer` (`src/http`) is the thin layer around cpp-httplib, tested with real sockets in
`http-test`. The libraries are in `third_party/` (see its README). To try the API on a PC, without a bus: `i2cbus-sim
--socket /tmp/i2cbus.sock i2cbus.ini.example`, and type `online pico-a` to let a board appear.

## Building for the Zero

`cppr-deploy Zero2WBusService` (see `tools/README.md` in CppRaspberry) builds on the Pi 5 with the toolchain file, and copies
the first executable it finds in `build/` to the Zero. So `CMakeLists.txt` has two modes (`I2CBUS_ZERO`, on when there is a
toolchain file): for the Zero it builds only the service `i2cbus`; on a PC it builds the core, the command line tool and the
tests, in `build/tools/`. Anything we need besides the library has to live in this directory: only the library and this
project are synchronised to the Pi 5.

## Decisions

* Configuration: the INI format, split in `i2cbus.ini` (by hand) and a state file (by the service).
* Several modules per board: `index` per display, the configuration decides how many. An `index` that the interface does not
  have is an error in the configuration. Measuring on real hardware comes later.
* Segments and text: a new message type in CppRaspberry, later. The core is ready for it.
* Leds and buttons: only board online/offline events for now. Buttons and leds when they have been tried on real hardware.

## Licences

This project is under the Apache License 2.0 (see `LICENSE`). `third_party/` holds two libraries under the MIT licence, copied
unchanged with their copyright notices: [cpp-httplib](https://github.com/yhirose/cpp-httplib) (Yuji Hirose) and
[nlohmann/json](https://github.com/nlohmann/json) (Niels Lohmann). Their licence texts are in `third_party/`, and
`third_party/README.md` has the versions and checksums.
