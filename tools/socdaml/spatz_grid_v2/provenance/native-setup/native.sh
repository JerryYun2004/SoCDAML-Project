#!/usr/bin/env bash
# Experimental native Apple Silicon bring-up. Run with bash, do not source.
set -Eeuo pipefail

kit_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
stage="${1:-help}"
if [ "$stage" = help ] || [ "$stage" = --help ]; then
    printf '%s\n' \
      'bash native.sh prepare CLEANUP_REPO NATIVE_DIRECTORY' \
      'bash native.sh dependencies NATIVE_DIRECTORY' \
      'bash native.sh simulator NATIVE_DIRECTORY' \
      'bash native.sh toolchain NATIVE_DIRECTORY' \
      'bash native.sh app NATIVE_DIRECTORY [diagnostics/timer]' \
      'bash native.sh run NATIVE_DIRECTORY' \
      'bash native.sh gemm-prepare NATIVE_DIRECTORY' \
      'bash native.sh gemm-run NATIVE_DIRECTORY' \
      'bash native.sh row-prepare NATIVE_DIRECTORY' \
      'bash native.sh row-run NATIVE_DIRECTORY row_direct|row_broadcast' \
      'bash native.sh row-compare NATIVE_DIRECTORY' \
      'bash native.sh bench-prepare NATIVE_DIRECTORY' \
      'bash native.sh bench-run NATIVE_DIRECTORY row_direct|row_broadcast [REPETITIONS=3]' \
      'bash native.sh bench-compare NATIVE_DIRECTORY' \
      'bash native.sh profile-prepare NATIVE_DIRECTORY' \
      'bash native.sh profile-app NATIVE_DIRECTORY row_direct|row_broadcast' \
      'bash native.sh profile-run NATIVE_DIRECTORY row_direct|row_broadcast' \
      'bash native.sh profile-compare NATIVE_DIRECTORY' \
      'bash native.sh reproduce-row NATIVE_DIRECTORY' \
      'bash native.sh reproduce-row-prepare NATIVE_DIRECTORY' \
      'bash native.sh reproduce-grid NATIVE_DIRECTORY' \
      'bash native.sh reproduce-grid-prepare NATIVE_DIRECTORY' \
      'bash native.sh application-grid NATIVE_DIRECTORY' \
      'bash native.sh application-row NATIVE_DIRECTORY' \
      'bash native.sh application-grid-prepare NATIVE_DIRECTORY' \
      'bash native.sh application-row-prepare NATIVE_DIRECTORY' \
      'bash native.sh kernel-control-grid NATIVE_DIRECTORY' \
      'bash native.sh kernel-control-grid-prepare NATIVE_DIRECTORY' \
      'bash native.sh presentation-spatz NATIVE_DIRECTORY' \
      'bash native.sh presentation-grid NATIVE_DIRECTORY' \
      'bash native.sh presentation-grid-diagnose NATIVE_DIRECTORY FAILED_EVIDENCE_DIRECTORY'
    exit 0
fi
case "$stage" in prepare|dependencies|simulator|toolchain|app|run|gemm-prepare|gemm-run|row-prepare|row-run|row-compare|bench-prepare|bench-run|bench-compare|profile-prepare|profile-app|profile-run|profile-compare|reproduce-row|reproduce-row-prepare|reproduce-grid|reproduce-grid-prepare|application-grid|application-row|application-grid-prepare|application-row-prepare|kernel-control-grid|kernel-control-grid-prepare|presentation-spatz|presentation-grid|presentation-grid-diagnose) ;;
    *) echo "Unknown stage: $stage" >&2; exit 2;; esac
