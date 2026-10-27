#!/usr/bin/env python3
"""Prepare an isolated worktree snapshot and explicit native-only adjustments."""
import ast
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile

KIT = Path(__file__).resolve().parent
CORE = "bda268a4808708a48eab21981c7d6bbced1577d7"
PULP = "3ca2016d056dda72317e27c4319fc6174b5f8c4d"


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def output(*args):
    return subprocess.check_output([str(a) for a in args]).decode().strip()


def apply_once(repo, patch):
    command = ["git", "-C", str(repo), "apply"]
    if subprocess.run(command + ["--reverse", "--check", str(patch)],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        print(f"Already applied: {patch.name}")
        return
    run(*command, "--check", patch)
    run(*command, patch)


def initialize(repo, root):
    repo = repo.resolve()
    root = root.resolve()
    if root.is_relative_to(repo) or repo.is_relative_to(root):
        raise SystemExit("Use separate sibling paths for the cleanup and native directories.")
    if re.search(r"[\s;'\"\\]", str(root)):
        raise SystemExit("Use a native directory path without whitespace, quotes, semicolons or backslashes.")
    if Path(output("git", "-C", repo, "rev-parse", "--show-toplevel")).resolve() != repo:
        raise SystemExit("Pass the repository root, not a subdirectory.")
    if not (repo / "soft_hier/flex_cluster_sdk/app_example/diagnostics/timer.c").is_file():
        raise SystemExit("The cleaned-up diagnostics/timer.c is missing.")
    marker = root / ".socdaml-native-v1"
    if root.exists():
        if not marker.is_file() or marker.read_text().strip() != str(repo):
            raise SystemExit("Native destination already exists and is not this kit's workspace. Use a new path.")
    else:
        root.mkdir(parents=True)
        marker.write_text(str(repo) + "\n")
    (root / "logs").mkdir(exist_ok=True)


def restore(root):
    repo = Path((root / ".socdaml-native-v1").read_text().strip())
    source = root / "source"
    snapshot = root / "snapshot"
    if source.exists():
        if not (root / ".source-restored").exists():
            raise SystemExit("Source restoration was interrupted. Preserve this folder and prepare a new native directory.")
        print("Keeping the existing native source copy; cleanup edits are not resynchronized automatically.")
        return
    snapshot.mkdir(exist_ok=True)
    run(sys.executable, KIT / "snapshot.py", repo, snapshot)
    manifest = json.loads((snapshot / "source-manifest.json").read_text())
    if manifest["gitlinks"] != {"core": CORE, "pulp": PULP}:
        raise SystemExit("This kit targets the inspected core/pulp revisions; snapshot has different gitlinks.")
    run("git", "clone", "--no-checkout", snapshot / "source.bundle", source)
    run("git", "-C", source, "checkout", "--detach", manifest["head"])
    # Only this freshly-created clone is adjusted. Remove baseline tracked files
    # before restoring, so user worktree deletions and staged renames survive.
    tracked = subprocess.check_output(["git", "-C", str(source), "ls-files", "-z"])
    for raw in tracked.split(b"\0"):
        if not raw:
            continue
        path = source / os.fsdecode(raw)
        if path.is_symlink() or path.is_file():
            path.unlink()
    with tarfile.open(snapshot / "source.tar.gz") as archive:
        for member in archive.getmembers():
            path = source / member.name
            if not path.resolve().is_relative_to(source):
                raise SystemExit("Snapshot contains a path outside the destination.")
            if not (member.isfile() or member.issym()):
                raise SystemExit("Unexpected snapshot member type.")
            if member.issym() and not (path.parent / member.linkname).resolve().is_relative_to(source):
                raise SystemExit("Snapshot contains a symlink outside the destination.")
        archive.extractall(source, filter="data")
    (root / ".source-restored").write_text(manifest["head"] + "\n")


def replace_once(path, before, after):
    text = path.read_text()
    if after in text:
        return
    if text.count(before) != 1:
        raise SystemExit(f"Expected exactly one native patch location in {path}")
    path.write_text(text.replace(before, after))


def native_patch(root):
    source = root / "source"
    for name, sha in (("core", CORE), ("pulp", PULP)):
        if output("git", "-C", source / name, "rev-parse", "HEAD") != sha:
            raise SystemExit(f"Unexpected {name} submodule revision.")
        apply_once(source / name, source / f"soft_hier/gvsoc_{name}.patch")
    model = source / "core/models/memory/dramsys.cpp"
    before = '    libraryHandle = dlopen("libDRAMSys_Simulator.so", RTLD_LAZY);'
    after = '''    // SoCDAML native: explicit Mach-O library path, checked before dlsym.
    const char *native_library = getenv("SOCDAML_DRAMSYS_LIBRARY");
    if (native_library == nullptr || native_library[0] == '\\0') {
        fprintf(stderr, "SOCDAML_DRAMSYS_LIBRARY is not set\\n");
        abort();
    }
    libraryHandle = dlopen(native_library, RTLD_NOW | RTLD_GLOBAL);
    if (libraryHandle == nullptr) {
        fprintf(stderr, "Cannot load DRAMSys: %s\\n", dlerror());
        abort();
    }'''
    replace_once(model, before, after)
    cmake = source / "core/models/memory/CMakeLists.txt"
    marker = "# SoCDAML native: explicitly resolve SystemC symbols on macOS."
    if marker not in cmake.read_text():
        with cmake.open("a") as file:
            file.write('\n' + marker + '\nif(APPLE)\n'
                       '    vp_model_link_libraries(NAME memory.dramsys\n'
                       '        LIBRARY "$ENV{SYSTEMC_HOME}/lib64/libsystemc.dylib")\n'
                       'endif()\n')
    shutil.copytree(source / "soft_hier/dramsys_configs",
                    source / "core/models/memory/dramsys_configs", dirs_exist_ok=True)
    (root / "logs/native-core.diff").write_text(output("git", "-C", source / "core", "diff") + "\n")


def simulator_patch(root):
    source = root / "source"
    model = source / "soft_hier/flex_cluster/light_redmule.cpp"
    # Patch the canonical model before `gmake config` copies it into pulp.
    before = r'_this->trace.fatal("[LightRedmule] INVALID RedMule Status: %d\n", _this->state);'
    after = r'_this->trace.fatal("[LightRedmule] INVALID RedMule Status: %u\n", static_cast<unsigned int>(_this->state.get()));'
    replace_once(model, before, after)
    (root / "logs/native-softhier.diff").write_text(
        output("git", "-C", source, "diff", "--",
               "soft_hier/flex_cluster/light_redmule.cpp") + "\n")
    # The memory plugin owns the service object and method implementations.
    # Dispatch through that object so the ISS plugin needs no direct symbols
    # from a separately loaded MODULE library. Rebuild both against this header.
    interface = source / "core/models/memory/memory_memcheck.hpp"
    before = ("    uint64_t alloc(uint64_t offset, uint64_t size);\n"
              "    uint64_t free(uint64_t offset, uint64_t size);")
    after = ("    // SoCDAML native: service calls across independently loaded plugins.\n"
             "    virtual uint64_t alloc(uint64_t offset, uint64_t size);\n"
             "    virtual uint64_t free(uint64_t offset, uint64_t size);")
    replace_once(interface, before, after)
    # The FP receiver has its own Iss layout and slave offload ports. It cannot
    # call the integer core's check_state/handle_req implementations. Keep the
    # ISA tables identical (instructions cross this boundary by pointer), but
    # compile sender-only handlers solely for the integer core.
    generator = source / "pulp/pulp/snitch/snitch_core.py"
    before = '            "-DCONFIG_ISS_CORE=snitch_fp_ss",'
    after = before + '\n            "-DSOCDAML_SNITCH_FP_RECEIVER=1",'
    replace_once(generator, before, after)
    before = '            "cpu/iss/src/snitch/snitch_fp_ss.cpp",'
    after = before + '\n            "cpu/iss/src/snitch/regfile.cpp",'
    replace_once(generator, before, after)
    decoder = source / "core/models/cpu/iss/src/snitch/decode.cpp"
    sender_guard = ("#if defined(CONFIG_GVSOC_ISS_SNITCH) && "
                    "!defined(SOCDAML_SNITCH_FP_RECEIVER)")
    for anchor in (
        "static inline iss_reg_t fp_offload_exec(",
        "static inline iss_reg_t int_offload_exec(",
        "    if (this->iss.snitch & !this->iss.fp_ss)",
    ):
        replace_once(decoder, "#ifdef CONFIG_GVSOC_ISS_SNITCH\n" + anchor,
                     sender_guard + "\n" + anchor)
    frep = source / "core/models/cpu/iss/include/isa/rv32frep.hpp"
    for kind in ("o", "i"):
        signature = (f"static inline iss_reg_t frep_{kind}_exec"
                     "(Iss *iss, iss_insn_t *insn, iss_reg_t pc)\n{\n")
        replace_once(frep, signature + "#ifdef CONFIG_GVSOC_ISS_SNITCH",
                     signature + "#if defined(SOCDAML_SNITCH_FP_RECEIVER)\n"
                     "    // FREP configures the sequencer; it is not forwarded to the FP receiver.\n"
                     '    iss->exec.trace.fatal("FREP reached the FP subsystem; expected sequencer consumption\\n");\n'
                     "    return pc;\n"
                     "#elif defined(CONFIG_GVSOC_ISS_SNITCH)")
    (root / "logs/native-core.diff").write_text(
        output("git", "-C", source / "core", "diff") + "\n")
    (root / "logs/native-pulp.diff").write_text(
        output("git", "-C", source / "pulp", "diff") + "\n")


def dram_patch(root):
    source = root / "source"
    dram = root / "deps/DRAMSys"
    apply_once(dram, source / "soft_hier/build_dynlib_from_github_dramsys5/patch")
    cmake = dram / "CMakeLists.txt"
    text = cmake.read_text()
    start = text.index("### SystemC ###")
    end = text.index("### DRAMPower ###", start)
    replacement = '''### SystemC ###
# SoCDAML native: one shared SystemC instance, also used by GVSoC.
list(APPEND CMAKE_PREFIX_PATH "$ENV{SYSTEMC_HOME}")
# The pinned 2.3.3 release exports a date-qualified CMake package version.
find_package(SystemCLanguage 2.3.3.20181013 EXACT CONFIG REQUIRED)

'''
    cmake.write_text(text[:start] + replacement + text[end:])
    (root / "logs/native-dramsys.diff").write_text(output("git", "-C", dram, "diff") + "\n")


def systemc_patch(root):
    systemc = root / "deps/systemc"
    cmake = systemc / "src/CMakeLists.txt"
    before = "                       INSTALL_NAME_DIR ${CMAKE_INSTALL_FULL_LIBDIR})"
    after = before + '''

# SoCDAML native: link with the final RPATH so repeated installation needs no edit.
if(APPLE)
  set_property(TARGET systemc PROPERTY BUILD_WITH_INSTALL_RPATH TRUE)
endif()
'''
    replace_once(cmake, before, after)
    (root / "logs/native-systemc.diff").write_text(output("git", "-C", systemc, "diff") + "\n")


def write_env(root, cc, cxx, brew):
    source = root / "source"
    paths = [root / "venv/bin", root / "prefix/riscv/bin", source / "install/bin", source / "gapy/bin",
             Path(brew) / "opt/make/libexec/gnubin", Path(brew) / "opt/gnu-sed/libexec/gnubin",
             Path(brew) / "opt/bison/bin", Path(brew) / "opt/flex/bin", Path(brew) / "opt/texinfo/bin",
             Path(brew) / "bin"]
    env = {
        "SOCDAML_NATIVE_ROOT": str(root),
        "SYSTEMC_HOME": str(root / "prefix/systemc"),
        "SOCDAML_DRAMSYS_LIBRARY": str(root / "build/dramsys/lib/libDRAMSys_Simulator.dylib"),
        "CC": cc, "CXX": cxx,
        "PYTHONPATH": f"{source}/install/python:{source}/soft_hier/flex_cluster_utilities",
        "VIRTUAL_ENV": str(root / "venv"),
    }
    lines = ["# Generated for this native workspace. Compatible with bash and zsh."]
    lines += [f"export {key}={shlex.quote(value)}" for key, value in env.items()]
    lines += [f'export PATH={shlex.quote(":".join(map(str, paths)))}:"$PATH"',
              'export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"', "unset PYTHONHOME"]
    (root / "env.sh").write_text("\n".join(lines) + "\n")


def main():
    action = sys.argv[1]
    if action == "init":
        initialize(Path(sys.argv[2]), Path(sys.argv[3]))
        return
    root = Path(sys.argv[2]).resolve()
    if not (root / ".socdaml-native-v1").is_file():
        raise SystemExit("Unrecognized native workspace.")
    if action == "restore":
        restore(root)
    elif action == "requirements":
        lines = (root / "source/requirements.txt").read_text().splitlines()
        (root / "requirements-native.txt").write_text("\n".join(
            line for line in lines if re.split(r"[<>=!~;\s]", line.strip())[0].lower()
            not in {"dataclasses", "argparse"}) + "\n")
    elif action == "patch":
        native_patch(root)
    elif action == "simulator-patch":
        simulator_patch(root)
    elif action == "dram-patch":
        dram_patch(root)
    elif action == "systemc-patch":
        systemc_patch(root)
    elif action == "env":
        write_env(root, *sys.argv[3:6])
    elif action == "invalidate-app":
        from gemm_validation import archive_results
        archive_results(root)
        for name in ("last-app-build.txt", "last-app-name.txt", "logs/timer-result.json",
                     "logs/gemm-single-cluster-result.json", "logs/row-comparison-result.json",
                     "logs/row-benchmark-result.json"):
            (root / name).unlink(missing_ok=True)
        selected = sys.argv[3] if len(sys.argv) > 3 else ""
        for variant in ("row_direct", "row_broadcast"):
            if selected == f"gemm/{variant}_checked":
                (root / "logs" / f"gemm-{variant}-result.json").unlink(missing_ok=True)
            if selected == f"gemm/{variant}_bench":
                (root / "logs" / f"bench-{variant}-result.json").unlink(missing_ok=True)
    elif action == "check-app":
        name = sys.argv[3]
        app_root = root / "source/soft_hier/flex_cluster_sdk/app_example"
        if not re.fullmatch(r"(?:diagnostics|gemm|experimental)/[a-zA-Z0-9_]+", name):
            raise SystemExit("Use a relative application name such as diagnostics/timer.")
        if not (app_root / (name + ".c")).is_file():
            raise SystemExit(f"Application is missing: {name}")
        architecture = root / "source/soft_hier/flex_cluster/flex_cluster_arch.py"
        assignments = [node for node in ast.walk(ast.parse(architecture.read_text()))
                       if isinstance(node, ast.Assign) and any(
                           isinstance(target, ast.Attribute) and target.attr == "spatz_attaced_core_list"
                           for target in node.targets)]
        if len(assignments) != 1 or not isinstance(assignments[0].value, ast.List) or assignments[0].value.elts:
            raise SystemExit("This native compiler setup targets the non-Spatz configuration (spatz_attaced_core_list = []).")
    else:
        raise SystemExit(f"Unknown action: {action}")


if __name__ == "__main__":
    main()
