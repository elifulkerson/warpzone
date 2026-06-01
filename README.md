# Warp Zone (`warpzone`)

> A seamless PTY wrapper that lets you capture command output and pipe it
> through a **local** shell pipeline, on demand, without disturbing your
> interactive session.

`warpzone` launches an interactive shell (or any program) inside a
pseudo-terminal (PTY) and sits transparently in front of it. You type and see
output exactly as usual. At any time you can press **`Ctrl-]`** to type a local
shell pipeline (for example `grep error | wc -l`); `warpzone` runs the current
remote command, captures its output, and feeds that captured output into your
pipeline **executed on the local machine**.

This is especially handy when the program you're driving lives somewhere your
local tools don't — a remote SSH session, a serial console, a container, or a
restricted appliance shell — but you'd still like to post-process its output
with the tools installed *here*.

---

## Table of contents

- [How it works](#how-it-works)
- [Building](#building)
- [Installing](#installing)
- [Quick start](#quick-start)
- [Interactive keys](#interactive-keys)
- [Capture end conditions](#capture-end-conditions)
- [Command-line options](#command-line-options)
- [Configuration file (`~/.warpzonerc`)](#configuration-file-warpzonerc)
- [Examples](#examples)
- [Notes & limitations](#notes--limitations)
- [Security considerations](#security-considerations)
- [License](#license)

---

## How it works

`warpzone` operates as a small state machine with three modes:

| Mode | What's happening |
|------|------------------|
| **PASSTHRU** | Normal operation. Your keystrokes go to the child program; its output is printed to your screen. |
| **PIPE_ENTRY** | You pressed `Ctrl-]`. A green `\|` marker appears and you type a local pipeline. Nothing is sent to the child yet. |
| **CAPTURE** | You pressed Enter to commit. `warpzone` sends Enter to the child, then silently records the child's output to a temporary file while showing a live status overlay. |

When capture ends, `warpzone`:

1. Draws a green horizontal rule.
2. Runs your pipeline locally via `/bin/sh -c "<pipeline>"`, with the captured
   output connected to the pipeline's **stdin**.
3. Draws a second green horizontal rule.
4. Returns to PASSTHRU.

The captured output is written to an anonymous temp file under `/tmp`
(created with `mkstemp` and immediately `unlink`ed), so it never lingers on
disk after the pipeline finishes.

### Signals & job control

In PASSTHRU mode, common interactive signals (`SIGINT`, `SIGQUIT`, `SIGTERM`,
`SIGHUP`, `SIGTSTP`, `SIGCONT`) are caught by `warpzone` and forwarded to the
PTY's foreground process group, so `Ctrl-C`, `Ctrl-Z`, and job control behave
just like a normal terminal. Window-size changes (`SIGWINCH`) are propagated to
the child so full-screen apps see the correct rows and columns.

---

## Building

`warpzone` is a single C source file with no external dependencies beyond a
POSIX libc.

```sh
make            # release build (-O2)
make debug      # debug build (-O0 -g)
make clean      # remove build artifacts
```

Or compile directly:

```sh
gcc -O2 -Wall -Wextra -pedantic -o warpzone warpzone.c
```

---

## Installing

```sh
sudo make install                 # installs to /usr/local/bin
make PREFIX=$HOME/.local install  # install somewhere else
sudo make uninstall               # remove it
```

`PREFIX` (default `/usr/local`) and `BINDIR` (default `$(PREFIX)/bin`) can be
overridden on the command line.

---

## Quick start

```sh
./warpzone
```

This drops you into an interactive `bash` (the default child) inside the warp
zone. Use the shell normally. When you want to post-process the output of the
*next* command:

1. Press **`Ctrl-]`** — a green `|` appears.
2. Type a local pipeline, e.g. `grep -i warning | sort | uniq -c`.
3. Press **Enter**. `warpzone` runs whatever command line is currently sitting
   at your remote prompt, captures its output, and pipes it through your local
   pipeline.

> **Tip:** Type (but don't run) the remote command first, then trigger
> `Ctrl-]`. Committing the pipeline sends Enter to the remote shell, which runs
> the command line that's already there.

---

## Interactive keys

| Context | Key | Action |
|---------|-----|--------|
| PASSTHRU | `Ctrl-]` | Enter PIPE_ENTRY (prints a green `\|`). |
| PIPE_ENTRY | *printable* | Append to the local pipeline. |
| PIPE_ENTRY | `Backspace` | Edit; backspacing over the `\|` marker cancels. |
| PIPE_ENTRY | `Enter` | Commit: send Enter to the child, begin CAPTURE. |
| PIPE_ENTRY | `Ctrl-C` | Cancel PIPE_ENTRY. |
| PIPE_ENTRY | `Ctrl-]` | Cancel PIPE_ENTRY. |
| CAPTURE | `Ctrl-]` | End capture immediately and run the pipeline. |

Committing an **empty** pipeline simply leaves PIPE_ENTRY without capturing.

---

## Capture end conditions

Once in CAPTURE mode, recording stops at the first of:

- **Quiet gap (default):** no PTY output for `--gap-ms` milliseconds (default
  `200`). This is the usual, automatic way capture ends.
- **Manual end:** you press `Ctrl-]` at any time.
- **Byte guardrail:** `--max-bytes` bytes have been captured (disabled by default).
- **Time guardrail:** `--max-seconds` have elapsed since capture began
  (disabled by default).

Use `--manual-end` to disable the quiet-gap heuristic entirely, so capture only
ends via `Ctrl-]` or a guardrail. This is useful for long-running or bursty
commands whose output has natural pauses.

During CAPTURE a dim status overlay is drawn on the last terminal row showing
captured byte count, idle time, capture age, the active end mode, and a live
throughput sparkline.

---

## Command-line options

```
warpzone [options]
```

| Option | Description |
|--------|-------------|
| `--shell PATH` | Program to exec inside the PTY (default: `/bin/bash`). |
| `--shell-arg ARG` | Add an argument for the child program. Repeatable. If none is given, `warpzone` adds `-i` by default. |
| `--gap-ms N` | Quiet-gap in milliseconds to end capture (default `200`; `0` disables gap-ending). |
| `--manual-end` | Disable quiet-gap ending (capture ends only via `Ctrl-]` or guardrails). |
| `--max-bytes N` | End capture after `N` bytes (`0` disables; default `0`). |
| `--max-seconds N` | End capture after `N` seconds (`0` disables; default `0`). |
| `--version`, `-V` | Print version and exit. |
| `--help`, `-h` | Show help and exit. |

CLI options override values loaded from the configuration file.

---

## Configuration file (`~/.warpzonerc`)

If present, `~/.warpzonerc` is read at startup, **before** command-line
arguments are parsed (so CLI flags win).

**Format**

- UTF-8 text, one `key=value` setting per line.
- Surrounding whitespace around keys and values is tolerated.
- Blank lines are ignored; lines beginning with `#` are comments.

**Supported keys**

| Key | Type | Meaning |
|-----|------|---------|
| `shell` | string | Path to the PTY child program (e.g. `/bin/zsh`). |
| `shell_arg` | string | Adds one argument; repeat the line for multiple args. |
| `gap_ms` | int ≥ 0 | Quiet-gap end condition, in milliseconds. |
| `manual_end` | bool | `true`/`false`/`yes`/`no`/`on`/`off`/`1`/`0`. |
| `max_bytes` | int ≥ 0 | Capture size guardrail, in bytes. |
| `max_seconds` | int ≥ 0 | Capture time guardrail, in seconds. |

**Example `~/.warpzonerc`**

```ini
# Use zsh and disable gap-ending
shell=/bin/zsh
shell_arg=-i
manual_end=true
max_seconds=10
max_bytes=1048576
```

---

## Examples

Wrap the default interactive bash:

```sh
warpzone
```

Wrap a remote session — capture remote output, process it with local tools:

```sh
warpzone --shell /usr/bin/ssh --shell-arg user@host
```

Then, at the remote prompt, type `dmesg`, press `Ctrl-]`, type
`grep -i error | tail -n 20`, and press Enter to run `dmesg` remotely and view
the last 20 error lines filtered locally.

Wrap a different shell with a longer quiet-gap for chatty commands:

```sh
warpzone --shell /bin/zsh --gap-ms 500
```

Capture a long-running command, ending capture manually and capping it at
30 seconds:

```sh
warpzone --manual-end --max-seconds 30
```

---

## Notes & limitations

- **Alternate-screen apps:** PIPE_ENTRY is disabled while the child is in the
  alternate screen buffer (e.g. `vim`, `less`, `top`), because injecting a
  visible marker into a full-screen app would corrupt its display. Pressing
  `Ctrl-]` there beeps and prints a notice — exit the full-screen app first.
- **CAPTURE swallows keystrokes:** while capturing, normal keystrokes are
  ignored; only `Ctrl-]` (to end capture) is honored. This keeps capture
  deterministic.
- The status overlay and throughput sparkline use Unicode block glyphs, so a
  UTF-8-capable terminal is recommended.
- `warpzone` targets POSIX systems with PTY support (Linux, the BSDs, macOS).

---

## Security considerations

The pipeline you type in PIPE_ENTRY is executed **locally** via
`/bin/sh -c`, with the captured remote output as its stdin. Treat captured
output as untrusted data: it is fed to your pipeline on stdin (not interpreted
as shell), but be mindful of what your *own* pipeline does with that data. The
command line you type is your own, so the usual care with shell metacharacters
applies.

---

## License

See the repository for license details.
