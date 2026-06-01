/*
Warp Zone (warpzone) — Seamless PTY wrapper with on-demand capture + local pipeline

Purpose
-------
warpzone launches an interactive shell (or other program) inside a PTY and acts as a
terminal “shim” in front of it. You normally type and see output as usual (PASSTHRU).

At any time (except while in the alternate screen), you can press Ctrl-] to enter a
local “PIPE_ENTRY” mini-editor. You type a shell pipeline locally (e.g., "grep foo | wc -l"),
then press Enter to commit. warpzone sends Enter to the remote shell, captures the
remote output for a short time window, and then feeds that captured output into the
local pipeline (executed on the local machine via /bin/sh -c).

Key behaviors
-------------
- Ctrl-] enters PIPE_ENTRY (prints a green "|")
- Enter in PIPE_ENTRY commits:
    - sends Enter to the PTY (remote)
    - switches to CAPTURE mode and stores PTY output into a temp file
- CAPTURE ends on:
    - quiet gap (default), or
    - Ctrl-] (always), or
    - guardrails: --manual-end, --max-bytes, --max-seconds
- After CAPTURE ends:
    - prints a green HR
    - runs local pipeline with captured output as stdin
    - prints a green HR
    - returns to PASSTHRU

Cancel / editing in PIPE_ENTRY
------------------------------
- Ctrl-C or Ctrl-] cancels PIPE_ENTRY
- Backspace edits, and backspace over the marker cancels

Safety/UX constraints
---------------------
- PIPE_ENTRY is disabled in alternate-screen apps (vim, less, top, etc.) because
  injecting visible text into those apps is disruptive.
- During CAPTURE, user keystrokes are mostly ignored (except Ctrl-] to end capture),
  because we’re capturing output deterministically.

Signals & job control
---------------------
- In PASSTHRU, common signals (SIGINT, SIGTSTP, etc.) are captured by the parent
  and forwarded to the PTY foreground process group, so job control behaves correctly.
- Uses poll() loop and forwards window size changes (SIGWINCH).

Build
-----
  gcc -O2 -Wall -Wextra -pedantic -o warpzone warpzone.c
*/

/* ---- versioning ---- */
#define WARPZONE_VERSION "0.3.0"


#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* Ctrl-] is ASCII 29 (GS). This is our “warp key”. */
#define WARP_KEY 29 /* Ctrl-] */

/* poll() tick. Short enough to keep overlays responsive. */
#define POLL_TICK_MS 20

/* Original and raw terminal settings for stdin. */
static struct termios g_stdin_orig;
static struct termios g_stdin_raw;

/* PTY master fd and child pid. */
static int   g_fdm = -1;
static pid_t g_child_pid = -1;

/* SIGCHLD sets this flag; main loop reaps children. */
static volatile sig_atomic_t g_sigchld_flag = 0;

/*
Pending signals to forward to the PTY foreground process group.
We use a bitmask because signal handlers must be async-signal-safe;
we just record “a signal arrived” and forward it in the main loop.
*/
static volatile sig_atomic_t g_pending_sig_mask = 0;

/* -----------------------------------------------------------------------------
 * Error + IO utilities
 * -------------------------------------------------------------------------- */

/* Print formatted error message to stderr and exit. */
static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

/*
write_all(fd, buf, n):
Write exactly n bytes unless an error occurs. Retries on EINTR.
Returns bytes written (n) or -1 on non-EINTR error.
*/
static ssize_t write_all(int fd, const void *buf, size_t n) {
  const unsigned char *p = (const unsigned char *)buf;
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    off += (size_t)w;
  }
  return (ssize_t)off;
}

/* Restore stdin terminal settings at program exit. */
static void restore_terminal(void) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_stdin_orig);
}

/*
Return elapsed seconds from time a to time b (b - a).
Uses timeval fields with microsecond precision.
*/
static double elapsed_secs(const struct timeval *a, const struct timeval *b) {
  return (double)(b->tv_usec - a->tv_usec) / 1000000.0 +
         (double)(b->tv_sec - a->tv_sec);
}

/* Minimal basename helper for shell argv[0]. */
static const char *basename_c(const char *path) {
  if (!path) return "sh";
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/*
Print a green "warpzone:" prefix info line on its own line.
Used for user-visible warnings/status.
*/
static void print_info_line(const char *msg) {
  const char *prefix = "\r\n\033[32mwarpzone:\033[39m ";
  (void)write_all(STDOUT_FILENO, prefix, strlen(prefix));
  (void)write_all(STDOUT_FILENO, msg, strlen(msg));
  (void)write_all(STDOUT_FILENO, "\r\n", 2);
}

/*
Print a solid green horizontal rule across the current terminal width.
ASCII-safe: uses '-' rather than box-drawing characters.
*/
static void print_green_hr(void) {
  struct winsize ws;
  int cols = 80;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    cols = (int)ws.ws_col;
  }

  (void)write_all(STDOUT_FILENO, "\r\033[32m", 6);
  for (int i = 0; i < cols; i++) (void)write_all(STDOUT_FILENO, "-", 1);
  (void)write_all(STDOUT_FILENO, "\033[39m\r\n", 7);
}

/* -----------------------------------------------------------------------------
 * Signal handling
 * -------------------------------------------------------------------------- */

/* SIGCHLD handler: set a flag; main loop will reap. */
static void sigchld_handler(int sig) {
  (void)sig;
  g_sigchld_flag = 1;
}