[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || {
    echo 'This script requires a native arm64 macOS terminal.' >&2; exit 2;
}
command -v brew >/dev/null || { echo 'Install Homebrew first: https://brew.sh'; exit 2; }
xcode-select -p >/dev/null
brew_prefix="$(brew --prefix)"
[ "$brew_prefix" = /opt/homebrew ] || {
    echo "Expected Apple Silicon Homebrew at /opt/homebrew; got $brew_prefix"; exit 2;
}
bootstrap_python="$(brew --prefix python@3.11)/bin/python3.11"
[ -x "$bootstrap_python" ] || { echo 'Run the Homebrew command in START_HERE.md first.'; exit 2; }

if [ "$stage" = prepare ]; then
    [ "$#" = 3 ] || { echo 'prepare requires CLEANUP_REPO NATIVE_DIRECTORY'; exit 2; }
    "$bootstrap_python" "$kit_dir/prepare.py" init "$2" "$3"
    native_root="$(cd "$3" && pwd -P)"
else
    [ "$#" -ge 2 ] || { echo 'A native directory is required.'; exit 2; }
    native_root="$(cd "$2" && pwd -P)"
    [ -f "$native_root/.socdaml-native-v1" ] || { echo 'Run prepare first.'; exit 2; }
fi
jobs="${SOCDAML_JOBS:-2}"
case "$jobs" in [1-8]) ;; *) echo 'SOCDAML_JOBS must be 1 through 8.'; exit 2;; esac
mkdir -p "$native_root/logs"
log_file="$native_root/logs/$(date +%Y%m%d-%H%M%S)-$stage-$$.log"
exec > >(tee "$log_file") 2>&1
finish() {
    rc=$?
    trap - EXIT
    printf 'stage=%s\nexit_code=%s\nlog=%s\n' "$stage" "$rc" "$log_file" \
      > "$native_root/logs/last-status.txt"
    if [ "$rc" -ne 0 ]; then
        printf '\nSTOP: %s failed (exit %s). Log: %s\n' "$stage" "$rc" "$log_file"
    else
        printf '\nCompleted stage: %s. Log: %s\n' "$stage" "$log_file"
    fi
    exit "$rc"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Do not inherit a prior Linux/toolchain configuration into native builds.
unset CC CXX CFLAGS CXXFLAGS CPPFLAGS LDFLAGS MAKEFLAGS
unset LD_LIBRARY_PATH DYLD_LIBRARY_PATH PYTHONPATH PYTHONHOME
unset CMAKE_PREFIX_PATH CMAKE_TOOLCHAIN_FILE
export CC="$(xcrun --find clang)"
export CXX="$(xcrun --find clang++)"
export PATH="$brew_prefix/bin:/usr/bin:/bin:/usr/sbin:/sbin"
macos_sdk="$(xcrun --sdk macosx --show-sdk-path)"
[ -d "$macos_sdk" ] || { echo "macOS SDK directory is missing: $macos_sdk"; exit 2; }
export SDKROOT="$macos_sdk"
printf 'macOS SDK: %s\n' "$macos_sdk"

checkout_dependency() {
    dep_path="$1"; dep_url="$2"; dep_sha="$3"
    if [ ! -d "$dep_path/.git" ]; then
        [ ! -e "$dep_path" ] || { echo "Unexpected existing directory: $dep_path"; exit 2; }
        git init "$dep_path"
        git -C "$dep_path" remote add origin "$dep_url"
    fi
    if ! git -C "$dep_path" rev-parse --verify HEAD >/dev/null 2>&1; then
        git -C "$dep_path" fetch --depth=1 origin "$dep_sha"
        git -C "$dep_path" checkout --detach "$dep_sha"
    fi
    [ "$(git -C "$dep_path" rev-parse HEAD)" = "$dep_sha" ] || {
        echo "Unexpected revision in $dep_path; refusing to reset it."; exit 2;
    }
}

check_python_compatibility() {
    python - <<'PY'
from importlib.metadata import version
from pkg_resources import resource_filename
print(f"PASS: pkg_resources.resource_filename is available (setuptools {version('setuptools')}).")
PY
    python -m pip check
    # pip freeze normally omits setuptools on Python 3.11; retain the pin too.
    python -m pip freeze --all > "$native_root/logs/python-freeze.txt"
}

configure_toolchain_host() {
    # Autoconf invokes the compiler directly; SDKROOT alone did not make the
    # Command Line Tools linker find libSystem. Keep the SDK on host compiler
    # wrappers, not global flags that could reach RISC-V runtime compilation.
    host_tools="$native_root/build/toolchain-host"
    mkdir -p "$host_tools/bin"
    for driver in cc cxx; do
        if [ "$driver" = cc ]; then
            host_compiler="$(xcrun --find clang)"
        else
            host_compiler="$(xcrun --find clang++)"
        fi
        printf '#!/bin/bash\nexec %q -isysroot %q "$@"\n' \
          "$host_compiler" "$macos_sdk" > "$host_tools/bin/$driver"
        chmod 755 "$host_tools/bin/$driver"
    done
    export CC="$host_tools/bin/cc"
    export CXX="$host_tools/bin/cxx"
    export CC_FOR_BUILD="$CC"
    export CXX_FOR_BUILD="$CXX"
    cat > "$host_tools/probe.c" <<'C'
#include <stdio.h>
int main(void) {
    puts("PASS: macOS host C compile/link/run with explicit SDK");
    return 0;
}
C
    cat > "$host_tools/probe.cpp" <<'CXX'
#include <iostream>
#include <string>
int main() {
    std::string message = "PASS: macOS host C++ compile/link/run with explicit SDK";
    std::cout << message << '\n';
    return 0;
}
CXX
    "$CC" "$host_tools/probe.c" -o "$host_tools/probe-c"
    "$CXX" -std=c++17 "$host_tools/probe.cpp" -o "$host_tools/probe-cxx"
    file "$host_tools/probe-c" "$host_tools/probe-cxx"
    "$host_tools/probe-c"
    "$host_tools/probe-cxx"
    # Binutils must use the installed host zlib instead of its legacy bundled
    # copy, whose classic-Mac fdopen macro conflicts with current SDK headers.
    cat > "$host_tools/probe-zlib.c" <<'ZLIB'
#include <stdio.h>
#include <string.h>
#include <zlib.h>
int main(void) {
    static const unsigned char input[] = "SoCDAML native toolchain zlib probe";
    unsigned char compressed[256], output[sizeof(input)];
    uLongf compressed_size = sizeof(compressed), output_size = sizeof(output);
    if (compress2(compressed, &compressed_size, input, sizeof(input), Z_BEST_SPEED) != Z_OK)
        return 1;
    if (uncompress(output, &output_size, compressed, compressed_size) != Z_OK)
        return 2;
    if (output_size != sizeof(input) || memcmp(input, output, sizeof(input)) != 0)
        return 3;
    printf("PASS: macOS host zlib compress/decompress (version %s)\n", zlibVersion());
    return 0;
}
ZLIB
    "$CC" "$host_tools/probe-zlib.c" -o "$host_tools/probe-zlib" -lz
    "$host_tools/probe-zlib"
}

configure_toolchain_math() {
    gmp_prefix="$(brew --prefix gmp)"
    mpfr_prefix="$(brew --prefix mpfr)"
    mpc_prefix="$(brew --prefix libmpc)"
    isl_prefix="$(brew --prefix isl)"
    math_includes=()
    math_libdirs=()
    for math_prefix in "$gmp_prefix" "$mpfr_prefix" "$mpc_prefix" "$isl_prefix"; do
        # These paths become whitespace-separated Autoconf/make flags below.
        case "$math_prefix" in
            *[[:space:]]*) echo "Unsupported Homebrew path: $math_prefix"; exit 2;;
        esac
        math_includes+=("-I$math_prefix/include")
        math_libdirs+=("-L$math_prefix/lib")
    done
    for math_file in \
      "$gmp_prefix/include/gmp.h" "$gmp_prefix/lib/libgmp.dylib" \
      "$mpfr_prefix/include/mpfr.h" "$mpfr_prefix/lib/libmpfr.dylib" \
      "$mpc_prefix/include/mpc.h" "$mpc_prefix/lib/libmpc.dylib" \
      "$isl_prefix/include/isl/ctx.h" "$isl_prefix/lib/libisl.dylib"
    do
        [ -f "$math_file" ] || {
            echo "Missing Homebrew dependency: $math_file"
            echo 'Run: brew install gmp mpfr libmpc isl'
            exit 2
        }
    done
    brew list --versions gmp mpfr libmpc isl
    cat > "$host_tools/probe-math.c" <<'MATH'
#include <stdio.h>
#include <gmp.h>
#include <mpfr.h>
#include <mpc.h>
#include <isl/ctx.h>
#include <isl/schedule.h>
int main(void) {
    mpz_t z;
    mpfr_t r;
    mpc_t c;
    isl_ctx *ctx;
    mpz_init_set_ui(z, 6);
    mpz_mul_ui(z, z, 7);
    if (mpz_cmp_ui(z, 42) != 0) return 1;
    mpfr_init2(r, 128);
    mpfr_set_z(r, z, MPFR_RNDN);
    if (mpfr_cmp_ui(r, 42) != 0) return 2;
    mpc_init2(c, 128);
    mpc_set_fr(c, r, MPC_RNDNN);
    if (mpc_cmp_si_si(c, 42, 0) != 0) return 3;
    ctx = isl_ctx_alloc();
    if (!ctx) return 4;
    if (isl_options_set_schedule_serialize_sccs(ctx, 0) != 0) return 5;
    isl_ctx_free(ctx);
    mpc_clear(c);
    mpfr_clear(r);
    mpz_clear(z);
    puts("PASS: Homebrew GMP/MPFR/MPC/ISL compile/link/run");
    return 0;
}
MATH
    "$CC" "${math_includes[@]}" "$host_tools/probe-math.c" \
      "${math_libdirs[@]}" -lmpc -lmpfr -lisl -lgmp -lm -o "$host_tools/probe-math-c"
    "$host_tools/probe-math-c"
    "$CXX" -x c++ "${math_includes[@]}" "$host_tools/probe-math.c" \
      "${math_libdirs[@]}" -lmpc -lmpfr -lisl -lgmp -lm -o "$host_tools/probe-math-cxx"
    "$host_tools/probe-math-cxx"
    math_cppflags="${math_includes[*]}"
    math_ldflags="${math_libdirs[*]}"
}

