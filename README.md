# Zero2WBusService

A service that keeps running on a Raspberry Pi Zero 2 W, owns the I2C bus to the Picos, and lets other programs ask it to pass
things on to them. Status: **an idea, nothing is built yet.** This file is the starting point.

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

1. **HTTP with JSON, over a Unix socket** (`/run/busd.sock`). A client is `curl --unix-socket /run/busd.sock ...`. Only those
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

## Plan

1. **The core, without network:** the configuration with names, the state per display, sending it again when a board appears,
   merging updates. Tested with a small command line program.
2. **The HTTP layer over the Unix socket** on that core.
3. **The systemd service**, and a short guide.
4. Then a TCP listener with a token, for the Windows PC.

## Open points

* The format of the configuration: use the existing INI, or something else?
* Several displays (modules) per board: the base supports it, but it has not been measured.
* Segments (`setBuffer`) and text need a new message type, in CppRaspberry.
* Leds and buttons (`Led`, `Button` messages exist, but have not been tried).