/*
Propagate current stdin window size to the PTY master.
This ensures child apps see correct rows/cols.
*/
static void sync_winsz(void) {
  if (g_fdm < 0) return;
  struct winsize ws;
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
    (void)ioctl(g_fdm, TIOCSWINSZ, &ws);
  }
}

/* SIGWINCH handler: update PTY size. */
static void sigwinch_handler(int sig) {
  (void)sig;
  sync_winsz();
}

/* Bit flags for “pending signal forwarding”. */
enum {
  PEND_INT  = 1 << 0,
  PEND_QUIT = 1 << 1,
  PEND_TERM = 1 << 2,
  PEND_HUP  = 1 << 3,
  PEND_TSTP = 1 << 4,
  PEND_CONT = 1 << 5
};

/*
Signal handler for signals we want to forward.
Only sets bits (async-signal-safe).
*/
static void forwardable_sig_handler(int sig) {
  switch (sig) {
    case SIGINT:  g_pending_sig_mask |= PEND_INT;  break;
    case SIGQUIT: g_pending_sig_mask |= PEND_QUIT; break;
    case SIGTERM: g_pending_sig_mask |= PEND_TERM; break;
    case SIGHUP:  g_pending_sig_mask |= PEND_HUP;  break;
    case SIGTSTP: g_pending_sig_mask |= PEND_TSTP; break;
    case SIGCONT: g_pending_sig_mask |= PEND_CONT; break;
    default: break;
  }
}

/*
Reap any children that have exited.
Returns true if the *main child* (the PTY shell) exited.
*/
static bool reap_children_and_check_main(void) {
  int status = 0;
  pid_t p;
  for (;;) {
    p = waitpid(-1, &status, WNOHANG);
    if (p == 0) return false;          /* no more to reap */
    if (p < 0) {
      if (errno == EINTR) continue;
      if (errno == ECHILD) return false;
      return false;
    }
    if (p == g_child_pid) return true; /* main child exited */
  }
}

/*
Forward a signal to the PTY foreground process group.
This makes Ctrl-C / Ctrl-Z etc. behave like a normal terminal.

- tcgetpgrp(g_fdm) gives the current foreground pgrp for the PTY.
- kill(-fg, sig) sends to that whole process group.
- fallback: if tcgetpgrp fails, signal the main child group.
*/
static void forward_signal_to_pty_fg(int sig) {
  if (g_fdm < 0) return;

  pid_t fg = tcgetpgrp(g_fdm);
  if (fg > 0) {
    (void)kill(-fg, sig);
  } else if (g_child_pid > 0) {
    (void)kill(-g_child_pid, sig);
  }
}

/*
Parent signal configuration:
- Ignore SIGTTOU/SIGTTIN so we don't get stopped when manipulating terminal.
- Install forwardable handler for common interactive signals.
*/
static void parent_setup_signal_behavior(void) {
  signal(SIGTTOU, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);

  struct sigaction sa = {0};
  sa.sa_handler = forwardable_sig_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;

  sigaction(SIGINT,  &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP,  &sa, NULL);
  sigaction(SIGTSTP, &sa, NULL);
  sigaction(SIGCONT, &sa, NULL);
}

/* -----------------------------------------------------------------------------
 * Minimal terminal feature tracking (alternate-screen detection)
 * -------------------------------------------------------------------------- */

/*
We track whether the child has switched into the alternate screen buffer
(ESC[?1049h and friends). While in alternate screen, we refuse PIPE_ENTRY because
printing our marker into a full-screen app is disruptive.
*/
typedef struct {
  bool alt_screen;             /* true if in alternate screen */
  unsigned char tail[64];      /* recent trailing bytes for boundary scanning */
  size_t tail_len;
} tty_features_t;

static bool memeq(const unsigned char *a, const unsigned char *b, size_t n) {
  return memcmp(a, b, n) == 0;
}

static void ttyfeat_init(tty_features_t *t) {
  memset(t, 0, sizeof(*t));
}

