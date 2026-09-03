# libosmocore-macos-arm64

Port de libosmocore (Osmocom Software) pentru macOS Apple Silicon (M1/M2/M3/M4),
livrat ca set de patch-uri aplicate peste upstream. Deblocheaza compilarea si
rularea nativa a lui **gr-gsm** si a stack-ului Osmocom pe MacBook fara VM/Docker.

Testat pe macOS Tahoe (Darwin 25.5.0), Apple Silicon M4, Homebrew 6.0.21,
GNU Radio 3.10.12, Python 3.14, libosmocore tag `1.14.2.4-2a26b`.

## Motivatie

libosmocore upstream (gitea.osmocom.org) este scris practic exclusiv pentru
Linux, cu dependinte glibc-specifice: `sys/timerfd.h`, `linux/if.h`,
`linux/tcp.h`, `cpu_set_t` + `sched_setaffinity`, `setresgid/setresuid`,
`SO_PRIORITY`, `CLOCK_REALTIME_COARSE/CLOCK_BOOTTIME`, `gettid()`, io_uring,
netlink via libmnl, SCTP.

Nu exista Homebrew formula. Nu exista tap Osmocom oficial. Nu exista fork
public mentenat cu suport macOS ARM64.

Alternativa curenta: Docker sau UTM Ubuntu VM. Costul: fara USB passthrough
functional pentru USRP-uri (Docker Desktop macOS), sau VM cu overhead
semnificativ pentru sesiuni interactive de captura.

Portul acesta rezolva compilarea si rularea nativa cu 21 patch-uri, dintre
care principalul e un `darwin_stubs.c` care exporta simbolurile publice ale
fisierelor Linux-only wrap-uite in `#ifdef __linux__`.

## Ce functioneaza

- `libosmocore.dylib` si toate submodulele (`libosmovty`, `libosmocodec`,
  `libosmogsm`, `libosmocoding`, `libosmoisdn`) compilate si link-abile
- Modulul Python `gnuradio.gsm` importabil in Python 3.14 din venv-ul
  gnuradio Homebrew
- `grgsm_decode` functional pentru decodare offline din capturi `.cfile`
- Namespace `gnuradio` unit prin `extend_path` intre prefix-ul local si
  Homebrew (blocks, uhd, qtgui accesibile impreuna cu gsm)

## Ce NU functioneaza

- `grgsm_scanner` cade la `import osmosdr` (gr-osmosdr necesar separat)
- `grgsm_livemon` / `grgsm_livemon_headless` necesita compilare manuala
  `.grc` post-install si patchuri suplimentare la flowgraph
- `grgsm_capture` neinstalat (nu exista in apps/ pentru versiunea forkului)
- Functiile Linux-only sunt no-op la runtime (CPU affinity VTY, Frame Relay
  transport GPRS, TUN device, TCP stats via timerfd, netlink). Nu afecteaza
  decodarea GSM passiva; pot afecta functionalitati Osmocom avansate

## Prerechizite

macOS pe Apple Silicon (M1/M2/M3/M4). Rularea pe Intel Mac neconfirmata.

Homebrew:
```
brew install cmake boost swig log4cpp cppunit pkg-config git \
             autoconf automake libtool talloc gnutls
```

Optional pentru gr-gsm ulterior: `brew install gnuradio pybind11`, plus
`pygccxml` in venv-ul gnuradio.

## Utilizare

```bash
git clone https://github.com/<username>/libosmocore-macos-arm64.git
cd libosmocore-macos-arm64
./install.sh --prefix=$HOME/sdr-lab/local
```

Instaleaza libosmocore + submodulele in prefix-ul specificat (default
`$HOME/sdr-lab/local`), fara sa polueze `/opt/homebrew` sau sistemul.

Pentru dezinstalare: `rm -rf $HOME/sdr-lab/local`.

