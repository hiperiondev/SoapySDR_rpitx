#!/usr/bin/env bash
#
# rpi-cross.sh - cross-compile for Raspberry Pi from a Linux x86_64 PC (Fedora family or Debian family).
#
#   * The host distribution is detected automatically (/etc/os-release): Fedora / RHEL family -> dnf,
#     Debian / Ubuntu / Linux Mint / Pop!_OS ... -> apt. Override with --distro.
#   * Menu / option to select the Raspberry Pi model (all Linux-capable models).
#   * Maps the model to the right ARM target: 32/64-bit userland, -mcpu, -mfpu,
#     -mfloat-abi, toolchain triplet, Debian multiarch directory, kernel image.
#   * Builds a sysroot from a running Pi (rsync over SSH) so the build uses the Pi's exact headers and
#     libraries (mandatory on Fedora, whose cross-gcc packages ship NO target C library).
#   * Builds CMake / Make / Eclipse-generated-makefile projects, or runs any
#     command with the cross environment (CC, CXX, CFLAGS, LDFLAGS, ...).
#   * "install" builds and installs the driver on a running Pi over SSH (module directory found
#     automatically, sudo handled like in "sysroot").
#
# Run "rpi-cross.sh help" for usage.  License: MIT-style, use at your own risk.
#
set -Eeuo pipefail