/*
Scan output stream for alternate screen enter/exit sequences.
We maintain a small tail buffer so sequences that straddle read() boundaries
are still recognized.
*/
static void ttyfeat_update(tty_features_t *t, const unsigned char *chunk, size_t n) {
  /* Common alternate screen toggles used by various apps/termcaps. */
  static const unsigned char enter_1049[] = "\x1b[?1049h";
  static const unsigned char exit_1049[]  = "\x1b[?1049l";
  static const unsigned char enter_47[]   = "\x1b[?47h";
  static const unsigned char exit_47[]    = "\x1b[?47l";
  static const unsigned char enter_1047[] = "\x1b[?1047h";
  static const unsigned char exit_1047[]  = "\x1b[?1047l";

  unsigned char scan[192];

  /* Start with previous tail, append new chunk (or the last part of it). */
  size_t tlen = t->tail_len;
  if (tlen > sizeof(scan)) tlen = sizeof(scan);

  size_t take = n;
  if (tlen + take > sizeof(scan)) {
    /* Ensure scan[] never overflows; if huge chunk, keep only tail end. */
    if (take > sizeof(scan)) {
      chunk += (take - sizeof(scan));
      take = sizeof(scan);
      tlen = 0;
    } else {
      /* Drop oldest portion of tail to make room. */
      size_t overflow = (tlen + take) - sizeof(scan);
      if (overflow >= tlen) {
        tlen = 0;
      } else {
        memmove(t->tail, t->tail + overflow, tlen - overflow);
        tlen -= overflow;
      }
    }
  }

  memcpy(scan, t->tail, tlen);
  memcpy(scan + tlen, chunk, take);
  size_t scan_len = tlen + take;

  /* Linear scan for known sequences. */
  for (size_t i = 0; i < scan_len; i++) {
    size_t rem = scan_len - i;

    if (rem >= sizeof(enter_1049) - 1 && memeq(scan + i, enter_1049, sizeof(enter_1049) - 1)) { t->alt_screen = true;  i += (sizeof(enter_1049) - 2); continue; }
    if (rem >= sizeof(exit_1049) - 1  && memeq(scan + i, exit_1049,  sizeof(exit_1049)  - 1)) { t->alt_screen = false; i += (sizeof(exit_1049)  - 2); continue; }
    if (rem >= sizeof(enter_47) - 1   && memeq(scan + i, enter_47,   sizeof(enter_47)   - 1)) { t->alt_screen = true;  i += (sizeof(enter_47)   - 2); continue; }
    if (rem >= sizeof(exit_47) - 1    && memeq(scan + i, exit_47,    sizeof(exit_47)    - 1)) { t->alt_screen = false; i += (sizeof(exit_47)    - 2); continue; }
    if (rem >= sizeof(enter_1047) - 1 && memeq(scan + i, enter_1047, sizeof(enter_1047) - 1)) { t->alt_screen = true;  i += (sizeof(enter_1047) - 2); continue; }
    if (rem >= sizeof(exit_1047) - 1  && memeq(scan + i, exit_1047,  sizeof(exit_1047)  - 1)) { t->alt_screen = false; i += (sizeof(exit_1047)  - 2); continue; }
  }

  /* Save trailing bytes for next update. */
  const size_t TAIL_MAX = 64;
  size_t new_tail = scan_len < TAIL_MAX ? scan_len : TAIL_MAX;
  memcpy(t->tail, scan + (scan_len - new_tail), new_tail);
  t->tail_len = new_tail;
}

/* -----------------------------------------------------------------------------
 * Overlay line rendering (used during CAPTURE)
 * -------------------------------------------------------------------------- */

/*
overlay_draw(text):
Draw a status line on the last terminal row using:
  ESC[s      save cursor
  ESC[row;1H move to last row
  ESC[K      clear to end of line
  text
  ESC[u      restore cursor

If we cannot detect terminal rows, falls back to carriage return + clear line.
*/
static void overlay_draw(const char *text) {
  struct winsize ws;
  int row = 0;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
    row = (int)ws.ws_row;
  } else {
    row = 0;
  }

  if (row > 0) {
    char buf[2048];
    snprintf(buf, sizeof(buf), "\x1b[s\x1b[%d;1H\x1b[K%s\x1b[u", row, text ? text : "");
    (void)write_all(STDOUT_FILENO, buf, strlen(buf));
  } else {
    const char *clr = "\r\x1b[K";
    (void)write_all(STDOUT_FILENO, clr, strlen(clr));
    if (text) (void)write_all(STDOUT_FILENO, text, strlen(text));
  }
}

static void overlay_clear(void) { overlay_draw(""); }

/* -----------------------------------------------------------------------------
 * Local pipeline execution
 * -------------------------------------------------------------------------- */

/*
Run a user-provided pipeline string locally, feeding capture_fd as stdin.
We fork and exec /bin/sh -c "<pipeline>".
Returns waitpid status (shell exit code in WEXITSTATUS()) or -1 on error.
*/
static int run_local_pipeline_with_stdin_fd(int capture_fd, const char *pipeline) {
  pid_t p = fork();
  if (p < 0) return -1;

  if (p == 0) {
    /* Child: connect captured output to stdin and exec sh -c. */
    dup2(capture_fd, STDIN_FILENO);
    execl("/bin/sh", "sh", "-c", pipeline, (char *)NULL);
    _exit(127);
  }

  /* Parent: wait for pipeline to complete. */
  int status = 0;
  for (;;) {
    pid_t w = waitpid(p, &status, 0);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    break;
  }
  return status;
}

/* -----------------------------------------------------------------------------
 * Options: CLI + ~/.warpzonerc
 * -------------------------------------------------------------------------- */

typedef struct {
  const char *shell_path; /* execv() target for the PTY child */
  char      **shell_args; /* optional args (repeatable) */
  int         shell_argc;

  long        gap_ms;      /* quiet-gap end condition (ms) */
  bool        manual_end;  /* if true, disable quiet-gap ending */

  long long   max_bytes;   /* capture size guardrail; <=0 disables */
  long        max_seconds; /* capture time guardrail; <=0 disables */
} options_t;

static void opts_init(options_t *o) {
  memset(o, 0, sizeof(*o));
  o->shell_path = "/bin/bash";
  o->gap_ms = 200;
  o->manual_end = false;
  o->max_bytes = 0;
  o->max_seconds = 0;
}

/* Append a shell arg to options, owning a strdup() of the arg. */
static void opts_push_shell_arg(options_t *o, const char *arg) {
  if (!arg) return;
  int newc = o->shell_argc + 1;
  char **nv = (char **)realloc(o->shell_args, sizeof(char *) * (size_t)newc);
  if (!nv) die("out of memory");
  o->shell_args = nv;
  o->shell_args[o->shell_argc] = strdup(arg);
  if (!o->shell_args[o->shell_argc]) die("out of memory");
  o->shell_argc = newc;
}

static void opts_free(options_t *o) {
  if (!o) return;
  for (int i = 0; i < o->shell_argc; i++) free(o->shell_args[i]);
  free(o->shell_args);
}