Pentru build gr-gsm complet peste asta (fork bkerker/gr-gsm), vezi scriptul
`install_gr_gsm.sh` complet in [sdr-lab tools](https://github.com/<username>/sdr-lab-tools).

## Lista patch-urilor

Toate in `patches/` ca fisiere text aplicabile cu `patch -p1` sau
manual. `install.sh` le aplica automat.

**0001-configure-disable-linux-only-features.patch**
Adauga `--disable-uring --disable-libmnl --disable-libsctp` la invocarea
configure. io_uring, netlink si SCTP nu exista pe Darwin.

**0002-exec.c-setresgid-setresuid-to-Darwin.patch**
Substituie apeluri `setresgid(a,a,a)` cu `setregid(a,a)` si `setresuid(a,a,a)`
cu `setreuid(a,a)` in `src/core/exec.c`. Semantic echivalent pentru
use-case-ul concret (toate cele 3 argumente sunt aceeasi valoare).

**0003-wrap-linux-only-sources.patch**
Wrap in `#ifdef __linux__ ... #endif` pentru fisiere care includ header-e
Linux-only sau folosesc API-uri Linux:
- `src/vty/cpu_sched_vty.c` (`cpu_set_t`, `sched_setaffinity`)
- `src/core/netdev.c` (`linux/if.h`)
- `src/core/serial.c` (Linux ioctl-uri)
- `src/core/stats_tcp.c` (`linux/tcp.h`)
- `src/core/tun.c` (`linux/if_tun.h`)
- `src/gb/gprs_ns2_fr.c` (`linux/if.h`, Frame Relay socket family)

**0004-add-darwin-stubs.patch**
Adauga `src/core/darwin_stubs.c` la libosmocore_la_SOURCES in
`src/core/Makefile.am`. Fisierul `darwin_stubs.c` (copy in `src/` din acest
repo) exporta stub-uri no-op pentru simbolurile publice din fisierele
wrap-uite: `osmo_tcp_stats_config`, `osmo_stats_tcp_*`, `osmo_timerfd_*`,
`osmo_tundev_*`.

**0005-darwin-compat-header.patch**
Adauga `darwin_compat.h` la root si il include prin CFLAGS `-include`.
Defineste:
- `SO_PRIORITY=999` (Linux socket priority option, no-op pe Darwin)
- `CLOCK_REALTIME_COARSE=100`, `CLOCK_MONOTONIC_COARSE=101`,
  `CLOCK_BOOTTIME=102` (dummy IDs care nu se ciocnesc cu enum-ul Darwin
  `_clock_id`)
- macro `gettid()` -> `getpid()` (semantic degradat multi-thread, ok pentru
  logging)

**0006-LDFLAGS-dynamic-lookup-at-make.patch**
`LDFLAGS="-Wl,-undefined,dynamic_lookup"` aplicat DOAR la faza `make`,
NU la `configure`. Aplicat la configure ar cauza fals pozitive pentru
detectia `gettid`/`setns`/`unshare`.

## Testare instalare

Dupa `./install.sh`:

```bash
export PKG_CONFIG_PATH=$HOME/sdr-lab/local/lib/pkgconfig:$PKG_CONFIG_PATH
pkg-config --exists libosmocore && echo "libosmocore OK"
pkg-config --exists libosmogsm && echo "libosmogsm OK"

# Symbol check
nm -gU $HOME/sdr-lab/local/lib/libosmocore.dylib | grep osmo_tcp_stats_config
# ar trebui sa arate _osmo_tcp_stats_config exportat

# Runtime check (nu ar trebui sa afiseze eroare)
otool -L $HOME/sdr-lab/local/lib/libosmocore.dylib
```

## Limitari cunoscute

**Timing precision**: `CLOCK_MONOTONIC_COARSE` si `CLOCK_BOOTTIME` intorc
EINVAL la runtime (nu exista pe Darwin). Codul osmocom are fallback la
`CLOCK_MONOTONIC` standard prin `try/if` in `timer_clockgettime.c`, deci
efectul e degradare minora la resolutia timerelor, nu crash.

**CPU affinity**: `osmo-cpu-sched` VTY commands nu au efect (functiile sunt
stub). Impact: nu poti seta afinitate CPU per thread la runtime prin CLI.
Nu afecteaza rularea passive RX.

**Frame Relay GPRS**: `gprs_ns2_fr` wrap-uit. Impact: nu poti face
transport GPRS peste Frame Relay (rar folosit oricum, TCP/UDP transport
functioneaza normal).

**TUN device**: `osmo_tundev_*` sunt no-op. Impact: nu poti crea TUN
interface din libosmocore direct pe macOS. Pentru majoritatea use-cases
osmocom (BTS, MSC, HLR) nu e necesar.

**libosmocore statistics via TCP**: `osmo_stats_tcp_*` sunt no-op.
Impact: nu poti raporta stats via `stats_tcp`. Alternativa: stats prin
GSMTAP UDP sau prin logging.

## Ce ar face nativ mai bine

Idealul ar fi ca upstream libosmocore sa accepte patch-urile ca
`#ifdef __linux__` conditionale in loc sa wrap-uiesc integral fisiere.
Ma astept ca upstream sa fie reticent, dat fiind ca portabilitatea nu
e in scope-ul lor declarat.

Alternativ, un fork mentenat activ (nu doar acest snapshot) ar aduce
Osmocom on macOS la nivelul pe care il are Kismet, aircrack-ng si stack-ul
WiFi.

## License

GPLv2+ (mostenire din libosmocore original). Patch-urile individuale
sunt sub aceeasi licenta.

## Credits

Portul realizat de Andrei Gosman in cadrul proiectului personal SDR Lab,
in perioada septembrie 2026, cu ajutorul lui Claude (Anthropic) pentru
diagnostic si iterare pe patch-uri. Multe multumiri.

Upstream libosmocore: comunitatea Osmocom (https://osmocom.org).
Upstream gr-gsm fork: bkerler (https://github.com/bkerler/gr-gsm).

## Vezi si

- [sdr-lab-tools](https://github.com/<username>/sdr-lab-tools) - Scripturile
  complete Sirio scanner + install gr-gsm + analiza spectrala
- [gr-gsm bkerler fork](https://github.com/bkerler/gr-gsm) - Fork gr-gsm
  compatibil GNU Radio 3.10+
- [Osmocom project](https://osmocom.org)