if [ "$stage" = prepare ]; then
    "$bootstrap_python" -m venv "$native_root/venv"
    source "$native_root/venv/bin/activate"
    python -m pip install 'cmake==3.28.4' ninja
    python "$kit_dir/prepare.py" restore "$native_root"
    # argparse/dataclasses are part of Python 3.11; omit obsolete backports.
    python "$kit_dir/prepare.py" requirements "$native_root"
    python -m pip install -r "$native_root/requirements-native.txt" \
      -r "$kit_dir/python-compat.txt"
    check_python_compatibility
    git -C "$native_root/source" submodule update --init --recursive
    python "$kit_dir/prepare.py" patch "$native_root"
    python "$kit_dir/prepare.py" env "$native_root" "$CC" "$CXX" "$brew_prefix"
    sw_vers
    uname -m
    "$CC" --version
    cmake --version
    echo 'Preparation complete. Next: dependencies.'
    exit 0
fi

[ -f "$native_root/env.sh" ] || { echo 'Preparation is incomplete; rerun prepare.'; exit 2; }
source "$native_root/env.sh"
source_root="$native_root/source"
mkdir -p "$native_root/deps" "$native_root/build" "$native_root/prefix"

case "$stage" in
dependencies)
    checkout_dependency "$native_root/deps/systemc" \
      https://github.com/accellera-official/systemc.git \
      38b8a2c61aafe40de44296bee0c9c28ac4e80b01
    python "$kit_dir/prepare.py" systemc-patch "$native_root"
    cmake -S "$native_root/deps/systemc" -B "$native_root/build/systemc" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 \
      -DCMAKE_OSX_SYSROOT="$macos_sdk" \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_SHARED_LIBS=ON \
      -DENABLE_PTHREADS=ON -DCMAKE_INSTALL_LIBDIR=lib64 \
      -DCMAKE_INSTALL_PREFIX="$SYSTEMC_HOME"
    cmake --build "$native_root/build/systemc" --parallel "$jobs"
    cmake --install "$native_root/build/systemc"
    test -f "$SYSTEMC_HOME/lib64/libsystemc.dylib"
    file "$SYSTEMC_HOME/lib64/libsystemc.dylib"

    checkout_dependency "$native_root/deps/DRAMSys" \
      https://github.com/tukl-msd/DRAMSys.git \
      8e021eada2cb937d5be24dbd02f3980e2193d6ec
    python "$kit_dir/prepare.py" dram-patch "$native_root"
    # Require the SAME installed SystemC as GVSoC; never auto-fetch a second one.
    cmake -S "$native_root/deps/DRAMSys" -B "$native_root/build/dramsys" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 \
      -DCMAKE_OSX_SYSROOT="$macos_sdk" \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_PREFIX_PATH="$SYSTEMC_HOME" \
      -DCMAKE_BUILD_RPATH="$SYSTEMC_HOME/lib64" \
      -DDRAMSYS_WITH_DRAMPOWER=ON -DDRAMSYS_BUILD_TESTS=OFF
    cmake --build "$native_root/build/dramsys" --target DRAMSys_Simulator --parallel "$jobs"
    test -f "$SOCDAML_DRAMSYS_LIBRARY"
    file "$SOCDAML_DRAMSYS_LIBRARY"
    otool -L "$SOCDAML_DRAMSYS_LIBRARY"
    # Use the same SDK-aware CMake path that successfully builds the libraries.
    # Keep its source path stable when a newer kit is unpacked elsewhere.
    probe_source="$native_root/probes/dependency"
    mkdir -p "$probe_source"
    cp "$kit_dir/probe/CMakeLists.txt" "$kit_dir/probe.cpp" "$probe_source/"
    cmake -S "$probe_source" -B "$native_root/build/dependency-probe-cmake" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 \
      -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_SYSROOT="$macos_sdk" \
      -DCMAKE_PREFIX_PATH="$SYSTEMC_HOME" \
      -DSystemCLanguage_DIR="$SYSTEMC_HOME/lib64/cmake/SystemCLanguage"
    cmake --build "$native_root/build/dependency-probe-cmake" --parallel "$jobs" --verbose
    "$native_root/build/dependency-probe-cmake/dependency-probe" "$SOCDAML_DRAMSYS_LIBRARY"
    ;;