readonly SCRIPT_NAME=${0##*/}
readonly VERSION="1.2.0"

die()  { printf '%s: error: %s\n' "$SCRIPT_NAME" "$*" >&2; exit 1; }
warn() { printf '%s: warning: %s\n' "$SCRIPT_NAME" "$*" >&2; }
note() { printf '%s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------------------
# Model table (sources: raspberrypi.com hardware/processor docs, config.txt docs)
#   id | name | aliases | SoC | CPU core | ISA | class | OS bits | boot kernel image(s)
#   class selects the GCC flags (see resolve_target).
# ---------------------------------------------------------------------------
MODELS=(
"pi1|Raspberry Pi 1 Model A / A+ / B / B+|1,1a,1b,pi1a,pi1b,pi1a+,pi1b+|BCM2835|ARM1176JZF-S x1|ARMv6 + VFPv2|arm1176|32|kernel.img"
"pizero|Raspberry Pi Zero / Zero W / Zero WH|zero,zerow,zerowh,pizerow,pizerowh|BCM2835|ARM1176JZF-S x1|ARMv6 + VFPv2|arm1176|32|kernel.img"
"cm1|Raspberry Pi Compute Module 1|compute1,computemodule1|BCM2835|ARM1176JZF-S x1|ARMv6 + VFPv2|arm1176|32|kernel.img"
"pi2|Raspberry Pi 2 Model B (v1.0/v1.1)|2,2b,pi2b,pi2v11|BCM2836|Cortex-A7 x4|ARMv7-A + VFPv4/NEON|a7|32|kernel7.img"
"pi2v12|Raspberry Pi 2 Model B (v1.2)|pi2b12,2b1.2|BCM2837|Cortex-A53 x4|ARMv8-A|a53|32,64|kernel7.img | kernel8.img"
"pi3|Raspberry Pi 3 Model B|3,3b,pi3b|BCM2837|Cortex-A53 x4|ARMv8-A|a53|32,64|kernel7.img | kernel8.img"
"pi3plus|Raspberry Pi 3 Model B+ / 3 Model A+|3+,3b+,3a+,pi3b+,pi3a+,pi3plus|BCM2837B0|Cortex-A53 x4|ARMv8-A|a53|32,64|kernel7.img | kernel8.img"
"cm3|Raspberry Pi Compute Module 3 / 3 Lite / 3+|cm3plus,cm3lite,compute3|BCM2837 / BCM2837B0|Cortex-A53 x4|ARMv8-A|a53|32,64|kernel7.img | kernel8.img"
"pizero2w|Raspberry Pi Zero 2 W|zero2,zero2w,pizero2|RP3A0 (BCM2710A1)|Cortex-A53 x4|ARMv8-A|a53|32,64|kernel7.img | kernel8.img"
"pi4|Raspberry Pi 4 Model B|4,4b,pi4b|BCM2711|Cortex-A72 x4|ARMv8-A|a72|32,64|kernel7l.img | kernel8.img"
"pi400|Raspberry Pi 400|400|BCM2711|Cortex-A72 x4|ARMv8-A|a72|32,64|kernel7l.img | kernel8.img"
"cm4|Raspberry Pi Compute Module 4 / 4 Lite / 4S|cm4s,cm4lite,compute4|BCM2711|Cortex-A72 x4|ARMv8-A|a72|32,64|kernel7l.img | kernel8.img"
"pi5|Raspberry Pi 5|5,pi5b|BCM2712|Cortex-A76 x4|ARMv8.2-A (+crypto, dotprod)|a76|64|kernel_2712.img"
"pi500|Raspberry Pi 500 / 500+|500,500+,pi500+,pi500plus|BCM2712|Cortex-A76 x4|ARMv8.2-A (+crypto, dotprod)|a76|64|kernel_2712.img"
"cm5|Raspberry Pi Compute Module 5|compute5|BCM2712|Cortex-A76 x4|ARMv8.2-A (+crypto, dotprod)|a76|64|kernel_2712.img"
)

# ---------------------------------------------------------------------------
# Defaults (override with options or environment)
# ---------------------------------------------------------------------------
MODEL=${RPI_MODEL:-}
BITS=${RPI_BITS:-}
SYSROOT=${RPI_SYSROOT:-}          # "none" = use the toolchain's own sysroot
RPI_HOST=${RPI_HOST:-}            # user@host of a running Pi (for "sysroot")
PI_PACKAGES=${RPI_PI_PACKAGES:-}  # extra apt packages to install on the Pi before syncing
PI_PASSWORD=${RPI_PI_PASSWORD:-}  # password of the Pi user (used for sudo on the Pi, and for ssh login if sshpass exists)
unset RPI_PI_PASSWORD             # the build tools started by this script do not inherit the password
ASK_PASS=0                        # 1 = prompt for the Pi password
PREFIX=${RPI_PREFIX:-}            # toolchain prefix, e.g. aarch64-linux-gnu-
TOOLCHAIN_DIR=${RPI_TOOLCHAIN_DIR:-}
PROJECT_DIR=$PWD
JOBS=$(nproc 2>/dev/null || echo 2)
GCCVER=${RPI_GCC_VERSION:-}
USE_PI_LIBSTDCXX=1
EXTRA_CFLAGS=${RPI_CFLAGS:-}
EXTRA_LDFLAGS=${RPI_LDFLAGS:-}
FORCE=0
OUT_FILE=
DISTRO=${RPI_DISTRO:-}            # fedora | debian (default: detected from /etc/os-release)
OS_RELEASE_FILE=${RPI_OS_RELEASE:-/etc/os-release}
MODULE_DIR=${RPI_MODULE_DIR:-}    # SoapySDR module directory on the Pi (default: asked to SoapySDRUtil on the Pi)
PI_PREFIX=${RPI_PI_PREFIX:-/usr/local}   # prefix for rpitx_reset and the Python tools on the Pi
NO_BUILD=0                        # install: use the existing build directory
NO_TOOLS=0                        # install: do not install the Python tools

# Derived by resolve_target()
CLASS= ; NAME= ; SOC= ; CORE= ; ISA= ; BITS_ALLOWED= ; KERNEL=
TRIPLET= ; MA= ; DEB_ARCH= ; ARCH_FLAGS= ; CANDIDATES=()
SYSROOT_FLAG= ; C_INC= ; CXX_INC= ; LD_FLAGS=

# ---------------------------------------------------------------------------
usage() {
cat <<EOF
$SCRIPT_NAME $VERSION - cross-compile for Raspberry Pi from a Fedora or Debian-family x86_64 Linux PC

Usage: $SCRIPT_NAME <command> [options] [-- args...]

Commands:
  models                 List all supported models with SoC, CPU core, ARM ISA and GCC target flags
  info                   Show the host distribution and the resolved target (compiler, flags, sysroot)
  deps                   Install the host packages needed for the selected target (sudo dnf on Fedora,
                         sudo apt on Debian / Ubuntu / Mint ...: chosen automatically)
  sysroot                Create/refresh the sysroot by rsync-ing headers+libs from a running Pi (--host)
  env                    Print 'export ...' lines:   eval "\$($SCRIPT_NAME env -m pi4)"
  cmake-toolchain        Write a CMake toolchain file (-o FILE, default ./rpi-<model>-<bits>.cmake)
  build                  Build the project in -C DIR (auto: CMake / Eclipse build/makefile / Makefile)
  exec -- CMD...         Run any command (./configure, gcc, make ...) with the cross environment
  install                Build (unless --no-build) and install the driver on a running Pi over SSH (--host):
                         module into SoapySDR's module directory, rpitx_reset and the tools into <prefix>/bin
  verify FILE            Check an output binary: architecture, float ABI, glibc/libstdc++ versions vs sysroot
  help                   This text

Selecting the target:
  -m, --model ID         Model id or alias (pi1 pizero cm1 pi2 pi2v12 pi3 pi3plus cm3 pizero2w pi4 pi400
                         cm4 pi5 pi500 cm5). Without it a menu is shown on a terminal.
  -b, --bits 32|64       Userland of the OS on the Pi (what 'dpkg --print-architecture' says: armhf / arm64)

Host:
      --distro fedora|debian  Force the package manager family (default: detected; env RPI_DISTRO)

Toolchain / sysroot:
  -s, --sysroot DIR|none Sysroot (default ~/rpi-sysroot/arm64 or ~/rpi-sysroot/armhf); 'none' = toolchain's own
  -H, --host USER@HOST   Raspberry Pi to copy the sysroot from ('sysroot') or to install on ('install')
      --pi-packages "A B" apt packages to install on the Pi first (e.g. "libsoapysdr-dev"); for 'sysroot' g++ is added
      --pi-password PASS  Password of the Pi user: lets 'sysroot' / 'install' use sudo on the Pi (apt, rsync, install; root-only
                         files such as /usr/lib/cups/backend/* can be read). Also: env RPI_PI_PASSWORD.
                         Visible in 'ps' and shell history - prefer --ask-pass.
      --ask-pass         Prompt for the Pi user's password (not echoed) instead of passing it on the command line
  -P, --prefix PREFIX    Toolchain prefix (default: auto-detect, e.g. aarch64-linux-gnu-)
  -T, --toolchain-dir D  Directory whose bin/ holds the cross toolchain (e.g. an Arm GNU / Bootlin download)
      --gcc-version N    libstdc++ version dir in the sysroot (default: highest found)
      --host-libstdcxx   Use the compiler's own libstdc++ headers instead of the Pi's

Install on the Pi:
      --module-dir DIR   SoapySDR module directory on the Pi (default: first "Search path" of SoapySDRUtil --info
                         on the Pi, e.g. /usr/lib/aarch64-linux-gnu/SoapySDR/modules0.8)
      --pi-prefix DIR    Where rpitx_reset and the Python tools go on the Pi (default: /usr/local -> DIR/bin)
      --no-build         Install the existing build directory (-o) without rebuilding
      --no-tools         Do not install the Python tools (rpitx_tone_test.py, hpsdr_rpitx_bridge.py)

Build:
  -C, --dir DIR          Project directory (default: current directory)
  -o, --output FILE|DIR  Output file (cmake-toolchain) / build directory (CMake)
  -j, --jobs N           Parallel jobs (default: $JOBS)
      --cflags "..."     Extra flags for C/C++ (e.g. "-O2 -g")
      --ldflags "..."    Extra linker flags
      --force            Ignore sanity-check failures (e.g. sysroot architecture mismatch)
  -h, --help

Typical session:
  $SCRIPT_NAME deps    -m pi4 -b 64
  $SCRIPT_NAME sysroot -m pi4 -b 64 -H pi@raspberrypi.local --pi-packages "libsoapysdr-dev"
  $SCRIPT_NAME build   -m pi4 -b 64 -C ~/src/SoapySDR_rpitx
  $SCRIPT_NAME verify  -m pi4 -b 64 ~/src/SoapySDR_rpitx/build-rpi-pi4-64/librpitxSupport.so
  $SCRIPT_NAME install -m pi4 -b 64 -C ~/src/SoapySDR_rpitx -H pi@raspberrypi.local --ask-pass
EOF
}

# ---------------------------------------------------------------------------
# Host distribution (Fedora family -> dnf, Debian family -> apt)
# ---------------------------------------------------------------------------
HOST_FAMILY= ; HOST_NAME=

detect_host() {
  [[ -z $HOST_FAMILY ]] || return 0
  local id="" like="" tok
  HOST_NAME=$(uname -s)
  if [[ -r $OS_RELEASE_FILE ]]; then
    id=$(. "$OS_RELEASE_FILE" >/dev/null 2>&1; printf '%s' "${ID:-}") || id=""
    like=$(. "$OS_RELEASE_FILE" >/dev/null 2>&1; printf '%s' "${ID_LIKE:-}") || like=""
    HOST_NAME=$(. "$OS_RELEASE_FILE" >/dev/null 2>&1; printf '%s' "${PRETTY_NAME:-${NAME:-}}") || true
  fi

  if [[ -n $DISTRO ]]; then
    case ${DISTRO,,} in
      fedora|rhel|redhat|centos|rocky|alma|almalinux|nobara|dnf) HOST_FAMILY=fedora ;;
      debian|ubuntu|mint|linuxmint|pop|raspbian|apt)             HOST_FAMILY=debian ;;
      *) die "--distro must be 'fedora' or 'debian' (got '$DISTRO')" ;;
    esac
  else
    # ID first (e.g. linuxmint), then ID_LIKE (e.g. "ubuntu debian", "rhel fedora")
    for tok in $id $like; do
      case $tok in
        fedora|rhel|centos|rocky|almalinux|ol|nobara|amzn)
          HOST_FAMILY=fedora; break ;;
        debian|ubuntu|linuxmint|raspbian|pop|neon|kali|elementary|zorin|devuan|mx|tuxedo)
          HOST_FAMILY=debian; break ;;
      esac
    done
    if [[ -z $HOST_FAMILY ]]; then            # unknown distribution: look at the package manager
      if   have dnf;     then HOST_FAMILY=fedora
      elif have apt-get; then HOST_FAMILY=debian
      else die "cannot tell the host distribution ('${id:-unknown}'): neither dnf nor apt-get found. Install the cross compiler by hand and use --prefix / --toolchain-dir."
      fi
    fi
  fi
}

host_root() {      # run a command as root on this PC (directly if already root, else through sudo)
  if ((EUID == 0)); then "$@"
  elif have sudo; then sudo "$@"
  else die "root privileges are needed and sudo is not installed (run this command as root)"
  fi
}

# ---------------------------------------------------------------------------
# Model helpers
# ---------------------------------------------------------------------------
norm() { local q=${1,,}; q=${q//[[:space:]_-]/}; q=${q#raspberry}; q=${q#rpi}; printf '%s' "$q"; }

lookup_model() {   # prints the table row for an id or alias
  local q row id aliases a arr
  q=$(norm "$1")
  for row in "${MODELS[@]}"; do
    IFS='|' read -r id _ aliases _ <<<"$row"
    [[ $q == "$id" || $q == "$(norm "$id")" ]] && { printf '%s\n' "$row"; return 0; }
    IFS=',' read -ra arr <<<"$aliases"
    for a in "${arr[@]}"; do [[ $q == "$a" ]] && { printf '%s\n' "$row"; return 0; }; done
  done
  return 1
}

choose_model() {
  [[ -t 0 && -t 2 ]] || die "no --model given and no terminal for the menu. Run '$SCRIPT_NAME models' for the list."
  local row id name soc core isa labels=() ids=()
  for row in "${MODELS[@]}"; do
    IFS='|' read -r id name _ soc core isa _ <<<"$row"
    labels+=("$(printf '%-9s %-46s %-18s %-16s %s' "$id" "$name" "$soc" "$core" "$isa")")
    ids+=("$id")
  done
  PS3="Select the Raspberry Pi model (number): "
  select _ in "${labels[@]}"; do
    if [[ ${REPLY:-} =~ ^[0-9]+$ ]] && ((REPLY >= 1 && REPLY <= ${#ids[@]})); then
      MODEL=${ids[REPLY-1]}; return 0
    fi
    note "Invalid choice."
  done
  die "no model selected"
}

choose_bits() {
  case $BITS_ALLOWED in
    32) BITS=32; return ;;
    64) BITS=64; return ;;
  esac
  if [[ -t 0 && -t 2 ]]; then
    PS3="OS userland on the Pi (dpkg --print-architecture): "
    select _ in "64-bit (arm64 / aarch64)" "32-bit (armhf)"; do
      case ${REPLY:-} in 1) BITS=64; return ;; 2) BITS=32; return ;; *) note "Invalid choice." ;; esac
    done
    die "no userland selected"
  fi
  BITS=64
  warn "no --bits given and no terminal: assuming 64-bit userland (arm64)"
}

# Fill the target variables from MODEL/BITS
resolve_target() {
  local row
  [[ -n $MODEL ]] || choose_model
  row=$(lookup_model "$MODEL") || die "unknown model '$MODEL' (try: $SCRIPT_NAME models)"
  IFS='|' read -r MODEL NAME _ SOC CORE ISA CLASS BITS_ALLOWED KERNEL <<<"$row"

  if [[ -z $BITS ]]; then choose_bits; fi
  BITS=${BITS%-bit}; BITS=${BITS%bit}
  [[ $BITS == 32 || $BITS == 64 ]] || die "--bits must be 32 or 64"
  [[ ",$BITS_ALLOWED," == *",$BITS,"* ]] || die "$NAME supports only $BITS_ALLOWED-bit userland"

  case $BITS in
    64)
      TRIPLET=aarch64-linux-gnu; MA=aarch64-linux-gnu; DEB_ARCH=arm64
      CANDIDATES=(aarch64-linux-gnu- aarch64-none-linux-gnu- aarch64-unknown-linux-gnu-)
      case $CLASS in
        a53) ARCH_FLAGS="-mcpu=cortex-a53" ;;
        a72) ARCH_FLAGS="-mcpu=cortex-a72" ;;
        a76) ARCH_FLAGS="-mcpu=cortex-a76+crypto" ;;
        *)   die "internal: class $CLASS has no 64-bit mapping" ;;
      esac ;;
    32)
      TRIPLET=arm-linux-gnueabihf; MA=arm-linux-gnueabihf; DEB_ARCH=armhf
      CANDIDATES=(arm-linux-gnueabihf- arm-none-linux-gnueabihf- arm-unknown-linux-gnueabihf- arm-linux-gnu-)
      case $CLASS in
        arm1176) ARCH_FLAGS="-marm -mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard" ;;
        a7)      ARCH_FLAGS="-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard" ;;
        a53)     ARCH_FLAGS="-mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard" ;;
        a72)     ARCH_FLAGS="-mcpu=cortex-a72 -mfpu=neon-fp-armv8 -mfloat-abi=hard" ;;
        *)       die "internal: class $CLASS has no 32-bit mapping" ;;
      esac ;;
  esac

  [[ -n $TOOLCHAIN_DIR ]] && PATH="$TOOLCHAIN_DIR/bin:$PATH"
  if [[ -z $PREFIX ]]; then
    local p
    for p in "${CANDIDATES[@]}"; do
      if have "${p}g++"; then PREFIX=$p; break; fi
    done
  fi
  [[ -n $SYSROOT ]] || SYSROOT="$HOME/rpi-sysroot/$DEB_ARCH"
}

# Compute include/link flags from the sysroot layout (Debian multiarch)
build_flags() {
  SYSROOT_FLAG=""; C_INC=""; CXX_INC=""; LD_FLAGS=""
  [[ $SYSROOT == none ]] && return 0
  [[ -d $SYSROOT/usr/include ]] || warn "sysroot '$SYSROOT' has no usr/include - run '$SCRIPT_NAME sysroot' first"

  SYSROOT_FLAG="--sysroot=$SYSROOT"
  [[ -d $SYSROOT/usr/include/$MA ]] && C_INC="-isystem $SYSROOT/usr/include/$MA"

  if [[ -z $GCCVER && -d $SYSROOT/usr/include/c++ ]]; then
    GCCVER=$(find "$SYSROOT/usr/include/c++" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort -V | tail -1)
  fi
  if ((USE_PI_LIBSTDCXX)) && [[ -n $GCCVER && -d $SYSROOT/usr/include/c++/$GCCVER ]]; then
    CXX_INC="-nostdinc++ -isystem $SYSROOT/usr/include/c++/$GCCVER"
    [[ -d $SYSROOT/usr/include/$MA/c++/$GCCVER ]] && CXX_INC+=" -isystem $SYSROOT/usr/include/$MA/c++/$GCCVER"
    [[ -d $SYSROOT/usr/include/c++/$GCCVER/backward ]] && CXX_INC+=" -isystem $SYSROOT/usr/include/c++/$GCCVER/backward"
  fi

  LD_FLAGS="$SYSROOT_FLAG"
  local d
  [[ -d $SYSROOT/usr/lib/$MA ]] && LD_FLAGS+=" -B$SYSROOT/usr/lib/$MA"
  for d in "$SYSROOT/usr/lib/gcc/$MA/$GCCVER" "$SYSROOT/usr/lib/$MA" "$SYSROOT/lib/$MA"; do
    [[ -d $d ]] && LD_FLAGS+=" -L$d"
  done
  for d in "$SYSROOT/usr/lib/$MA" "$SYSROOT/lib/$MA"; do
    [[ -d $d ]] && LD_FLAGS+=" -Wl,-rpath-link,$d"
  done
  return 0
}

# Cross compilers built only for the kernel (Fedora's gcc-<arch>-linux-gnu packages) have no target libgcc_s,
# so their driver never links it and C++ unwinding (_Unwind_Resume) stays undefined. The Pi's own
# libgcc_s.so.1 from the sysroot is linked explicitly in that case.
detect_libgcc_s() {
  LIBGCC_S=""
  [[ $SYSROOT == none || -z $PREFIX ]] && return 0
  have "${PREFIX}g++" || return 0
  local own d
  # shellcheck disable=SC2086
  own=$("${PREFIX}g++" $ARCH_FLAGS -print-file-name=libgcc_s.so 2>/dev/null) || own=""
  [[ $own == /* && -e $own ]] && return 0   # the compiler provides its own libgcc_s
  for d in "$SYSROOT/usr/lib/$MA" "$SYSROOT/lib/$MA"; do
    if [[ -e $d/libgcc_s.so.1 ]]; then
      LIBGCC_S="$d/libgcc_s.so.1"
      return 0
    fi
  done
  warn "${PREFIX}g++ has no libgcc_s and the sysroot has no libgcc_s.so.1: C++ programs will not link (re-run '$SCRIPT_NAME sysroot')"
}

# Warn when an ARMv6 target gets a toolchain whose libgcc is built for ARMv7+
check_toolchain_isa() {
  [[ $CLASS == arm1176 && -n $PREFIX ]] || return 0
  have "${PREFIX}readelf" || return 0
  local lg ca
  # shellcheck disable=SC2086
  lg=$("${PREFIX}g++" $ARCH_FLAGS -print-libgcc-file-name 2>/dev/null) || return 0
  [[ -f $lg ]] || return 0
  ca=$("${PREFIX}readelf" -A "$lg" 2>/dev/null | awk '/Tag_CPU_arch:/ {print $2; exit}')
  case $ca in
    ""|v4*|v5*|v6*) ;;
    *) warn "this toolchain's libgcc is built for ARM $ca, but $NAME is ARMv6 (ARM1176). Code may crash on
         the Pi (e.g. Thumb-2 or ARMv7 instructions in libgcc helpers). Use an ARMv6-capable toolchain
         (Bootlin 'armv6-eabihf', crosstool-NG, or build in an armhf container) via --toolchain-dir/--prefix." ;;
  esac
}

# Full environment for a build
prepare() {
  resolve_target
  [[ -n $PREFIX ]] || die "no cross compiler found for $BITS-bit. Run '$SCRIPT_NAME deps -m $MODEL -b $BITS', or use --prefix / --toolchain-dir."
  have "${PREFIX}g++" || die "${PREFIX}g++ not found in PATH"
  build_flags
  detect_libgcc_s
  check_toolchain_isa
  [[ $GCCVER =~ ^[0-9]*$ ]] || true
}

compose_env() {   # sets C_FLAGS_ALL CXX_FLAGS_ALL LD_FLAGS_ALL LD_LINK_ALL
  C_FLAGS_ALL="$ARCH_FLAGS $SYSROOT_FLAG $C_INC $EXTRA_CFLAGS"
  CXX_FLAGS_ALL="$ARCH_FLAGS $SYSROOT_FLAG $CXX_INC $C_INC $EXTRA_CFLAGS"
  LD_FLAGS_ALL="$LD_FLAGS $EXTRA_LDFLAGS"
  # Make and Eclipse put LDFLAGS before the objects: --no-as-needed keeps libgcc_s although nothing refers to
  # it yet at that point. CMake appends it at the end of the link line instead (CMAKE_<LANG>_STANDARD_LIBRARIES).
  LD_LINK_ALL=$LD_FLAGS_ALL
  [[ -n ${LIBGCC_S:-} ]] && LD_LINK_ALL+=" -Wl,--push-state,--no-as-needed $LIBGCC_S -Wl,--pop-state"
  # squeeze blanks
  C_FLAGS_ALL=$(echo $C_FLAGS_ALL); CXX_FLAGS_ALL=$(echo $CXX_FLAGS_ALL); LD_FLAGS_ALL=$(echo $LD_FLAGS_ALL)
  LD_LINK_ALL=$(echo $LD_LINK_ALL)
}

export_env() {
  compose_env
  export CROSS_COMPILE=$PREFIX
  export CC="${PREFIX}gcc" CXX="${PREFIX}g++" CPP="${PREFIX}cpp"
  export AR="${PREFIX}ar" AS="${PREFIX}as" LD="${PREFIX}ld" NM="${PREFIX}nm" OBJCOPY="${PREFIX}objcopy"
  export OBJDUMP="${PREFIX}objdump" RANLIB="${PREFIX}ranlib" READELF="${PREFIX}readelf" STRIP="${PREFIX}strip"
  export CFLAGS="$C_FLAGS_ALL" CXXFLAGS="$CXX_FLAGS_ALL" LDFLAGS="$LD_LINK_ALL"
  export RPI_MODEL=$MODEL RPI_BITS=$BITS RPI_TRIPLET=$TRIPLET RPI_SYSROOT=$SYSROOT
  if [[ $SYSROOT != none ]]; then
    export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
    export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/$MA/pkgconfig:$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig:$SYSROOT/usr/local/lib/pkgconfig"
    unset PKG_CONFIG_PATH
  fi
}

# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
cmd_models() {
  local row id name soc core isa bits kern
  printf '\n%-9s %-46s %-18s %-16s %-26s %-6s %s\n' ID MODEL SoC "CPU CORE" "ARM ISA" "OS" "BOOT KERNEL"
  printf '%s\n' "$(printf -- '-%.0s' {1..150})"
  for row in "${MODELS[@]}"; do
    IFS='|' read -r id name _ soc core isa _ bits kern <<<"$row"
    printf '%-9s %-46s %-18s %-16s %-26s %-6s %s\n' "$id" "$name" "$soc" "$core" "$isa" "$bits" "$kern"
  done
  cat <<'EOF'

GCC target flags used per CPU class (OS bits: 32 = armhf / 64 = arm64):

  class    64-bit (aarch64-linux-gnu)         32-bit (arm-linux-gnueabihf)
  -------  ---------------------------------  ------------------------------------------------------------
  arm1176  (n/a - 32-bit only)                -marm -mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard
  a7       (n/a - 32-bit only)                -mcpu=cortex-a7  -mfpu=neon-vfpv4   -mfloat-abi=hard
  a53      -mcpu=cortex-a53                   -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard
  a72      -mcpu=cortex-a72                   -mcpu=cortex-a72 -mfpu=neon-fp-armv8 -mfloat-abi=hard
  a76      -mcpu=cortex-a76+crypto            (not offered: Pi 5 family is used with 64-bit userland)

Notes
  * No +crypto for Cortex-A53/A72: the BCM2837/BCM2711 do not implement the ARMv8 crypto extensions
    (SIGILL if used). The Pi 5 (BCM2712) reports aes/sha1/sha2/pmull in /proc/cpuinfo, so +crypto is safe there.
  * Pi 2 v1.1 (BCM2836, Cortex-A7) is 32-bit only; v1.2 (BCM2837) can run 64-bit.
  * A binary built for ARMv6 runs on every 32-bit Pi; one built for -mcpu=cortex-a7+ will NOT run on Pi 1/Zero.
  * Not handled: Raspberry Pi Pico family (RP2040 Cortex-M0+, RP2350) - microcontrollers; use the
    Pico SDK / arm-none-eabi-gcc instead of this script.
EOF
}

cmd_info() {
  detect_host
  prepare
  compose_env
  cat <<EOF

Host           : $HOST_NAME  [$HOST_FAMILY family, $([[ $HOST_FAMILY == fedora ]] && echo dnf || echo apt)]
Model          : $NAME  [$MODEL]
SoC / CPU      : $SOC / $CORE
ARM ISA        : $ISA
OS userland    : $BITS-bit ($DEB_ARCH)   -> boot kernel image(s): $KERNEL
Target triplet : $TRIPLET   (Debian multiarch dir: $MA)
Arch flags     : $ARCH_FLAGS
Compiler       : ${PREFIX}g++  ($(command -v "${PREFIX}g++") ; $("${PREFIX}g++" -dumpfullversion 2>/dev/null || echo '?'))
Sysroot        : $SYSROOT  $( [[ $SYSROOT == none || -d $SYSROOT/usr/include ]] && echo '(found)' || echo '(MISSING - run: sysroot)' )
Pi libstdc++   : ${GCCVER:-n/a}  $( ((USE_PI_LIBSTDCXX)) && echo '(Pi C++ headers via -nostdinc++)' || echo '(compiler C++ headers)' )

CFLAGS         : $C_FLAGS_ALL
CXXFLAGS       : $CXX_FLAGS_ALL
LDFLAGS        : $LD_LINK_ALL
libgcc_s       : ${LIBGCC_S:-provided by the compiler}
EOF
}

cmd_deps() {
  resolve_target
  detect_host
  local pkgs=() arch
  arch=$(uname -m)
  [[ $arch == x86_64 ]] || warn "this PC is $arch: the cross packages are meant for x86_64 hosts (on an ARM PC build natively)"
  case $HOST_FAMILY in
    fedora)
      have dnf || die "dnf not found - this does not look like a Fedora-family system (use --distro debian to force apt)"
      pkgs=(make cmake git rsync openssh-clients file binutils)
      if [[ $BITS == 64 ]]; then
        pkgs+=(gcc-c++-aarch64-linux-gnu binutils-aarch64-linux-gnu)
      else
        pkgs+=(gcc-c++-arm-linux-gnu binutils-arm-linux-gnu)
      fi
      note "Host: $HOST_NAME  ->  dnf"
      note "Installing: ${pkgs[*]}"
      host_root dnf install -y "${pkgs[@]}"
      ;;
    debian)
      have apt-get || die "apt-get not found - this does not look like a Debian-family system (use --distro fedora to force dnf)"
      pkgs=(make cmake git rsync openssh-client file binutils)
      if [[ $BITS == 64 ]]; then
        pkgs+=(g++-aarch64-linux-gnu)
      else
        pkgs+=(g++-arm-linux-gnueabihf)
      fi
      note "Host: $HOST_NAME  ->  apt"
      note "Installing: ${pkgs[*]}"
      host_root apt-get update || warn "apt-get update failed, trying to install anyway"
      host_root env DEBIAN_FRONTEND=noninteractive apt-get install -y "${pkgs[@]}"
      ;;
  esac
  if [[ $BITS == 32 ]]; then
    cat >&2 <<'EOF'

NOTE (32-bit): the distributions' arm cross-gcc (Fedora arm-linux-gnu, Debian/Ubuntu arm-linux-gnueabihf)
defaults to ARMv7-A hard-float (Thumb-2).
  * Pi 2/3/4 targets work with the flags this script passes.
  * Pi 1 / Zero / CM1 (ARMv6) need an ARMv6-capable toolchain: download e.g. a Bootlin "armv6-eabihf"
    glibc toolchain, unpack it and use  --toolchain-dir <dir> --prefix arm-linux-
    (or build natively / in an armhf container).
EOF
  fi
  if [[ $HOST_FAMILY == fedora ]]; then
    note "Fedora's cross-gcc has no target libc: create a sysroot with '$SCRIPT_NAME sysroot -H user@pi'."
  else
    note "Next: '$SCRIPT_NAME sysroot -H user@pi' copies the Pi's exact headers and libraries (libsoapysdr-dev ...) into a sysroot."
  fi
}

fix_symlinks() {   # make absolute symlinks inside the sysroot relative to the sysroot
  local link target new n=0
  while IFS= read -r -d '' link; do
    target=$(readlink "$link")
    if [[ $target == /* ]]; then
      new=$(realpath -m --relative-to="$(dirname "$link")" "$SYSROOT$target")
      ln -sfn "$new" "$link"; n=$((n+1))
    fi
  done < <(find "$SYSROOT" -type l -print0)
  note "  converted $n absolute symlinks to relative"
}

# --- SSH / sudo helpers for the Pi ----------------------------------------
SSH=() ; SSH_RSH= ; SSH_CTL_DIR= ; REMOTE_ASKPASS= ; REMOTE_TMP= ; RSYNC_SUDO=0 ; PI_SUDO=none

shq() { local s=${1//\'/\'\\\'\'}; printf "'%s'" "$s"; }   # POSIX single-quoting for remote command lines

setup_ssh() {      # one multiplexed connection: the login is asked at most once
  SSH_CTL_DIR=$(mktemp -d)
  SSH_RSH="ssh -o ControlMaster=auto -o ControlPersist=300 -o ControlPath=$SSH_CTL_DIR/cm-%C"
  read -r -a SSH <<<"$SSH_RSH"
  if [[ -n $PI_PASSWORD ]] && have sshpass; then
    SSH=(ssh_with_password "${SSH[@]}")
  fi
}

ssh_with_password() {    # sshpass -e reads SSHPASS, which is set for this one command only (not exported, not in argv)
  SSHPASS=$PI_PASSWORD sshpass -e "$@"
}

make_remote_askpass() {   # temporary SUDO_ASKPASS helper on the Pi (mode 700, in RAM, removed on exit)
  local pw=${PI_PASSWORD//\'/\'\\\'\'}     # single-quote escaping; sent via stdin, never on a command line
  # The helper holds the password in clear, so it is created on a tmpfs (the user's runtime dir or /dev/shm),
  # never on the SD card.
  REMOTE_ASKPASS=$("${SSH[@]}" "$RPI_HOST" 'umask 077; f=$(mktemp -p "${XDG_RUNTIME_DIR:-/dev/shm}") && chmod 700 "$f" && cat >"$f" && printf %s "$f"' \
    <<<"#!/bin/sh
printf '%s\n' '$pw'") || die "could not prepare the sudo helper on $RPI_HOST"
  [[ -n $REMOTE_ASKPASS ]] || die "could not prepare the sudo helper on $RPI_HOST"
}

cleanup_ssh() {
  if [[ -n $REMOTE_ASKPASS ]]; then
    "${SSH[@]}" "$RPI_HOST" "rm -f '$REMOTE_ASKPASS'" >/dev/null 2>&1 || true
  fi
  if [[ -n $REMOTE_TMP ]]; then
    "${SSH[@]}" "$RPI_HOST" "rm -rf '$REMOTE_TMP'" >/dev/null 2>&1 || true
  fi
  if [[ -n $SSH_CTL_DIR ]]; then
    ssh -o "ControlPath=$SSH_CTL_DIR/cm-%C" -O exit "$RPI_HOST" >/dev/null 2>&1 || true
    rm -rf "$SSH_CTL_DIR"
  fi
  unset SSHPASS
}

pi_ask_password() {       # --ask-pass: read the password now (before a long build, not after it)
  if ((ASK_PASS)) && [[ -z $PI_PASSWORD ]]; then
    [[ -r /dev/tty ]] || die "--ask-pass needs a terminal"
    read -rsp "Password of $RPI_HOST: " PI_PASSWORD </dev/tty; echo >&2
  fi
}

pi_connect() {
  [[ -n $RPI_HOST ]] || die "--host user@pi is required for '$COMMAND'"
  have ssh   || die "ssh not installed (run: $SCRIPT_NAME deps)"
  have rsync || die "rsync not installed (run: $SCRIPT_NAME deps)"
  pi_ask_password
  [[ -z $PI_PASSWORD ]] || have sshpass || \
    note "(sshpass not installed: the SSH login itself will be asked interactively; the password is used for sudo only)"
  setup_ssh
  trap cleanup_ssh EXIT
}

pi_check_arch() {         # the Pi's userland must match the selected -b
  local remote_arch
  remote_arch=$("${SSH[@]}" "$RPI_HOST" 'dpkg --print-architecture' 2>/dev/null) || die "cannot run 'dpkg --print-architecture' on $RPI_HOST (is it Raspberry Pi OS / Debian? wrong login/password?)"
  if [[ $remote_arch != "$DEB_ARCH" ]]; then
    ((FORCE)) || die "the Pi's userland is '$remote_arch' but the selection needs '$DEB_ARCH' (-b $BITS). Use -b to match, or --force."
    warn "architecture mismatch ($remote_arch vs $DEB_ARCH) ignored (--force)"
  fi
}

pi_sudo_setup() {         # how to become root on the Pi: root | askpass | nopasswd | none
  if "${SSH[@]}" "$RPI_HOST" 'test "$(id -u)" -eq 0' >/dev/null 2>&1; then
    PI_SUDO=root
  elif [[ -n $PI_PASSWORD ]]; then
    make_remote_askpass
    PI_SUDO=askpass
  elif "${SSH[@]}" "$RPI_HOST" 'sudo -n true' >/dev/null 2>&1; then
    PI_SUDO=nopasswd
  else
    PI_SUDO=none
  fi
}

pi_root() {               # pi_root 'shell command line': run it as root on the Pi
  local cmd=$1
  case $PI_SUDO in
    root)     "${SSH[@]}" "$RPI_HOST" "$cmd" ;;
    askpass)  "${SSH[@]}" "$RPI_HOST" "SUDO_ASKPASS=$(shq "$REMOTE_ASKPASS") sudo -A sh -c $(shq "$cmd")" ;;
    nopasswd) "${SSH[@]}" "$RPI_HOST" "sudo sh -c $(shq "$cmd")" ;;
    *)        [[ -t 0 ]] || die "root access on the Pi needs a password: use --pi-password / --ask-pass (or passwordless sudo)"
              "${SSH[@]}" -t "$RPI_HOST" "sudo sh -c $(shq "$cmd")" ;;
  esac
}

pi_apt_install() {        # pi_apt_install PKG... : apt-get update + install on the Pi
  pi_root "export DEBIAN_FRONTEND=noninteractive; apt-get update && apt-get install -y $*"
}

cmd_sysroot() {
  resolve_target
  [[ $SYSROOT != none ]] || die "--sysroot none cannot be synced"
  pi_connect
  pi_check_arch
  pi_sudo_setup

  # Root is needed to read root-only files such as /usr/lib/cups/backend/*
  # Kernel modules and firmware blobs are not needed to build and account for most of /usr/lib.
  local rs=(rsync -a --delete --exclude=/modules/ --exclude=/firmware/ -e "$SSH_RSH")
  case $PI_SUDO in
    root)     RSYNC_SUDO=1 ;;
    askpass)  rs+=(--rsync-path="env SUDO_ASKPASS=$REMOTE_ASKPASS sudo -A rsync"); RSYNC_SUDO=1 ;;
    nopasswd) rs+=(--rsync-path="sudo rsync"); RSYNC_SUDO=1 ;;
    *)        warn "no sudo on the Pi without a password: root-only files will be skipped (use --pi-password or --ask-pass)" ;;
  esac

  if [[ -n $PI_PACKAGES ]]; then
    note "Installing packages on the Pi: g++ $PI_PACKAGES"
    pi_apt_install g++ libc6-dev "$PI_PACKAGES"
  fi

  pi_rsync() {   # pi_rsync SRC DST - without sudo, "some files not readable" (23/24) is only a warning
    local rc=0
    "${rs[@]}" "$1" "$2" || rc=$?
    if ((rc == 23 || rc == 24)) && ((! RSYNC_SUDO)); then
      warn "some root-only files could not be read and were skipped (harmless for building)"; return 0
    fi
    return "$rc"
  }

  mkdir -p "$SYSROOT/usr"
  note "Syncing /usr/include ..."; pi_rsync "$RPI_HOST:/usr/include/" "$SYSROOT/usr/include/"
  note "Syncing /usr/lib ...";     pi_rsync "$RPI_HOST:/usr/lib/"     "$SYSROOT/usr/lib/"
  # Arch-independent CMake package configs and pkg-config files (e.g. SoapySDRConfig.cmake on Debian)
  local d
  for d in cmake pkgconfig; do
    if "${SSH[@]}" "$RPI_HOST" "test -d /usr/share/$d"; then
      mkdir -p "$SYSROOT/usr/share/$d"
      note "Syncing /usr/share/$d ..."; pi_rsync "$RPI_HOST:/usr/share/$d/" "$SYSROOT/usr/share/$d/"
    fi
  done
  if "${SSH[@]}" "$RPI_HOST" 'test -d /usr/local/include || test -d /usr/local/lib'; then
    mkdir -p "$SYSROOT/usr/local"
    note "Syncing /usr/local ...";  pi_rsync "$RPI_HOST:/usr/local/" "$SYSROOT/usr/local/"
  fi
  if "${SSH[@]}" "$RPI_HOST" 'test -L /lib'; then
    # usr-merged distro: /lib -> usr/lib (a real directory left by an older sync is replaced by the link)
    if [[ -d $SYSROOT/lib && ! -L $SYSROOT/lib ]]; then
      rm -rf "${SYSROOT:?}/lib"
    fi
    ln -sfn usr/lib "$SYSROOT/lib"
  else
    mkdir -p "$SYSROOT/lib"; note "Syncing /lib ..."; pi_rsync "$RPI_HOST:/lib/" "$SYSROOT/lib/"
  fi
  fix_symlinks
  build_flags
  note "Sysroot ready: $SYSROOT  (libstdc++ headers: ${GCCVER:-none found - install g++ on the Pi})"
}

sysroot_soapy_abi() {     # SoapySDR module ABI the sysroot's headers were taken from (e.g. 0.8)
  local f
  [[ $SYSROOT != none ]] || return 0
  for f in "$SYSROOT/usr/include/SoapySDR/Version.h" "$SYSROOT/usr/local/include/SoapySDR/Version.h"; do
    if [[ -f $f ]]; then
      sed -n 's/^#define SOAPY_SDR_ABI_VERSION "\(.*\)".*/\1/p' "$f" | head -n 1
      return 0
    fi
  done
  return 0
}

cmd_install() {
  [[ -n $RPI_HOST ]] || die "--host user@pi is required for 'install'"
  [[ -f $PROJECT_DIR/CMakeLists.txt ]] || die "'install' needs the SoapySDR_rpitx CMake project in $PROJECT_DIR (use -C DIR)"
  pi_ask_password                       # asked before the build, which can take a while
  if ((NO_BUILD)); then prepare; else cmd_build; fi

  local bdir=${OUT_FILE:-build-rpi-$MODEL-$BITS} bpath so reset t tools=()
  if [[ $bdir == /* ]]; then bpath=$bdir; else bpath=$PROJECT_DIR/$bdir; fi
  so=$bpath/librpitxSupport.so
  reset=$bpath/rpitx_reset
  [[ -f $so ]] || die "$so not found (build first, or point -o to the build directory)"
  if ((! NO_TOOLS)); then
    for t in "$PROJECT_DIR"/tools/*.py; do [[ -f $t ]] && tools+=("$t"); done
  fi

  note ">> checking $so against the sysroot"
  cmd_verify "$so" || { ((FORCE)) || die "verification failed (see above): refresh the sysroot and rebuild, or use --force"; }

  pi_connect
  pi_check_arch
  note "Raspberry Pi: $("${SSH[@]}" "$RPI_HOST" 'cat /proc/device-tree/model 2>/dev/null | tr -d "\0"' || echo '?')"
  pi_sudo_setup

  # Runtime on the Pi: SoapySDRUtil (soapysdr-tools pulls in libsoapysdr) and any extra packages
  if ! "${SSH[@]}" "$RPI_HOST" 'command -v SoapySDRUtil >/dev/null 2>&1'; then
    note "SoapySDR is not installed on the Pi: installing soapysdr-tools ..."
    pi_apt_install soapysdr-tools
  fi
  if [[ -n $PI_PACKAGES ]]; then
    note "Installing packages on the Pi: $PI_PACKAGES"
    pi_apt_install "$PI_PACKAGES"
  fi

  # Where SoapySDR on the Pi looks for modules
  local moddir=$MODULE_DIR abi_sys base
  abi_sys=$(sysroot_soapy_abi)
  if [[ -z $moddir ]]; then
    moddir=$("${SSH[@]}" "$RPI_HOST" "SoapySDRUtil --info 2>/dev/null | sed -n 's/^Search path: *//p' | head -n 1" | tr -d '\r') || moddir=""
    if [[ -z $moddir && -n $abi_sys ]]; then
      moddir=/usr/lib/$MA/SoapySDR/modules$abi_sys
      warn "SoapySDRUtil did not report a search path; assuming $moddir"
    fi
    [[ -n $moddir ]] || die "cannot find SoapySDR's module directory on the Pi: use --module-dir DIR"
  fi
  base=${moddir##*/}
  if [[ $base == modules* && -n $abi_sys && $base != "modules$abi_sys" ]]; then
    ((FORCE)) || die "SoapySDR ABI mismatch: the module was built against ABI $abi_sys (sysroot) but the Pi loads '$base'. Refresh the sysroot ('$SCRIPT_NAME sysroot') and rebuild, or use --force."
    warn "SoapySDR ABI mismatch ($abi_sys vs $base) ignored (--force)"
  fi

  # Copy to a private temporary directory on the Pi, then install as root
  local bindir=$PI_PREFIX/bin files=("$so") cmd
  [[ -f $reset ]] && files+=("$reset")
  files+=("${tools[@]}")
  REMOTE_TMP=$("${SSH[@]}" "$RPI_HOST" 'mktemp -d') || die "cannot create a temporary directory on $RPI_HOST"
  note "Copying ${#files[@]} file(s) to $RPI_HOST ..."
  rsync -a -e "$SSH_RSH" "${files[@]}" "$RPI_HOST:$REMOTE_TMP/"

  cmd="set -e; install -d -m 755 $(shq "$moddir") $(shq "$bindir"); install -m 644 $(shq "$REMOTE_TMP/${so##*/}") $(shq "$moddir/${so##*/}")"
  if [[ -f $reset ]]; then
    cmd+="; install -m 755 $(shq "$REMOTE_TMP/${reset##*/}") $(shq "$bindir/${reset##*/}")"
  fi
  for t in "${tools[@]}"; do
    cmd+="; install -m 755 $(shq "$REMOTE_TMP/${t##*/}") $(shq "$bindir/${t##*/}")"
  done
  note "Installing on the Pi: ${so##*/} -> $moddir ; tools -> $bindir"
  pi_root "$cmd"

  local info
  info=$("${SSH[@]}" "$RPI_HOST" 'SoapySDRUtil --info 2>&1' || true)
  if grep -qi 'rpitx' <<<"$info"; then
    note "OK: SoapySDR on the Pi loads the rpitx driver."
  else
    warn "SoapySDRUtil --info on the Pi does not list rpitx: check '$moddir' and the SoapySDR ABI."
  fi
  note "Test on the Pi (dummy load!):  sudo SoapySDRUtil --find=\"driver=rpitx\"   /   sudo rpitx_tone_test.py --freq 144.800M"
}

cmd_env() {
  prepare
  export_env
  local v
  for v in CROSS_COMPILE CC CXX CPP AR AS LD NM OBJCOPY OBJDUMP RANLIB READELF STRIP CFLAGS CXXFLAGS LDFLAGS \
           RPI_MODEL RPI_BITS RPI_TRIPLET RPI_SYSROOT PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_LIBDIR; do
    [[ -n ${!v:-} ]] && printf 'export %s=%q\n' "$v" "${!v}"
  done
  [[ $SYSROOT != none ]] && echo 'unset PKG_CONFIG_PATH'
  return 0
}

cmd_cmake_toolchain() {
  prepare
  compose_env
  local out=${OUT_FILE:-$PWD/rpi-$MODEL-$BITS.cmake} proc
  [[ $BITS == 64 ]] && proc=aarch64 || proc=arm
  {
    cat <<EOF
# Generated by $SCRIPT_NAME $VERSION - target: $NAME ($BITS-bit, $SOC, $CORE)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $proc)
set(CMAKE_C_COMPILER   $(command -v "${PREFIX}gcc"))
set(CMAKE_CXX_COMPILER $(command -v "${PREFIX}g++"))
set(CMAKE_C_COMPILER_TARGET   $TRIPLET)
set(CMAKE_CXX_COMPILER_TARGET $TRIPLET)
EOF
    if [[ $SYSROOT != none ]]; then
      cat <<EOF
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_FIND_ROOT_PATH $SYSROOT)
set(ENV{PKG_CONFIG_SYSROOT_DIR} "$SYSROOT")
set(ENV{PKG_CONFIG_LIBDIR} "$SYSROOT/usr/lib/$MA/pkgconfig:$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig:$SYSROOT/usr/local/lib/pkgconfig")
EOF
    fi
    cat <<EOF
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_C_FLAGS_INIT   "$ARCH_FLAGS $C_INC $EXTRA_CFLAGS")
set(CMAKE_CXX_FLAGS_INIT "$ARCH_FLAGS $CXX_INC $C_INC $EXTRA_CFLAGS")
set(_rpi_ld "${LD_FLAGS_ALL//$SYSROOT_FLAG/}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "\${_rpi_ld}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "\${_rpi_ld}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "\${_rpi_ld}")
EOF
    if [[ -n ${LIBGCC_S:-} ]]; then
      cat <<EOF
# ${PREFIX}g++ has no target libgcc_s: link the Pi's at the end of every link line.
set(CMAKE_C_STANDARD_LIBRARIES_INIT   "$LIBGCC_S")
set(CMAKE_CXX_STANDARD_LIBRARIES_INIT "$LIBGCC_S")
EOF
    fi
  } >"$out"
  note "Wrote $out"
}

# --- Eclipse (CDT managed-build) generated makefiles ------------------------
patch_eclipse_makefiles() {   # make build/makefile + subdir.mk honour $(CXX)/$(CC)/EXTRA_* (idempotent)
  local bd=$1 f
  [[ -f $bd/makefile ]] || return 0
  grep -q 'rpi-cross.sh patched' "$bd/makefile" && return 0
  note "Patching Eclipse makefiles in $bd to honour CXX/CC/EXTRA_*FLAGS ..."
  while IFS= read -r -d '' f; do
    sed -i -e 's/^\tg++ /\t$(CXX) $(EXTRA_CXXFLAGS) /' -e 's/^\tgcc /\t$(CC) $(EXTRA_CFLAGS_ECL) /' "$f"
  done < <(find "$bd" -name subdir.mk -print0)
  # linker rules live in makefile itself
  sed -i -e 's#^\tg++ -L/opt/vc/lib #\t$(CXX) $(EXTRA_LDFLAGS) #' \
         -e 's#^\tg++ #\t$(CXX) $(EXTRA_LDFLAGS) #' \
         -e 's#^\tgcc #\t$(CC) $(EXTRA_LDFLAGS) #' "$bd/makefile"
  sed -i '1i # rpi-cross.sh patched\nCXX ?= g++\nCC ?= gcc\nEXTRA_CXXFLAGS ?=\nEXTRA_CFLAGS_ECL ?=\nEXTRA_LDFLAGS ?=' "$bd/makefile"
}

write_makefile_init() {       # ../makefile.init relative to build/ = project root
  local f=$1/makefile.init
  if [[ -f $f ]] && ! grep -q 'generated by rpi-cross.sh' "$f"; then
    die "$f exists and was not generated by this script - move it away or edit it by hand"
  fi
  compose_env
  cat >"$f" <<EOF
# generated by rpi-cross.sh - target: $NAME ($BITS-bit). Re-generated on every 'build'; do not edit.
CXX := ${PREFIX}g++
CC  := ${PREFIX}gcc
EXTRA_CXXFLAGS     := $CXX_FLAGS_ALL
EXTRA_CFLAGS_ECL   := $C_FLAGS_ALL
EXTRA_LDFLAGS      := $LD_LINK_ALL
EOF
  note "Wrote $f"
}

cmd_build() {
  prepare
  [[ -d $PROJECT_DIR ]] || die "project directory '$PROJECT_DIR' not found"
  cd "$PROJECT_DIR"

  if [[ -f CMakeLists.txt ]]; then
    have cmake || die "cmake not installed (run: $SCRIPT_NAME deps)"
    local bdir=${OUT_FILE:-build-rpi-$MODEL-$BITS} tc
    tc="$PROJECT_DIR/$bdir.toolchain.cmake"
    OUT_FILE=$tc cmd_cmake_toolchain
    note ">> cmake -S . -B $bdir"
    cmake -S . -B "$bdir" -DCMAKE_TOOLCHAIN_FILE="$tc" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
    cmake --build "$bdir" -j"$JOBS"
    note "Done. Output in: $PROJECT_DIR/$bdir"
  elif [[ -f build/makefile ]] && grep -q 'makefile.init' build/makefile; then
    note ">> Eclipse CDT project detected (build/makefile)"
    patch_eclipse_makefiles build
    write_makefile_init "$PROJECT_DIR"
    make -C build -j"$JOBS" all
    note "Done. Output in: $PROJECT_DIR/build"
  elif [[ -f Makefile || -f makefile || -f GNUmakefile ]]; then
    export_env
    note ">> make -j$JOBS  (CC/CXX/CFLAGS/CXXFLAGS/LDFLAGS from the cross environment)"
    make -j"$JOBS" CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP"
  else
    die "nothing to build in $PROJECT_DIR (looked for CMakeLists.txt, build/makefile, Makefile). Use '$SCRIPT_NAME exec -- ...'."
  fi
}

cmd_exec() {
  [[ $# -gt 0 ]] || die "exec needs a command after --"
  prepare
  export_env
  note ">> $* (model $MODEL, $BITS-bit)"
  (cd "$PROJECT_DIR" && exec "$@")
}

cmd_verify() {
  local f=${1:-}
  [[ -n $f && -f $f ]] || die "usage: $SCRIPT_NAME verify -m MODEL -b BITS FILE"
  prepare
  local readelf="${PREFIX}readelf" objdump="${PREFIX}objdump"
  have "$objdump" || die "$objdump not found"

  echo "File     : $f"
  echo "Machine  : $("$readelf" -h "$f" | awk -F: '/Machine:/ {gsub(/^ +/,"",$2); print $2}')  /  $("$readelf" -h "$f" | awk -F: '/Class:/ {gsub(/^ +/,"",$2); print $2}')"
  if [[ $BITS == 32 ]]; then
    "$readelf" -A "$f" 2>/dev/null | grep -E 'Tag_CPU_arch:|Tag_ABI_VFP_args|Tag_FP_arch|Tag_THUMB_ISA_use' | sed 's/^ */           /' || true
  fi

  local need_c need_cxx have_c have_cxx libc libcxx
  need_c=$("$objdump" -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9.]*'   | sort -uV | tail -1 || true)
  need_cxx=$("$objdump" -T "$f" 2>/dev/null | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1 || true)
  echo "Needs    : ${need_c:-no glibc symbols}  ${need_cxx:-}"

  if [[ $SYSROOT == none || ! -d $SYSROOT ]]; then
    warn "no sysroot to compare against; compare with 'ldd --version' / libstdc++ on the Pi manually"; return 0
  fi
  libc=$(find "$SYSROOT/lib/$MA" "$SYSROOT/usr/lib/$MA" -maxdepth 1 -name 'libc.so.6' 2>/dev/null | head -1 || true)
  libcxx=$(find "$SYSROOT/usr/lib/$MA" "$SYSROOT/lib/$MA" -maxdepth 1 -name 'libstdc++.so.6' 2>/dev/null | head -1 || true)
  local rc=0 top
  if [[ -n $libc && -n $need_c ]]; then
    have_c=$("$objdump" -T "$libc" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 || true)
    top=$(printf '%s\n%s\n' "$need_c" "$have_c" | sort -V | tail -1)
    if [[ $top == "$have_c" ]]; then echo "glibc    : OK   (binary needs $need_c, Pi provides up to $have_c)"
    else echo "glibc    : FAIL (binary needs $need_c, Pi provides only $have_c) - rebuild against the Pi sysroot"; rc=1; fi
  fi
  if [[ -n $libcxx && -n $need_cxx ]]; then
    have_cxx=$("$objdump" -T "$libcxx" 2>/dev/null | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1 || true)
    top=$(printf '%s\n%s\n' "$need_cxx" "$have_cxx" | sort -V | tail -1)
    if [[ $top == "$have_cxx" ]]; then echo "libstdc++: OK   (binary needs $need_cxx, Pi provides up to $have_cxx)"
    else echo "libstdc++: FAIL (binary needs $need_cxx, Pi provides only $have_cxx) - use the Pi's C++ headers (drop --host-libstdcxx)"; rc=1; fi
  fi
  return $rc
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
[[ $# -gt 0 ]] || { usage; exit 0; }
COMMAND=$1; shift
POSITIONAL=(); PASSTHRU=()

while (($#)); do
  case $1 in
    -m|--model)         [[ $# -ge 2 ]] || die "$1 needs a value"; MODEL=$2; shift 2 ;;
    -b|--bits)          [[ $# -ge 2 ]] || die "$1 needs a value"; BITS=$2; shift 2 ;;
    -s|--sysroot)       [[ $# -ge 2 ]] || die "$1 needs a value"; SYSROOT=$2; shift 2 ;;
    -H|--host)          [[ $# -ge 2 ]] || die "$1 needs a value"; RPI_HOST=$2; shift 2 ;;
    --pi-packages)      [[ $# -ge 2 ]] || die "$1 needs a value"; PI_PACKAGES=$2; shift 2 ;;
    --pi-password)      [[ $# -ge 2 ]] || die "$1 needs a value"; PI_PASSWORD=$2; shift 2
                        warn "--pi-password is visible in 'ps' and in the shell history; prefer --ask-pass or RPI_PI_PASSWORD" ;;
    --ask-pass)         ASK_PASS=1; shift ;;
    -P|--prefix)        [[ $# -ge 2 ]] || die "$1 needs a value"; PREFIX=$2; shift 2 ;;
    -T|--toolchain-dir) [[ $# -ge 2 ]] || die "$1 needs a value"; TOOLCHAIN_DIR=$2; shift 2 ;;
    --gcc-version)      [[ $# -ge 2 ]] || die "$1 needs a value"; GCCVER=$2; shift 2 ;;
    --host-libstdcxx)   USE_PI_LIBSTDCXX=0; shift ;;
    -C|--dir)           [[ $# -ge 2 ]] || die "$1 needs a value"; PROJECT_DIR=$(realpath -m "$2"); shift 2 ;;
    -o|--output)        [[ $# -ge 2 ]] || die "$1 needs a value"; OUT_FILE=$2; shift 2 ;;
    -j|--jobs)          [[ $# -ge 2 ]] || die "$1 needs a value"; JOBS=$2; shift 2 ;;
    --cflags)           [[ $# -ge 2 ]] || die "$1 needs a value"; EXTRA_CFLAGS=$2; shift 2 ;;
    --ldflags)          [[ $# -ge 2 ]] || die "$1 needs a value"; EXTRA_LDFLAGS=$2; shift 2 ;;
    --distro)           [[ $# -ge 2 ]] || die "$1 needs a value"; DISTRO=$2; shift 2 ;;
    --module-dir)       [[ $# -ge 2 ]] || die "$1 needs a value"; MODULE_DIR=$2; shift 2 ;;
    --pi-prefix)        [[ $# -ge 2 ]] || die "$1 needs a value"; PI_PREFIX=$2; shift 2 ;;
    --no-build)         NO_BUILD=1; shift ;;
    --no-tools)         NO_TOOLS=1; shift ;;
    --force)            FORCE=1; shift ;;
    -h|--help)          usage; exit 0 ;;
    --)                 shift; PASSTHRU=("$@"); break ;;
    -*)                 die "unknown option: $1 (see '$SCRIPT_NAME help')" ;;
    *)                  POSITIONAL+=("$1"); shift ;;
  esac
done

[[ -n $SYSROOT && $SYSROOT != none ]] && SYSROOT=$(realpath -m "$SYSROOT")

case $COMMAND in
  models)          cmd_models ;;
  info)            cmd_info ;;
  deps)            cmd_deps ;;
  sysroot)         cmd_sysroot ;;
  install)         cmd_install ;;
  env)             cmd_env ;;
  cmake-toolchain) cmd_cmake_toolchain ;;
  build)           cmd_build ;;
  exec)            cmd_exec "${PASSTHRU[@]}" ;;
  verify)          cmd_verify "${POSITIONAL[0]:-}" ;;
  help|-h|--help)  usage ;;
  *)               die "unknown command '$COMMAND' (see '$SCRIPT_NAME help')" ;;
esac
