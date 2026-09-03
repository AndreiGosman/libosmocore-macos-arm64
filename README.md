# libosmocore-macos-arm64

Port of libosmocore (Osmocom Software) for macOS Apple Silicon (M1/M2/M3/M4),
delivered as a patch set applied on top of upstream. Unblocks native
compilation and runtime of **gr-gsm** and the Osmocom stack on Macs without
Docker or a Linux VM.

Tested on macOS Tahoe (Darwin 25.5.0), Apple Silicon M4, Homebrew 6.0.21,
GNU Radio 3.10.12, Python 3.14, libosmocore tag `1.14.2.4-2a26b`.

## Motivation

Upstream libosmocore (gitea.osmocom.org) is written almost exclusively for
Linux, with glibc-specific dependencies: `sys/timerfd.h`, `linux/if.h`,
`linux/tcp.h`, `cpu_set_t` + `sched_setaffinity`, `setresgid/setresuid`,
`SO_PRIORITY`, `CLOCK_REALTIME_COARSE/CLOCK_BOOTTIME`, `gettid()`, io_uring,
netlink via libmnl, SCTP.

No Homebrew formula exists. No official Osmocom tap. No publicly maintained
fork with macOS ARM64 support.

The current alternative is Docker or a UTM Ubuntu VM. Cost: no functional
USB passthrough for USRPs under Docker Desktop macOS, or the VM overhead
during interactive capture sessions.

This port solves native compilation and runtime with 21 patches. The main
one is a `darwin_stubs.c` that exports the public symbols of the Linux-only
files wrapped in `#ifdef __linux__`.

## What works

- `libosmocore.dylib` and all submodules (`libosmovty`, `libosmocodec`,
  `libosmogsm`, `libosmocoding`, `libosmoisdn`) compile and link
- Python module `gnuradio.gsm` importable in Python 3.14 from the
  Homebrew gnuradio venv
- `grgsm_decode` works for offline decoding of `.cfile` captures
- `gnuradio` namespace unified via `extend_path` between the local prefix
  and Homebrew (blocks, uhd, qtgui accessible alongside gsm)

## What doesn't work yet

- `grgsm_scanner` fails at `import osmosdr` (gr-osmosdr needs a separate port)
- `grgsm_livemon` / `grgsm_livemon_headless` need manual `.grc` compilation
  post-install plus additional patches to the flowgraph