simulator)
    test -f "$SOCDAML_DRAMSYS_LIBRARY" || { echo 'Run dependencies first.'; exit 2; }
    # Also repair existing workspaces prepared before the compatibility pin.
    python -m pip install -r "$kit_dir/python-compat.txt"
    check_python_compatibility
    sndfile_prefix="$(brew --prefix libsndfile)"
    samplerate_prefix="$(brew --prefix libsamplerate)"
    for host_file in \
      "$sndfile_prefix/include/sndfile.hh" \
      "$sndfile_prefix/include/sndfile.h" \
      "$sndfile_prefix/lib/libsndfile.dylib" \
      "$samplerate_prefix/include/samplerate.h" \
      "$samplerate_prefix/lib/libsamplerate.dylib"
    do
        [ -f "$host_file" ] || {
            echo "Missing native audio dependency: $host_file"
            echo 'Run: brew install libsndfile libsamplerate'
            exit 2
        }
    done
    brew list --versions libsndfile libsamplerate
    # Upstream finds the libraries but omits their non-system header paths.
    # Set paths explicitly for both block objects and models consuming them.
    host_cxx_flags="-DSC_USE_PTHREADS -isystem $sndfile_prefix/include -isystem $samplerate_prefix/include"
    host_link_flags="-L$sndfile_prefix/lib -L$samplerate_prefix/lib"
    python "$kit_dir/prepare.py" simulator-patch "$native_root"
    cd "$source_root"
    # This target only generates/copies the hardware and SDK configuration.
    gmake config
    cmake -S "$source_root" -B "$native_root/build/gvsoc" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_STANDARD=17 \
      -DCMAKE_OSX_SYSROOT="$macos_sdk" \
      "-DCMAKE_CXX_FLAGS=$host_cxx_flags" \
      "-DCMAKE_SHARED_LINKER_FLAGS=$host_link_flags" \
      "-DCMAKE_MODULE_LINKER_FLAGS=$host_link_flags" \
      "-DCMAKE_EXE_LINKER_FLAGS=$host_link_flags" \
      "-DCMAKE_PREFIX_PATH=$SYSTEMC_HOME;$sndfile_prefix;$samplerate_prefix" \
      "-DSNDFILE_LIB:FILEPATH=$sndfile_prefix/lib/libsndfile.dylib" \
      "-DSAMPLERATE_LIB:FILEPATH=$samplerate_prefix/lib/libsamplerate.dylib" \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_INSTALL_PREFIX="$source_root/install" \
      "-DCMAKE_INSTALL_RPATH=$source_root/install/lib;$SYSTEMC_HOME/lib64" \
      -DCMAKE_SKIP_INSTALL_RPATH=FALSE \
      "-DGVSOC_MODULES=$source_root/core/models;$source_root/pulp" \
      -DGVSOC_TARGETS=pulp.chips.flex_cluster.flex_cluster \
      -DBUILD_OPTIMIZED=ON -DBUILD_DEBUG=ON
    cmake --build "$native_root/build/gvsoc" --parallel "$jobs"
    cmake --install "$native_root/build/gvsoc"
    file "$source_root/install/bin/gvsoc_launcher"
    otool -L "$source_root/install/bin/gvsoc_launcher"
    "$source_root/install/bin/gvsoc" --help
    echo 'Simulator build completed. No application has been simulated yet.'
    ;;
