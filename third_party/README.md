# Third-party code

Two single-header libraries, copied as released. They live here because `cppr-deploy` only synchronises the CppRaspberry
library and this project to the build host, so nothing can be fetched or installed there for us. Do not edit them: to update,
download the new release and change the version and checksum below.

| File | Library | Version | Licence | Source | SHA-256 |
|---|---|---|---|---|---|
| `httplib.h` | [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.60.1 | MIT, see `LICENSE-cpp-httplib` | `https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.60.1/httplib.h` | `e932d7771bdb93150868987e30112d9a31e24bef592d6d7a07e973d5845cf8d6` |
| `json.hpp` | [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT, see `LICENSE-nlohmann-json` | `https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp` | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |

Check with `sha256sum third_party/httplib.h third_party/json.hpp`.

cpp-httplib is used for its Unix domain socket server only (`set_address_family(AF_UNIX)`, `bind_to_port()`, `listen_after_bind()`);
no TLS and no compression are compiled in. Version 0.60.1 was released on 8 October 2026, one day before it was added here.