/* Simple whitespace trimming helpers for rc parsing. */
static char *ltrim(char *s) {
  while (s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
  return s;
}
static void rtrim_inplace(char *s) {
  if (!s) return;
  size_t n = strlen(s);
  while (n > 0) {
    char c = s[n - 1];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { s[n - 1] = '\0'; n--; }
    else break;
  }
}
static bool parse_bool(const char *v, bool *out) {
  if (!v || !out) return false;
  if (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on")) { *out = true; return true; }
  if (!strcasecmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "no")  || !strcasecmp(v, "off")) { *out = false; return true; }
  return false;
}

/*
Load ~/.warpzonerc if present.
Key/value format: key=value (with whitespace tolerated).
Supported keys:
  shell, shell_arg, gap_ms, manual_end, max_bytes, max_seconds
*/
static void load_rc_file(options_t *o) {
  const char *home = getenv("HOME");
  if (!home || !home[0]) return;

  char path[1024];
  snprintf(path, sizeof(path), "%s/.warpzonerc", home);

  FILE *f = fopen(path, "r");
  if (!f) return;

  char line[2048];
  while (fgets(line, sizeof(line), f)) {
    char *s = ltrim(line);
    rtrim_inplace(s);
    if (*s == '\0' || *s == '#') continue;

    char *eq = strchr(s, '=');
    if (!eq) continue;
    *eq = '\0';
    char *key = ltrim(s);
    rtrim_inplace(key);
    char *val = ltrim(eq + 1);
    rtrim_inplace(val);

    if (!strcasecmp(key, "shell")) {
      if (*val) o->shell_path = strdup(val);
    } else if (!strcasecmp(key, "shell_arg")) {
      if (*val) opts_push_shell_arg(o, val);
    } else if (!strcasecmp(key, "gap_ms")) {
      long v = strtol(val, NULL, 10);
      if (v >= 0) o->gap_ms = v;
    } else if (!strcasecmp(key, "manual_end")) {
      bool b;
      if (parse_bool(val, &b)) o->manual_end = b;
    } else if (!strcasecmp(key, "max_bytes")) {
      long long v = strtoll(val, NULL, 10);
      if (v >= 0) o->max_bytes = v;
    } else if (!strcasecmp(key, "max_seconds")) {
      long v = strtol(val, NULL, 10);
      if (v >= 0) o->max_seconds = v;
    }
  }

  fclose(f);
}

static void print_help(FILE *out, const char *prog) {
  fprintf(out,
    "warpzone %s\n"
    "\n"
    "A PTY wrapper that lets you capture command output and pipe it through a LOCAL shell pipeline on demand.\n"
    "\n"
    "USAGE\n"
    "  %s [options]\n"
    "\n"
    "INTERACTIVE KEYS\n"
    "  Ctrl-]      Enter PIPE_ENTRY (prints a green '|')\n"
    "  PIPE_ENTRY:\n"
    "    Enter     Commit pipeline: sends Enter to remote, then begins CAPTURE\n"
    "    Ctrl-C    Cancel PIPE_ENTRY\n"
    "    Ctrl-]    Cancel PIPE_ENTRY\n"
    "    Backspace Edit pipeline; backspace over '|' cancels PIPE_ENTRY\n"
    "  CAPTURE:\n"
    "    Ctrl-]    End capture immediately\n"
    "\n"
    "CAPTURE END CONDITIONS\n"
    "  By default, capture ends when PTY output has been quiet for --gap-ms milliseconds.\n"
    "  You can disable gap-ending with --manual-end and end capture only via Ctrl-] or guardrails.\n"
    "\n"
    "OPTIONS\n"
    "  --shell PATH           Program to exec inside the PTY (default: /bin/bash)\n"
    "  --shell-arg ARG        Add an argument for the PTY child program (repeatable).\n"
    "                         If no --shell-arg is provided, warpzone adds '-i' by default.\n"
    "  --gap-ms N             Quiet-gap in milliseconds to end capture (default: 200). 0 disables gap-ending.\n"
    "  --manual-end           Disable quiet-gap ending (capture ends via Ctrl-] or guardrails only).\n"
    "  --max-bytes N          End capture after N bytes captured (0 disables; default: 0).\n"
    "  --max-seconds N        End capture after N seconds (0 disables; default: 0).\n"
    "  --version, -V          Print version and exit.\n"
    "  --help, -h             Show this help and exit.\n"
    "\n"
    "RC FILE: ~/.warpzonerc\n"
    "  If present, warpzone reads ~/.warpzonerc at startup before parsing CLI args.\n"
    "  CLI args override values loaded from the rc file.\n"
    "\n"
    "  FILE FORMAT\n"
    "    - UTF-8 text file\n"
    "    - One setting per line: key=value\n"
    "    - Leading/trailing whitespace around keys/values is allowed\n"
    "    - Blank lines are ignored\n"
    "    - Lines starting with '#' are comments\n"
    "\n"
    "  SUPPORTED KEYS\n"
    "    shell=PATH            (string)  Path to PTY child program (e.g., /bin/zsh)\n"
    "    shell_arg=ARG         (string)  Adds one argument (repeat this line for multiple args)\n"
    "    gap_ms=N              (int >=0) Quiet-gap end condition in milliseconds\n"
    "    manual_end=BOOL       (bool)    true/false/yes/no/on/off/1/0\n"
    "    max_bytes=N           (int >=0) Capture guardrail in bytes\n"
    "    max_seconds=N         (int >=0) Capture guardrail in seconds\n"
    "\n"
    "  EXAMPLE ~/.warpzonerc\n"
    "    # Use zsh and disable gap-ending\n"
    "    shell=/bin/zsh\n"
    "    shell_arg=-i\n"
    "    manual_end=true\n"
    "    max_seconds=10\n"
    "    max_bytes=1048576\n"
    "\n"
    "NOTES / LIMITATIONS\n"
    "  - PIPE_ENTRY is disabled while the child is in the alternate screen buffer (e.g., vim/less/top).\n"
    "  - During CAPTURE, normal keystrokes are ignored; Ctrl-] always ends capture.\n"
    "\n",
    WARPZONE_VERSION, prog);
}

/*
Parse CLI args.
CLI overrides rc defaults.
*/

static void parse_args(options_t *o, int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];

    if (!strcmp(a, "--shell")) {
      if (i + 1 >= argc) die("--shell requires a path");
      o->shell_path = argv[++i];

    } else if (!strcmp(a, "--shell-arg")) {
      if (i + 1 >= argc) die("--shell-arg requires a value");
      opts_push_shell_arg(o, argv[++i]);

    } else if (!strcmp(a, "--gap-ms")) {
      if (i + 1 >= argc) die("--gap-ms requires a number");
      o->gap_ms = strtol(argv[++i], NULL, 10);
      if (o->gap_ms < 0) die("--gap-ms must be >= 0");

    } else if (!strcmp(a, "--manual-end")) {
      o->manual_end = true;

    } else if (!strcmp(a, "--max-bytes")) {
      if (i + 1 >= argc) die("--max-bytes requires a number");
      o->max_bytes = strtoll(argv[++i], NULL, 10);
      if (o->max_bytes < 0) die("--max-bytes must be >= 0");

    } else if (!strcmp(a, "--max-seconds")) {
      if (i + 1 >= argc) die("--max-seconds requires a number");
      o->max_seconds = strtol(argv[++i], NULL, 10);
      if (o->max_seconds < 0) die("--max-seconds must be >= 0");

    } else if (!strcmp(a, "--version") || !strcmp(a, "-V")) {
      fprintf(stdout, "warpzone %s\n", WARPZONE_VERSION);
      exit(0);

    } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
      print_help(stderr, argv[0]);
      exit(0);

    } else {
      die("Unknown argument: %s (try --help)", a);
    }
  }
}