toolchain)
    configure_toolchain_host
    configure_toolchain_math
    # The repo already names this GNU toolchain revision as a source-build option.
    checkout_dependency "$native_root/deps/riscv-gnu-toolchain" \
      https://github.com/riscv-collab/riscv-gnu-toolchain.git \
      935b263c8ef7f250819c74aeb7736c87ad87ef2b
    mkdir -p "$native_root/build/riscv"
    cd "$native_root/build/riscv"
    # Scope these flags to the top-level HOST library probes. GCC gets its own
    # --with-* options below; no Homebrew flags enter RISC-V runtime builds.
    CPPFLAGS="$math_cppflags" LDFLAGS="$math_ldflags" LIBS="" \
      "$native_root/deps/riscv-gnu-toolchain/configure" \
      --prefix="$native_root/prefix/riscv" \
      --with-arch=rv32imafd_zfh --with-abi=ilp32d --with-cmodel=medlow \
      --with-languages=c,c++ --disable-multilib --disable-gdb
    python "$kit_dir/toolchain_math.py" "$native_root"
    gmake -j"$jobs" newlib \
      "BINUTILS_TARGET_FLAGS_EXTRA=--with-system-zlib" \
      "GCC_EXTRA_CONFIGURE_FLAGS=--with-gmp=$gmp_prefix --with-mpfr=$mpfr_prefix --with-mpc=$mpc_prefix --with-isl=$isl_prefix"
    riscv32-unknown-elf-gcc --version
    file "$native_root/prefix/riscv/bin/riscv32-unknown-elf-gcc"
    printf 'int f(int x) { return x + 1; }\n' > "$native_root/build/riscv-probe.c"
    riscv32-unknown-elf-gcc -march=rv32imafd_zfh -mabi=ilp32d -c \
      "$native_root/build/riscv-probe.c" -o "$native_root/build/riscv-probe.o"
    riscv32-unknown-elf-readelf -h -A "$native_root/build/riscv-probe.o"
    ;;
