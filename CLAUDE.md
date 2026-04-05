# CLAUDE.md

## Project Overview

**tio** is a serial device I/O tool written in C (GNU C99). It provides a command-line interface for connecting to serial TTY devices, aimed at embedded developers and hardware hackers. Licensed under GPL-2.0-or-later.

- Version: 3.9
- Build system: [Meson](https://mesonbuild.com/) (>= 0.53.2)
- Platforms: Linux, macOS

## Repository Structure

```
src/              # All C source and header files
  main.c          # Entry point
  options.c/h     # CLI option parsing
  tty.c/h         # Core TTY device handling, serial I/O
  configfile.c/h  # Configuration file parsing
  log.c/h         # File logging
  socket.c/h      # UNIX/network socket I/O redirection
  script.c/h      # Lua scripting engine
  xymodem.c/h     # X/Y-modem file transfer
  rs485.c/h       # RS-485 mode support (Linux only)
  setspeed.c/h    # Non-standard baud rate support
  timestamp.c/h   # Timestamp formatting
  alert.c/h       # Alert notifications
  print.c/h       # ANSI-formatted output
  signals.c/h     # Signal handling
  error.c/h       # Error handling and exit
  misc.c/h        # Utility functions
  fs.c/h          # Filesystem helpers
  readline.c/h    # Line-mode input
  meson.build     # Source-level build config
  bash-completion/ # Bash completion scripts
  version.h.in    # Version template (populated by git describe)
man/              # Man page sources
examples/         # Example config files and Lua scripts
  config/         # tio configuration file examples
  lua/            # Lua scripting examples
images/           # README images
.github/workflows/ # CI workflows (Ubuntu, macOS, CodeQL)
```

## Build Commands

```bash
# Configure (first time)
meson setup build

# Build
meson compile -C build

# Install (optional prefix)
meson setup build --prefix $HOME/opt/tio
meson install -C build

# Clean rebuild
rm -rf build && meson setup build
```

### Build Dependencies

- **Required**: meson, ninja, glib-2.0, lua (5.1–5.4), pthreads
- **Ubuntu**: `sudo apt install meson liblua5.2-dev libglib2.0-dev`
- **macOS**: `brew install meson ninja lua` (also uses IOKit and CoreFoundation frameworks)

### Meson Options

| Option | Type | Default | Description |
|---|---|---|---|
| `bashcompletiondir` | string | (auto) | Directory for bash completion scripts ("no" disables) |
| `install_man_pages` | boolean | true | Install man pages |

## CI / GitHub Actions

Workflows run on push/PR to `master`:

- **Ubuntu build** (`.github/workflows/ubuntu.yml`): Build on ubuntu-latest
- **macOS build** (`.github/workflows/macos.yml`): Build on macos-latest
- **CodeQL** (`.github/workflows/codeql.yml`): Security analysis

There are no automated test suites — CI validates that the project compiles successfully.

## Code Conventions

### C Style

- **Standard**: GNU C99 (`c_std=gnu99`)
- **Warning level**: 2, plus `-Wshadow` and `-Wno-unused-result`
- **Brace style**: Allman (opening brace on its own line)
- **Include guards**: `#pragma once`
- **License header**: Every source file starts with the GPL-2.0 license block
- **Naming**: `snake_case` for functions and variables; module-prefixed (e.g., `tty_connect()`, `log_open()`, `option_parse_flow()`)
- **Types**: Custom enums use `_t` suffix (e.g., `flow_t`, `parity_t`, `input_mode_t`)
- **Structs**: Named with `_t` suffix (e.g., `device_t`, `tty_line_config_t`)
- **Global state**: Options stored in a global `struct option_t option`
- **Comments**: C-style `/* ... */` for block comments; `//` for inline

### Architecture Patterns

- **Module pattern**: Each feature has a `.c/.h` pair with a clear prefix (e.g., `tty_`, `log_`, `socket_`)
- **Initialization flow** (`main.c`): signal handlers → option parse (pass 1) → config file parse → option parse (pass 2) → TTY configure → connect
- **Platform-specific code**: Guarded by `#ifdef` macros (`HAVE_TERMIOS2`, `HAVE_IOSSIOSPEED`, `HAVE_RS485`) set by meson build-time detection
- **Scripting**: Lua scripting API embedded via liblua

### When Making Changes

- Follow the existing Allman brace style and module-prefix naming
- Keep platform-specific code behind appropriate `#ifdef` guards
- Add new source files to `tio_sources` list in `src/meson.build`
- Include the GPL-2.0 license header in new source files
- The project has no automated tests — verify changes compile on both Linux and macOS build paths