/*
Build argv for execv(opt.shell_path, argv).
If the user did not provide shell args, we default to "-i" for an interactive shell.
*/
static char **build_shell_argv(const options_t *o) {
  const char *argv0 = basename_c(o->shell_path);
  bool user_provided_args = (o->shell_argc > 0);
  int extra_i = user_provided_args ? 0 : 1;

  int argc_total = 1 + o->shell_argc + extra_i + 1;
  char **av = (char **)calloc((size_t)argc_total, sizeof(char *));
  if (!av) die("out of memory");

  int k = 0;
  av[k++] = strdup(argv0);
  if (!av[0]) die("out of memory");

  if (!user_provided_args) {
    av[k++] = strdup("-i");
    if (!av[k-1]) die("out of memory");
  } else {
    for (int i = 0; i < o->shell_argc; i++) {
      av[k++] = strdup(o->shell_args[i]);
      if (!av[k-1]) die("out of memory");
    }
  }
  av[k] = NULL;
  return av;
}

static void free_shell_argv(char **av) {
  if (!av) return;
  for (int i = 0; av[i]; i++) free(av[i]);
  free(av);
}

/* -----------------------------------------------------------------------------
 * CAPTURE overlay sparkline (throughput visualization)
 * -------------------------------------------------------------------------- */

#define SPARK_LEN 40

/*
spark_t:
A simple fixed-length ring buffer of “activity levels” based on bytes per poll tick.
Rendered as Unicode block “sparks” for quick visual feedback while capturing.
*/
typedef struct {
  unsigned char levels[SPARK_LEN];
  size_t idx;
  bool initialized;
} spark_t;

static void spark_init(spark_t *s) { memset(s, 0, sizeof(*s)); s->initialized = true; }

/*
Convert bytes observed this tick into a level 0..7 (roughly log-ish),
store into ring buffer.
*/
static void spark_push(spark_t *s, size_t bytes_this_tick) {
  if (!s->initialized) spark_init(s);
  unsigned char lvl = 0;
  if (bytes_this_tick == 0) lvl = 0;
  else if (bytes_this_tick < 16) lvl = 1;
  else if (bytes_this_tick < 64) lvl = 2;
  else if (bytes_this_tick < 256) lvl = 3;
  else if (bytes_this_tick < 1024) lvl = 4;
  else if (bytes_this_tick < 4096) lvl = 5;
  else if (bytes_this_tick < 16384) lvl = 6;
  else lvl = 7;
  s->levels[s->idx % SPARK_LEN] = lvl;
  s->idx++;
}