app)
    # A failed rebuild must not leave an older ELF selected for the smoke run.
    export SOCDAML_APP="${3:-diagnostics/timer}"
    python "$kit_dir/prepare.py" invalidate-app "$native_root" "$SOCDAML_APP"
    command -v riscv32-unknown-elf-gcc >/dev/null || { echo 'Run toolchain first.'; exit 2; }
    python "$kit_dir/prepare.py" check-app "$native_root" "$SOCDAML_APP"
    cd "$source_root"
    gmake config
    # Retain a fresh, identifiable build for each run, including before row-prepare.
    app_build="$(mktemp -d "$native_root/build/app.XXXXXX")"
    cmake -S "$source_root/soft_hier/flex_cluster_sdk" -B "$app_build" -G 'Unix Makefiles' \
      -DCMAKE_OSX_SYSROOT="$macos_sdk" \
      -DRISCV_ARCH=rv32imafd_zfh -DCMAKE_MAKE_PROGRAM="$(command -v gmake)"
    cmake --build "$app_build" --parallel "$jobs"
    test -s "$app_build/softhier.elf"
    if grep -q ebreak "$app_build/softhier.dump"; then
        echo "Upstream ebreak check failed: $app_build/softhier.dump"; exit 1
    fi
    riscv32-unknown-elf-readelf -h -A "$app_build/softhier.elf"
    if [ "$SOCDAML_APP" = gemm/single_cluster_checked ]; then
        python "$kit_dir/gemm_validation.py" record-build "$native_root" "$app_build"
    fi
    case "$SOCDAML_APP" in
        gemm/row_direct_checked|gemm/row_broadcast_checked)
            row_variant="${SOCDAML_APP#gemm/}"
            row_variant="${row_variant%_checked}"
            python "$kit_dir/row_validation.py" record-build "$native_root" "$row_variant" "$app_build"
            ;;
        gemm/row_direct_bench|gemm/row_broadcast_bench)
            bench_variant="${SOCDAML_APP#gemm/}"
            bench_variant="${bench_variant%_bench}"
            python "$kit_dir/row_bench.py" record-build "$native_root" "$bench_variant" "$app_build"
            ;;
    esac
    printf '%s\n' "$app_build" > "$native_root/last-app-build.txt"
    printf '%s\n' "$SOCDAML_APP" > "$native_root/last-app-name.txt"
    shasum -a 256 "$app_build/softhier.elf"
    echo 'Application cross-compiled. This is not a simulator result.'
    ;;