- `grgsm_capture` not installed (missing from `apps/` in this fork's version)
- Linux-only functions are no-op at runtime (CPU affinity VTY, Frame Relay
  GPRS transport, TUN device, TCP stats via timerfd, netlink). This does not
  affect passive GSM decoding; it may affect advanced Osmocom features

## Prerequisites

macOS on Apple Silicon (M1/M2/M3/M4). Untested on Intel Macs.

Homebrew:
```
brew install cmake boost swig log4cpp cppunit pkg-config git \
             autoconf automake libtool talloc gnutls
```

Optional for gr-gsm afterward: `brew install gnuradio pybind11`, plus
`pygccxml` in the gnuradio venv.

## Usage

```bash
git clone https://github.com/AndreiGosman/libosmocore-macos-arm64.git
cd libosmocore-macos-arm64
./install.sh --prefix=$HOME/sdr-lab/local
```

Installs libosmocore + submodules into the specified prefix (default
`$HOME/sdr-lab/local`), without touching `/opt/homebrew` or the system.

Uninstall: `rm -rf $HOME/sdr-lab/local`.

For a full gr-gsm build on top of this (bkerler/gr-gsm fork), see the
companion `install_gr_gsm.sh` script (not part of this repository).

## Patch list

All patches live in `patches/` as text files applicable with `patch -p1`
or manually. `install.sh` applies them automatically.

**0001-configure-disable-linux-only-features.patch**
Adds `--disable-uring --disable-libmnl --disable-libsctp` to the configure
invocation. io_uring, netlink, and SCTP don't exist on Darwin.

**0002-exec.c-setresgid-setresuid-to-Darwin.patch**
Replaces `setresgid(a,a,a)` calls with `setregid(a,a)` and `setresuid(a,a,a)`
with `setreuid(a,a)` in `src/core/exec.c`. Semantically equivalent for the
specific use case (all three arguments are the same value).

**0003-wrap-linux-only-sources.patch**
Wraps in `#ifdef __linux__ ... #endif` for files that include Linux-only
headers or use Linux-only APIs:
- `src/vty/cpu_sched_vty.c` (`cpu_set_t`, `sched_setaffinity`)
- `src/core/netdev.c` (`linux/if.h`)
- `src/core/serial.c` (Linux ioctls)
- `src/core/stats_tcp.c` (`linux/tcp.h`)
- `src/core/tun.c` (`linux/if_tun.h`)
- `src/gb/gprs_ns2_fr.c` (`linux/if.h`, Frame Relay socket family)

**0004-add-darwin-stubs.patch**
Adds `src/core/darwin_stubs.c` to `libosmocore_la_SOURCES` in
`src/core/Makefile.am`. The `darwin_stubs.c` file (from this repo's `src/`)
exports no-op stubs for the public symbols of wrapped files:
`osmo_tcp_stats_config`, `osmo_stats_tcp_*`, `osmo_timerfd_*`,
`osmo_tundev_*`.

**0005-darwin-compat-header.patch**
Adds `darwin_compat.h` at the root and includes it via CFLAGS `-include`.
Defines:
- `SO_PRIORITY=999` (Linux socket priority option, no-op on Darwin)
- `CLOCK_REALTIME_COARSE=100`, `CLOCK_MONOTONIC_COARSE=101`,
  `CLOCK_BOOTTIME=102` (dummy IDs that don't collide with the Darwin
  `_clock_id` enum)
- `gettid()` macro → `getpid()` (degraded multi-thread semantics, fine
  for logging)

**0006-LDFLAGS-dynamic-lookup-at-make.patch**
`LDFLAGS="-Wl,-undefined,dynamic_lookup"` applied only at `make` time,
NOT at `configure`. Applying it at configure would cause false positives
in `gettid`/`setns`/`unshare` detection.

## Verifying the install

After `./install.sh`:

```bash
export PKG_CONFIG_PATH=$HOME/sdr-lab/local/lib/pkgconfig:$PKG_CONFIG_PATH
pkg-config --exists libosmocore && echo "libosmocore OK"
pkg-config --exists libosmogsm && echo "libosmogsm OK"

# Symbol check
nm -gU $HOME/sdr-lab/local/lib/libosmocore.dylib | grep osmo_tcp_stats_config
# should show _osmo_tcp_stats_config exported

# Dynamic linker check
otool -L $HOME/sdr-lab/local/lib/libosmocore.dylib
```

## Known limitations

**Timing precision**: `CLOCK_MONOTONIC_COARSE` and `CLOCK_BOOTTIME` return
EINVAL at runtime (they don't exist on Darwin). Osmocom code has a fallback
to standard `CLOCK_MONOTONIC` via `try/if` in `timer_clockgettime.c`, so the
effect is minor timer resolution degradation, not a crash.

**CPU affinity**: `osmo-cpu-sched` VTY commands have no effect (functions
are stubs). Impact: no CPU affinity control per thread at runtime via CLI.
Does not affect passive RX runs.

**Frame Relay GPRS**: `gprs_ns2_fr` wrapped out. Impact: no GPRS transport
over Frame Relay (rarely used anyway; TCP/UDP transport works normally).

**TUN device**: `osmo_tundev_*` are no-op. Impact: no TUN interface creation
from libosmocore directly on macOS. Not needed for most Osmocom use cases
(BTS, MSC, HLR).

**libosmocore TCP statistics**: `osmo_stats_tcp_*` are no-op. Impact: no
stats reporting via `stats_tcp`. Alternatives: stats via GSMTAP UDP or
via logging.

## What would be better upstream

Ideally, upstream libosmocore would accept the patches as `#ifdef __linux__`
inline conditionals rather than wrapping whole files. I expect upstream to
be reluctant, since portability is not in their declared scope.

Alternatively, an actively maintained fork (not just this snapshot) would
bring Osmocom on macOS to the level WiFi tooling already has (Kismet,
aircrack-ng, etc.).

## License

GPLv2+ (inherited from upstream libosmocore). Individual patches are under
the same license.

## Credits

Ported by Andrei Gosman, 2026. Patchset iterated with Claude (Anthropic).

Upstream libosmocore: the Osmocom community (https://osmocom.org).
Upstream gr-gsm fork: bkerler (https://github.com/bkerler/gr-gsm).