/*
Render the last SPARK_LEN values to a UTF-8 string.
Note: this uses Unicode block characters; if you want ASCII-only, we can swap glyphs.
*/
static void spark_render(const spark_t *s, char *out, size_t outsz) {
  if (!out || outsz == 0) return;
  out[0] = '\0';

  size_t pos = 0;
  size_t base = (s->idx >= SPARK_LEN) ? (s->idx - SPARK_LEN) : 0;

  for (size_t i = 0; i < SPARK_LEN; i++) {
    size_t idx = (base + i) % SPARK_LEN;
    unsigned char lvl = s->levels[idx];
    if (lvl > 7) lvl = 7;

    const char *glyph = "▁";
    switch (lvl) {
      case 0: glyph="▁"; break; case 1: glyph="▂"; break; case 2: glyph="▃"; break; case 3: glyph="▄"; break;
      case 4: glyph="▅"; break; case 5: glyph="▆"; break; case 6: glyph="▇"; break; case 7: glyph="█"; break;
    }
    size_t gl = strlen(glyph);
    if (pos + gl + 1 < outsz) {
      memcpy(out + pos, glyph, gl);
      pos += gl;
    }
  }
  out[pos] = '\0';
}

/* -----------------------------------------------------------------------------
 * PIPE_ENTRY mini editor helpers
 * -------------------------------------------------------------------------- */

/* Print green '|' marker to indicate local pipeline input. */
static void print_green_pipe_marker(void) {
  const char *mark = "\033[32m|\033[39m";
  (void)write_all(STDOUT_FILENO, mark, strlen(mark));
}

/* Backspace sequence that visually erases one character on terminal. */
static void erase_one_char_left(void) {
  const char bs_seq[] = "\b \b";
  (void)write_all(STDOUT_FILENO, bs_seq, sizeof(bs_seq) - 1);
}

/* -----------------------------------------------------------------------------
 * State machine
 * -------------------------------------------------------------------------- */

/*
Modes:
- PASSTHRU: stdin -> PTY, PTY -> stdout (normal terminal experience)
- PIPE_ENTRY: capture keystrokes locally to build a pipeline command line
- CAPTURE: PTY output saved to tempfile; user input mostly ignored
*/
typedef enum {
  WZ_MODE_PASSTHRU = 0,
  WZ_MODE_PIPE_ENTRY,
  WZ_MODE_CAPTURE
} wz_mode_t;