run)
    python "$kit_dir/run_timer.py" "$native_root"
    ;;
gemm-prepare)
    python "$kit_dir/gemm_validation.py" prepare "$native_root"
    ;;
gemm-run)
    python "$kit_dir/gemm_validation.py" run "$native_root"
    ;;
row-prepare)
    python "$kit_dir/row_validation.py" prepare "$native_root"
    ;;
row-run)
    [ "$#" = 3 ] || { echo 'row-run requires row_direct or row_broadcast.'; exit 2; }
    python "$kit_dir/row_validation.py" run "$native_root" "$3"
    ;;
row-compare)
    python "$kit_dir/row_validation.py" compare "$native_root"
    ;;
bench-prepare)
    python "$kit_dir/row_bench.py" prepare "$native_root"
    ;;
bench-run)
    [ "$#" -ge 3 ] && [ "$#" -le 4 ] || { echo 'bench-run requires variant and optional repetitions.'; exit 2; }
    python "$kit_dir/row_bench.py" run "$native_root" "$3" "${4:-3}"
    ;;
bench-compare)
    python "$kit_dir/row_bench.py" compare "$native_root"
    ;;
reproduce-row|reproduce-row-prepare)
    [ "$#" = 2 ] || { echo "$stage requires only the native directory."; exit 2; }
    if [ "$stage" = reproduce-row-prepare ]; then
        python "$kit_dir/reproduction/row_strategy.py" "$native_root" --prepare-only
    else
        python "$kit_dir/reproduction/row_strategy.py" "$native_root"
    fi
    ;;
reproduce-grid|reproduce-grid-prepare)
    [ "$#" = 2 ] || { echo "$stage requires only the native directory."; exit 2; }
    if [ "$stage" = reproduce-grid-prepare ]; then
        python "$kit_dir/reproduction/grid_strategy.py" "$native_root" --prepare-only
    else
        python "$kit_dir/reproduction/grid_strategy.py" "$native_root"
    fi
    ;;
presentation-spatz)
    [ "$#" = 2 ] || { echo "$stage requires only the native directory."; exit 2; }
    export SOCDAML_JOBS="$jobs"
    python "$kit_dir/reproduction/presentation_spatz.py" "$native_root"
    ;;
presentation-grid-diagnose)
    [ "$#" = 3 ] || { echo "$stage requires the native directory and failed evidence directory."; exit 2; }
    python "$kit_dir/reproduction/grid_diagnose.py" "$native_root" "$3"
    ;;
presentation-grid)
    [ "$#" = 2 ] || { echo "$stage requires only the native directory."; exit 2; }
    export SOCDAML_JOBS="$jobs"
    python "$kit_dir/reproduction/presentation_grid.py" "$native_root"
    ;;
kernel-control-grid|kernel-control-grid-prepare)
    [ "$#" = 2 ] || { echo "$stage requires only the native directory."; exit 2; }
    if [ "$stage" = kernel-control-grid-prepare ]; then
        python "$kit_dir/reproduction/kernel_control.py" "$native_root" --prepare-only
    else
        python "$kit_dir/reproduction/kernel_control.py" "$native_root"
    fi
    ;;
application-grid|application-row|application-grid-prepare|application-row-prepare)
    [ "$#" = 2 ] || { echo "$stage requires only the native directory."; exit 2; }
    application_pair="${stage#application-}"
    case "$application_pair" in
        *-prepare)
            application_pair="${application_pair%-prepare}"
            python "$kit_dir/reproduction/application_strategy.py" "$application_pair" "$native_root" --prepare-only
            ;;
        *) python "$kit_dir/reproduction/application_strategy.py" "$application_pair" "$native_root" ;;
    esac
    ;;
profile-prepare|profile-app|profile-run|profile-compare)
    profile_action="${stage#profile-}"
    if [ "$profile_action" = app ] || [ "$profile_action" = run ]; then
        [ "$#" = 3 ] || { echo "$stage requires row_direct or row_broadcast."; exit 2; }
        python "$kit_dir/row_profile.py" "$profile_action" "$native_root" "$3"
    else
        [ "$#" = 2 ] || { echo "$stage requires only the native directory."; exit 2; }
        python "$kit_dir/row_profile.py" "$profile_action" "$native_root"
    fi
    ;;
esac
