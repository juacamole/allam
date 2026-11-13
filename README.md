# allam

simple llama daemon. Runs `llama-server` in router mode and lets a CLI or UI widget control it over a unix socket.

## features

- adjust batch size (`-ub` / `-b`), applied as soon as the server is idle
- load and unload model weights
- JSON replies, one line per command

Tech stack: C++20, cpp-httplib, nlohmann/json, CMake, Nix.

## usage

```sh
nix build                     # or: direnv allow && cmake -B build && cmake --build build
cp allam.conf.example ~/.config/allam/allam.conf
allam start                   # background, log: ~/.local/state/allam/allam.log
allam status                  # {"ok":true,"phase":"running","ubatch":512,...}
allam ls                      # numbered list, also: list, models
allam load <model>            # full name, unique part of it, or index
allam unload [model]
allam batch <ubatch> [batch]
allam stop
```

Socket: `$XDG_RUNTIME_DIR/allam.sock` (override with `ALLAM_SOCKET`).

License: MIT