int main(int argc, char **argv) {
  /* ----- options ----- */
  options_t opt;
  opts_init(&opt);
  load_rc_file(&opt);
  parse_args(&opt, argc, argv);

  /* ----- set stdin raw mode (for single-byte key handling) ----- */
  if (tcgetattr(STDIN_FILENO, &g_stdin_orig) != 0) {
    die("tcgetattr(stdin) failed: %s", strerror(errno));
  }
  atexit(restore_terminal);

  g_stdin_raw = g_stdin_orig;
  cfmakeraw(&g_stdin_raw);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &g_stdin_raw) != 0) {
    die("tcsetattr(stdin raw) failed: %s", strerror(errno));
  }

  parent_setup_signal_behavior();

  {
    const char *banner = "\r\n\033[32mWelcome to Warp Zone!\033[39m\r\n";
    (void)write_all(STDOUT_FILENO, banner, strlen(banner));
  }

  /* ----- create PTY master/slave ----- */
  int fdm = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fdm < 0) die("posix_openpt failed: %s", strerror(errno));
  if (grantpt(fdm) != 0) die("grantpt failed: %s", strerror(errno));
  if (unlockpt(fdm) != 0) die("unlockpt failed: %s", strerror(errno));

  char *slave_name = ptsname(fdm);
  if (!slave_name) die("ptsname failed: %s", strerror(errno));

  int fds = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fds < 0) die("open(slave) failed: %s", strerror(errno));

  g_fdm = fdm;

  /* ----- SIGCHLD / SIGWINCH ----- */
  struct sigaction sch = {0};
  sch.sa_handler = sigchld_handler;
  sigemptyset(&sch.sa_mask);
  sch.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sch, NULL);

  struct sigaction sw = {0};
  sw.sa_handler = sigwinch_handler;
  sigemptyset(&sw.sa_mask);
  sw.sa_flags = SA_RESTART;
  sigaction(SIGWINCH, &sw, NULL);

  /* Initial window size sync. */
  sync_winsz();
  {
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
      (void)ioctl(fds, TIOCSWINSZ, &ws);
    }
  }

  /* Build argv for the PTY child. */
  char **child_argv = build_shell_argv(&opt);

  /* ----- fork PTY child ----- */
  pid_t pid = fork();
  if (pid < 0) die("fork failed: %s", strerror(errno));

  if (pid == 0) {
    /*
    Child process:
    - New session (setsid) so we can acquire controlling terminal
    - Set slave PTY as controlling tty
    - Wire stdio to PTY slave
    - Restore cooked terminal for the child
    - exec the shell
    */
    signal(SIGTTOU, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP,  SIG_DFL);

    if (setsid() < 0) _exit(1);

#ifdef TIOCSCTTY
    (void)ioctl(fds, TIOCSCTTY, 0);
#endif

    (void)setpgid(0, 0);

    close(fdm);
    dup2(fds, STDIN_FILENO);
    dup2(fds, STDOUT_FILENO);
    dup2(fds, STDERR_FILENO);
    if (fds > 2) close(fds);

    /* Give child process group control of terminal. */
    (void)tcsetpgrp(STDIN_FILENO, getpgrp());
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_stdin_orig);

    if (getenv("TERM") == NULL) {
      setenv("TERM", "xterm-256color", 0);
    }

    execv(opt.shell_path, child_argv);
    _exit(127);
  }

  /* Parent process: keep PTY master, close slave. */
  g_child_pid = pid;
  close(fds);

  wz_mode_t mode = WZ_MODE_PASSTHRU;

  /* Track alternate screen state from PTY output. */
  tty_features_t ttyf;
  ttyfeat_init(&ttyf);

  /* PIPE_ENTRY local edit buffer */
  char   pipe_argument[1024];
  size_t pipe_len = 0;
  memset(pipe_argument, 0, sizeof(pipe_argument));

  /* CAPTURE state */
  int   capture_fd = -1;     /* underlying temp file fd */
  FILE *capture_fp = NULL;   /* stdio wrapper for capture_fd */
  bool  force_end_capture = false;

  struct timeval capture_start = {0};
  struct timeval last_child_output = {0};
  struct timeval last_overlay_draw = {0};

  size_t total_captured = 0;
  size_t bytes_this_tick = 0;

  spark_t spark;
  spark_init(&spark);
  gettimeofday(&last_overlay_draw, NULL);

  /* poll() setup: stdin + PTY master. */
  struct pollfd pfds[2];
  pfds[0].fd = STDIN_FILENO;
  pfds[0].events = POLLIN;
  pfds[1].fd = fdm;
  pfds[1].events = POLLIN;

  /* =========================
   * Main event loop
   * ========================= */
  for (;;) {
    /* If child exited, break out cleanly. */
    if (g_sigchld_flag) {
      g_sigchld_flag = 0;
      if (reap_children_and_check_main()) {
        overlay_clear();
        print_info_line("child exited");
        break;
      }
    }

    /*
    Forward pending signals only in PASSTHRU.
    In PIPE_ENTRY/CAPTURE, we drop them because user is “in local UI”
    and we don’t want to surprise the child (and CAPTURE ignores keystrokes).
    */
    if (mode == WZ_MODE_PASSTHRU && g_pending_sig_mask) {
      sig_atomic_t m = g_pending_sig_mask;
      g_pending_sig_mask = 0;

      if (m & PEND_INT)  forward_signal_to_pty_fg(SIGINT);
      if (m & PEND_QUIT) forward_signal_to_pty_fg(SIGQUIT);
      if (m & PEND_TERM) forward_signal_to_pty_fg(SIGTERM);
      if (m & PEND_HUP)  forward_signal_to_pty_fg(SIGHUP);
      if (m & PEND_TSTP) forward_signal_to_pty_fg(SIGTSTP);
      if (m & PEND_CONT) forward_signal_to_pty_fg(SIGCONT);
    } else if (g_pending_sig_mask) {
      g_pending_sig_mask = 0;
    }

    int prc = poll(pfds, 2, POLL_TICK_MS);
    if (prc < 0) {
      if (errno == EINTR) continue;
      die("poll failed: %s", strerror(errno));
    }

    bytes_this_tick = 0;

    /* ----- PTY output handling ----- */
    if (pfds[1].revents & POLLIN) {
      unsigned char out[4096];
      ssize_t n = read(fdm, out, sizeof(out));
      if (n > 0) {
        /* Track alt-screen enter/exit sequences. */
        ttyfeat_update(&ttyf, out, (size_t)n);

        if (mode == WZ_MODE_CAPTURE) {
          /* In CAPTURE, we save PTY output but do not print it. */
          fwrite(out, 1, (size_t)n, capture_fp);
          fflush(capture_fp);
          total_captured += (size_t)n;
          bytes_this_tick += (size_t)n;
        } else {
          /* Otherwise, PTY output is displayed normally. */
          (void)write_all(STDOUT_FILENO, out, (size_t)n);
        }

        /* Any output resets the “idle gap” timer. */
        gettimeofday(&last_child_output, NULL);
      }
    }

    /* ----- stdin handling ----- */
    if (pfds[0].revents & POLLIN) {
      unsigned char buf[256];
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n > 0) {
        for (ssize_t i = 0; i < n; i++) {
          unsigned char ch = buf[i];

          /* CAPTURE: ignore typing except WARP_KEY ends capture. */
          if (mode == WZ_MODE_CAPTURE) {
            if (ch == WARP_KEY) force_end_capture = true;
            continue;
          }

          /* PASSTHRU: send bytes straight to PTY unless WARP_KEY triggers PIPE_ENTRY. */
          if (mode == WZ_MODE_PASSTHRU) {
            if (ch == WARP_KEY) {
              if (ttyf.alt_screen) {
                /* Prevent injecting marker into full-screen apps. */
                (void)write_all(STDOUT_FILENO, "\a", 1);
                print_info_line("pipe mode disabled in alternate screen (exit the full-screen app first)");
                continue;
              }
              mode = WZ_MODE_PIPE_ENTRY;
              pipe_len = 0;
              pipe_argument[0] = '\0';
              print_green_pipe_marker();
              continue;
            }

            (void)write_all(fdm, &ch, 1);
            continue;
          }

          /* PIPE_ENTRY: local editing for pipeline command. */
          if (mode == WZ_MODE_PIPE_ENTRY) {
            /* Commit on Enter: send Enter to PTY, then begin CAPTURE. */
            if (ch == '\r' || ch == '\n') {
              (void)write_all(STDOUT_FILENO, "\r\n", 2);

              pipe_argument[pipe_len] = '\0';
              if (pipe_len == 0) {
                /* Empty pipeline: just exit PIPE_ENTRY. */
                mode = WZ_MODE_PASSTHRU;
                continue;
              }

              /* Send CR to the remote side to run the command. */
              const char cr = '\r';
              (void)write_all(fdm, &cr, 1);

              /* Create temp file for capture, unlink it immediately (anonymous). */
              char tmpl[] = "/tmp/warpzoneXXXXXX";
              capture_fd = mkstemp(tmpl);
              if (capture_fd < 0) {
                print_info_line("mkstemp failed; aborting capture");
                mode = WZ_MODE_PASSTHRU;
                continue;
              }
              unlink(tmpl);
              fcntl(capture_fd, F_SETFD, FD_CLOEXEC);

              capture_fp = fdopen(capture_fd, "w+");
              if (!capture_fp) {
                close(capture_fd);
                capture_fd = -1;
                print_info_line("fdopen failed; aborting capture");
                mode = WZ_MODE_PASSTHRU;
                continue;
              }

              /* Initialize capture state. */
              total_captured = 0;
              force_end_capture = false;
              spark_init(&spark);

              gettimeofday(&capture_start, NULL);
              gettimeofday(&last_child_output, NULL);
              gettimeofday(&last_overlay_draw, NULL);

              mode = WZ_MODE_CAPTURE;
              continue;
            }

            /* Cancel PIPE_ENTRY on Ctrl-C or Ctrl-]. */
            if (ch == 0x03 || ch == WARP_KEY) {
              while (pipe_len > 0) { pipe_len--; erase_one_char_left(); }
              erase_one_char_left(); /* erase marker */
              pipe_argument[0] = '\0';
              mode = WZ_MODE_PASSTHRU;
              continue;
            }

            /* Backspace edits; backspace over marker cancels. */
            if (ch == 0x7f || ch == 0x08) {
              if (pipe_len > 0) {
                pipe_len--;
                pipe_argument[pipe_len] = '\0';
                erase_one_char_left();
              } else {
                erase_one_char_left(); /* marker cancels */
                pipe_argument[0] = '\0';
                mode = WZ_MODE_PASSTHRU;
              }
              continue;
            }

            /* Refuse other control chars (beep). */
            if (ch < 0x20) {
              (void)write_all(STDOUT_FILENO, "\a", 1);
              continue;
            }

            /* Append printable characters to pipeline buffer. */
            if (pipe_len + 1 < sizeof(pipe_argument)) {
              pipe_argument[pipe_len++] = (char)ch;
              pipe_argument[pipe_len] = '\0';
              (void)write_all(STDOUT_FILENO, &ch, 1);
            } else {
              /* Buffer full (beep). */
              (void)write_all(STDOUT_FILENO, "\a", 1);
            }
            continue;
          }
        }
      }
    }

    /* ----- CAPTURE overlay + termination conditions ----- */
    if (mode == WZ_MODE_CAPTURE) {
      spark_push(&spark, bytes_this_tick);

      struct timeval now;
      gettimeofday(&now, NULL);

      /* Redraw overlay at ~5Hz to reduce flicker. */
      if (elapsed_secs(&last_overlay_draw, &now) >= 0.2) {
        double idle = elapsed_secs(&last_child_output, &now);
        double since_start = elapsed_secs(&capture_start, &now);

        char sparkbuf[256];
        spark_render(&spark, sparkbuf, sizeof(sparkbuf));

        char line[1024];
        snprintf(line, sizeof(line),
                 "\033[2m[warpzone] capture: bytes=%zu idle=%.2fs age=%.2fs mode=%s  %s  (Ctrl-]=end)\033[0m",
                 total_captured, idle, since_start, opt.manual_end ? "manual" : "gap", sparkbuf);
        overlay_draw(line);
        last_overlay_draw = now;
      }

      bool end_now = false;

      /* Immediate user-requested termination. */
      if (force_end_capture) end_now = true;

      /* Quiet-gap ending, unless manual_end enabled. */
      if (!end_now && !opt.manual_end) {
        double gap = (double)opt.gap_ms / 1000.0;
        if (elapsed_secs(&last_child_output, &now) > gap) end_now = true;
      }

      /* Guardrail: max bytes captured. */
      if (!end_now && opt.max_bytes > 0) {
        if ((long long)total_captured >= opt.max_bytes) end_now = true;
      }

      /* Guardrail: max capture duration. */
      if (!end_now && opt.max_seconds > 0) {
        if (elapsed_secs(&capture_start, &now) >= (double)opt.max_seconds) end_now = true;
      }

      if (end_now) {
        overlay_clear();

        /* Prepare capture file for reading by the pipeline. */
        fflush(capture_fp);
        fseek(capture_fp, 0, SEEK_SET);

        /*
        Important: temporarily restore cooked terminal mode while running local pipeline.
        This prevents raw-mode weirdness in downstream tools (pager, grep, etc.).
        */
        tcsetattr(STDIN_FILENO, TCSANOW, &g_stdin_orig);

        /* Present pipeline output bracketed by green rules. */
        print_green_hr();
        (void)run_local_pipeline_with_stdin_fd(capture_fd, pipe_argument);
        print_green_hr();

        /* Re-enter raw mode for continued interactive PTY use. */
        tcsetattr(STDIN_FILENO, TCSANOW, &g_stdin_raw);

        /* Tear down capture resources. */
        fclose(capture_fp);
        capture_fp = NULL;
        capture_fd = -1;

        force_end_capture = false;
        mode = WZ_MODE_PASSTHRU;

        /*
        Send a CR to the PTY after returning from local pipeline.
        This nudges the remote prompt cleanly onto a fresh line if needed.
        */
        const char cr = '\r';
        (void)write_all(fdm, &cr, 1);
      }
    }
  }

  /* Cleanup on exit. */
  overlay_clear();
  free_shell_argv(child_argv);
  opts_free(&opt);
  return 0;
}
