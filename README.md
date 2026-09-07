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
- The stats and rate counter timers of every Osmocom daemon run, on top of
  the timerfd emulation described below (since v0.2.1)

## What doesn't work yet

- `grgsm_scanner` fails at `import osmosdr` (gr-osmosdr needs a separate port)
- `grgsm_livemon` / `grgsm_livemon_headless` need manual `.grc` compilation
  post-install plus additional patches to the flowgraph
- `grgsm_capture` not installed (missing from `apps/` in this fork's version)
- Linux-only functions are no-op at runtime (CPU affinity VTY, Frame Relay
  GPRS transport, TUN device, TCP socket statistics, netlink). This does not
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

> **Note**: Use tag v0.2.6 or master. v0.2.1 emulates timerfd for full stats
> support; v0.2.2 adds the `osmo_sock_local_ip()` fix that osmo-mgw and
> libosmo-mgcp-client need; v0.2.3 adds the second cpu-sched stub that
> osmo-trx and osmo-bts link against; v0.2.4 makes `timerfd_settime()`
> reset the pending expiration count, without which osmo-trx spins at
> 100 % CPU after POWERON. Tag v0.2.0 is functional but disables the stats
> subsystem. v0.1.0 remains deprecated (null dereference at daemon startup). v0.2.5 fixes the sockaddr length
> `osmo_sock_init_osa()` passes to `bind()` and `connect()`, which osmo-pcu
> and every other NS2 user need; v0.2.6 gives `osmo_tundev` and `osmo_netdev`
> a real Darwin backend (utun, ioctl, `PF_ROUTE`) for osmo-ggsn.

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

## Building with SCTP support

`install.sh` configures with `--disable-libsctp`. That is the right choice
for gr-gsm and for passive scanning, which never open an SCTP socket, and it
was the only choice available when this port was made: macOS has no kernel
SCTP.

It is the wrong choice for the rest of the Osmocom stack. `--disable-libsctp`
compiles out `osmo_sock_init2_multiaddr`, `osmo_sock_init2_multiaddr2`,
`osmo_sock_multiaddr_add_local_addr` and `osmo_sock_multiaddr_del_local_addr`
while `osmocom/core/socket.h` goes on declaring all four. Nothing warns you.
The build above libosmocore fails instead, at a configure check that looks
like a fault in the component being built: libosmo-netif tests for
`osmo_sock_init2_multiaddr` and reports that libosmocore was built without
libsctp support.

[libsctp-compat](https://github.com/AndreiGosman/libsctp-compat-macos-arm64)
v0.2.0 or later supplies the missing `netinet/sctp.h` and `libsctp`. Install
it first, then rebuild libosmocore against it:

```bash
pkg-config --modversion libsctp     # expect 0.2.0 or later

cd build/libosmocore                # where install.sh put the source
./configure --prefix=$HOME/sdr-lab/local \
    --disable-doxygen --disable-pcsc --disable-systemd-logging \
    --disable-uring --disable-libmnl --enable-libsctp \
    CFLAGS="-include $PWD/darwin_compat.h -I$PWD/include"
make -j$(sysctl -n hw.ncpu) LDFLAGS="-Wl,-undefined,dynamic_lookup"
make install
```

Keep the whole `CFLAGS`. The `-I$PWD/include` is what lets configure find
`sys/timerfd.h` and define `HAVE_SYS_TIMERFD_H`. Without it the build still
succeeds, but `src/core/select.c` drops its `osmo_timerfd_*` wrappers, the
stats timers are gone again, and every daemon fails to link or to start
with an undefined `_osmo_timerfd_setup`. The symbol check at the end of
`install.sh` reports exactly that case.

Keep the `LDFLAGS`. It is not incidental to the SCTP build, it is what the
whole port needs: `libosmogb` refers to the `gprs_ns2_fr_*` symbols that the
Frame Relay patch removes, and to `bssgp_prim_cb`, which an application
supplies. `libosmocodec` needs it for an unrelated reason, an upstream one:
it calls `_talloc_free` and `_talloc_zero` but has no `$(TALLOC_LIBS)` in its
`LIBADD`, where ctrl, gb, gsm and isdn all have it. Linux does not notice,
because the symbols resolve transitively through `libosmocore.so`.

Check that the rebuild did what you wanted:

```bash
nm -gU $HOME/sdr-lab/local/lib/libosmocore.dylib | grep -c multiaddr   # expect 7
otool -L $HOME/sdr-lab/local/lib/libosmocore.dylib | grep sctp
```

Adding SCTP only adds symbols, so anything already built against this
libosmocore keeps working. gr-gsm was re-checked after the rebuild.

## Changes applied by install.sh

The port changes the upstream tree in two ways. The seven items below are
in-place edits that `install.sh` performs itself, because each is a
substitution or a file copy rather than a diff. The numbered series in
`patches/` follows them, and is described in the next section.

**0001-configure-disable-linux-only-features**
Adds `--disable-uring --disable-libmnl --disable-libsctp` to the configure
invocation. io_uring, netlink, and SCTP don't exist on Darwin.

**0002-exec.c-setresgid-setresuid-to-Darwin**
Replaces `setresgid(a,a,a)` calls with `setregid(a,a)` and `setresuid(a,a,a)`
with `setreuid(a,a)` in `src/core/exec.c`. Semantically equivalent for the
specific use case (all three arguments are the same value).

**0003-wrap-linux-only-sources**
Wraps in `#ifdef __linux__ ... #endif` for files that include Linux-only
headers or use Linux-only APIs:
- `src/core/netdev.c` (`linux/if.h`)
- `src/core/serial.c` (Linux ioctls)
- `src/core/stats_tcp.c` (`linux/tcp.h`)
- `src/core/tun.c` (`linux/if_tun.h`)
- `src/gb/gprs_ns2_fr.c` (`linux/if.h`, Frame Relay socket family)

`src/vty/cpu_sched_vty.c` used to be wrapped here too. It is handled by
`patches/005` instead, because wrapping it removes a public function that
every Osmocom daemon calls, so the file needs a replacement definition and
not only a guard.

**0004-add-darwin-stubs**
Adds `src/core/darwin_stubs.c` to `libosmocore_la_SOURCES` in
`src/core/Makefile.am`. The `darwin_stubs.c` file, which lives at the root of this repository,
exports no-op stubs for the public symbols of wrapped files:
`osmo_tcp_stats_config`, `osmo_stats_tcp_*`, `osmo_tundev_*`. Up to v0.2.0
it also stubbed `osmo_timerfd_*`; those are now the upstream functions, see
item 0007.

**0005-darwin-compat-header**
Adds `darwin_compat.h` at the root and includes it via CFLAGS `-include`.
Defines:
- `SO_PRIORITY=999` (Linux socket priority option, no-op on Darwin)
- `CLOCK_REALTIME_COARSE=100`, `CLOCK_MONOTONIC_COARSE=101`,
  `CLOCK_BOOTTIME=102` (dummy IDs that don't collide with the Darwin
  `_clock_id` enum)
- `gettid()` macro → `getpid()` (degraded multi-thread semantics, fine
  for logging)

**0006-LDFLAGS-dynamic-lookup-at-make**
`LDFLAGS="-Wl,-undefined,dynamic_lookup"` applied only at `make` time,
NOT at `configure`. Applying it at configure would cause false positives
in `gettid`/`setns`/`unshare` detection.

**0007-timerfd-emulation** (since v0.2.1)
Copies `darwin_timerfd.c` to `src/core/` and adds it to
`libosmocore_la_SOURCES`, copies `darwin_timerfd.h` to
`include/sys/timerfd.h`, and adds `-I$PWD/include` to the configure
`CFLAGS` so that the check for `sys/timerfd.h` succeeds. After `make
install` the header is copied to `$PREFIX/include/sys/timerfd.h` as well.
Details in the next section.

## timerfd emulation

Up to v0.2.0, `osmo_timerfd_setup()`, `osmo_timerfd_schedule()` and
`osmo_timerfd_disable()` were stubs returning -1. Every daemon then logged
two `stats.c` errors at startup and ran with its stats and rate counter
timers dead: no periodic flush, so statsd, Prometheus exporters and CTRL
polling saw nothing. The osmo-hlr `db_upgrade` test failed on exactly those
two lines.

Linux `timerfd_create()` returns a descriptor that becomes readable when the
timer expires; `read()` gives the number of expirations as a `uint64_t`, and
`stats.c` asserts that it reads exactly eight bytes. Darwin has no timerfd.
Its timing primitive is kqueue `EVFILT_TIMER`, but a kqueue descriptor only
reports readiness and cannot be read. `darwin_timerfd.c` therefore builds
each timer from three parts: a pipe, whose read end is the descriptor the
caller gets; a kqueue with an `EVFILT_TIMER`; and a worker thread that
waits on the kqueue and writes the expiration count into the pipe. The
result works with `select()`, `poll()`, `read()` and plain `close()`, so the
upstream `select.c` wrappers compile and run unchanged. The three functions
are exported from `libosmocore.dylib` under their Linux names; nothing on
Darwin defines them, so there is no collision.

What is emulated: `CLOCK_MONOTONIC` and `CLOCK_REALTIME`; `TFD_NONBLOCK`
and `TFD_CLOEXEC`; `it_value` as first expiration and `it_interval` as
period, kept separate (the first shot is a one-shot kevent, the worker then
arms a repeating one, which does not drift); disarm with a zero `it_value`;
one-shot with a zero `it_interval`; coalescing of missed expirations into
the count, which is what the "We missed N timers" notice in `stats.c`
reads; `TFD_TIMER_ABSTIME` by conversion to a delay at `settime()`;
`timerfd_gettime()` and the `old_value` argument; `close()`, detected
through `EV_EOF` on the write end; and `fork()`, through an `atfork` child
handler that rebuilds the kqueue and the worker, because neither survives a
fork on Darwin while the pipe does. The last point is what keeps the stats
timers alive after `osmo_daemonize()`.

What is not emulated: `TFD_TIMER_CANCEL_ON_SET` returns `EINVAL` (Darwin
has no notification for wall clock jumps; Osmocom does not use it). An
absolute `CLOCK_REALTIME` timer is converted once and does not follow later
clock changes.

Since v0.2.4, `settime()` also discards an expiration already written to
the pipe but not yet read, as Linux resets the count. Up to v0.2.3 the
count stayed in the pipe. For `stats.c` and `rate_ctr.c`, which `read()`
before rescheduling, that was at most one extra tick. osmo-trx's rate
counter timers disarm from the read callback without a `read()`, so the
descriptor stayed readable and the main thread spun at 100 % CPU from the
first POWERON on, logging "Main thread is updating Transceiver counters"
about 260000 times per second. The fix drains the read end under the
global lock with `FIONREAD`; the worker only writes with that lock held,
so the count is exact and the read cannot block. A read end inherited by
another process delays the release of the worker until that process closes
it too.

Two test programs are in `tests/`. `darwin_timerfd_test.c` exercises the
raw API from a `select()` loop: 100 ms periodic ticks, distinct first
expiration, coalescing, `gettime`, disarm, one-shot, close detection and
fork. `darwin_timerfd_osmo_fd_test.c` goes through `osmo_timerfd_setup()`
and `osmo_select_main()`, the path the daemons use.

```bash
cc tests/darwin_timerfd_test.c -o /tmp/tfd $(pkg-config --cflags --libs libosmocore)
DYLD_LIBRARY_PATH=$HOME/sdr-lab/local/lib /tmp/tfd          # expect PASSED (0 failures)

cc tests/darwin_timerfd_osmo_fd_test.c -o /tmp/tfd2 $(pkg-config --cflags --libs libosmocore talloc)
DYLD_LIBRARY_PATH=$HOME/sdr-lab/local/lib /tmp/tfd2         # expect 6 callbacks in 2.6 s
```

Verified end to end with the osmo-hlr testsuite (`db_upgrade` passes, 8 of
9 with one skipped), with a `stats reporter statsd` sending one datagram per
second, and with osmo-hlr started with `-D`, where the daemon child keeps
its worker thread and kqueue.

## Patches applied

The numbered series in `patches/` is applied by `install.sh` after the
in-place changes above and before `autoreconf`. Each file carries a
`git format-patch` compatible header describing the problem and the reasoning.
An already applied patch is skipped, so re-running `install.sh` over an
existing build tree is safe.

| Patch | Upstream file | Darwin issue | Fix |
|-------|---------------|--------------|-----|
| 001 | `src/core/socket.c` | `getaddrinfo()` answers `EAI_BADHINTS` for `SOCK_STREAM` with `IPPROTO_SCTP`, for every host and family, so no SCTP server reaches `bind()` | Resolve with an unspecified protocol, restore `IPPROTO_SCTP` in the results, as the file already does for glibc and `SOCK_RAW` |
| 002 | `include/osmocom/core/log2.h` | The bundled `static inline fls(unsigned int)` follows Darwin's non-static `fls(int)` from `<strings.h>`; glibc has no `fls()` | Use the libc `fls()` on Darwin, keep the bundled one elsewhere. Semantics checked to be identical |
| 003 | `include/osmocom/core/hash.h` | `__always_inline` is defined by glibc's `<sys/cdefs.h>` but not Darwin's, so the declaration fails to parse | Spell it `__attribute__((always_inline))`, the change upstream already made in `log2.h` |
| 004 | `include/osmocom/core/stats_tcp.h` | The prototypes name `struct osmo_fd` without declaring it, giving it prototype scope and breaking any definition in the same unit | Forward declare the type in the header |
| 005 | `src/vty/cpu_sched_vty.c` | Guarding the file for Linux removes `osmo_cpu_sched_vty_init()`, which every daemon calls, so linking fails | Guard it and add an `#else` no-op initialiser, leaving the `cpu-sched` node absent |
| 006 | `src/core/socket.c` | `osmo_sock_local_ip()` connects its dummy UDP socket to port 0 to learn the local address; Darwin and the BSDs reject that with `EADDRNOTAVAIL`, so the function fails for every remote. libosmo-mgcp-client then cannot build any MGCP message with SDP, and osmo-mgw cannot pick a local RTP address | Connect to port 9 instead. No packet is sent, so the port is irrelevant to the answer; Linux behaviour is unchanged (since v0.2.2) |
| 007 | `src/vty/cpu_sched_vty.c` | Patch 005 left out the second public function of the file, `osmo_cpu_sched_vty_apply_localthread()`, which every worker thread of osmo-trx and osmo-bts calls, so linking `osmo-trx-uhd` fails with an undefined symbol | Add it next to the no-op initialiser, returning 0 as the Linux code does when no policy matches the thread (since v0.2.3) |
| 008 | `src/core/socket.c` | `osmo_sock_init_osa()` passes `sizeof(struct osmo_sockaddr)`, the 128 byte union, to `bind()` and `connect()`. Linux accepts a namelen longer than the family's sockaddr; Darwin and the BSDs reject it with `EINVAL`. `gprs_ns2_ip_bind()` cannot bind its NS-VC UDP socket, so osmo-pcu exits right after the INFO_IND from osmo-bts, and osmo-sgsn and osmo-gbproxy would fail the same way | Use `osmo_sockaddr_size()`, which the header already provides for `sendto()` and returns the size for the family in use; Linux behaviour is unchanged (since v0.2.5) |
| 009 | `src/core/tun.c` | Written for the Linux tun driver: `/dev/net/tun`, `TUNSETIFF`, bare IP packets on `read()`/`write()`; Darwin has utun, a `PF_SYSTEM` control socket with a four byte address family in front of every packet | Add a utun backend next to the Linux one: connect the control socket, report the `utunN` name the kernel assigned, strip and prepend the family word with `readv()`/`writev()`; the rest of the file is unchanged and now builds on Darwin (since v0.2.6) |
| 010 | `src/core/netdev.c` | Interface management goes through rtnetlink (libmnl); without it every operation returns `-ENOTSUP`, and Darwin has no netlink at all | `#elif __APPLE__` branches call `darwin_netdev.c`: `SIOCAIFADDR`/`SIOCAIFADDR_IN6` for addresses, `SIOCSIFMTU`, `SIOCSIFFLAGS`, and `RTM_ADD` on a `PF_ROUTE` socket for routes; up/down and MTU callbacks fire from the setters since there is no link monitor (since v0.2.6) |

Patches 003, 004, 006 and 008 are not Darwin specific; 009 and 010 are
Darwin backends and would need a BSD generalisation before going upstream. All three are worth
sending upstream.

Patch 001 and the `osmo_tcp_stats_config` fix in `darwin_stubs.c` are what
make a daemon such as `osmo-stp` start at all: before them it died with
SIGSEGV inside `osmo_stats_init()`, and once past that it could not bind its
SCTP listener.

## Verifying the install

After `./install.sh`:

```bash
export PKG_CONFIG_PATH=$HOME/sdr-lab/local/lib/pkgconfig:$PKG_CONFIG_PATH
pkg-config --exists libosmocore && echo "libosmocore OK"
pkg-config --exists libosmogsm && echo "libosmogsm OK"

# Symbol check
nm -gU $HOME/sdr-lab/local/lib/libosmocore.dylib | grep osmo_tcp_stats_config
# should show _osmo_tcp_stats_config exported
nm -gU $HOME/sdr-lab/local/lib/libosmocore.dylib | grep -c timerfd
# should be 6: timerfd_create/settime/gettime and osmo_timerfd_setup/schedule/disable

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

**TUN device**: since v0.2.6 `osmo_tundev_*` open a utun (`PF_SYSTEM`
control socket, four byte family prefix handled inside) and `osmo_netdev_*`
set addresses, MTU, link state and routes with the BSD ioctls and a
`PF_ROUTE` socket (patches 009 and 010, `darwin_netdev.c`). Both need root,
as on Linux. The kernel names the interface `utunN`; a requested name is
reported back with the one assigned. There is no link monitor, so the
up/down and MTU callbacks fire from the setters. Up to v0.2.5 these were
no-op stubs. Verified without root: every call fails with `EPERM` and
returns cleanly; osmo-ggsn 1.15.0 starts, rejects PDP contexts for lack of
an interface, and answers GTP-C. A root run with an APN up is still to do.

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
