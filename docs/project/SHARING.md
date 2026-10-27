# Sharing and reproduction dependencies

Publication starts from `aa045b570e5aaeb5df1b44efb2fb4015e51fb4f6`.
The committed tree was reviewed in an isolated checkout without importing the
main workspace's unrelated edits or dirty submodules. No simulator, dependency
installation or environment rebuild is part of this review.

## What a recipient can do

| Level | Required material | Verification status |
|---|---|---|
| Inspect results | This Git tree: applications, reports, JSON, PNG/SVG, attribution | Ready; no raw traces or native environment needed |
| Offline revalidation | This Git tree, Python with standard library, original raw evidence, its ELFs/config/source and manifests | Accepted captures validated; evidence must be supplied separately |
| Execute in existing environment | Pinned Apple Silicon native workspace, installed model/toolchain/libraries, environment, CLI and fresh evidence destination | CLI completed accepted bounded runs; `check` verifies paths/hashes without building |
| Fresh-machine provisioning | Matching source revisions and patches, host tools/libraries, native setup, isolated profile generation/build | **Unverified**; recorded provenance is not an installer guarantee |

The V2 [workflow](../../tools/socdaml/spatz_grid_v2/README.md) uses configurable
`SOCDAML_NATIVE_ROOT` and ELF paths. Historical commands and absolute paths in
captured JSON/status/build records are provenance and are not portable setup
instructions. Existing Mach-O install names can embed the native workspace root;
arbitrary relocation has not been demonstrated.

## Source, tools and exact revisions

Authoritative [metadata](../../tools/socdaml/spatz_grid_v2/reference/metadata.json)
pins upstream source `707cc6f0db370d16f608de7b9ff86e9c758d95e9`, core
`bda268a4808708a48eab21981c7d6bbced1577d7`, pulp
`3ca2016d056dda72317e27c4319fc6174b5f8c4d`, SystemC
`38b8a2c61aafe40de44296bee0c9c28ac4e80b01`, DRAMSys
`8e021eada2cb937d5be24dbd02f3980e2193d6ec`, GNU toolchain
`935b263c8ef7f250819c74aeb7736c87ad87ef2b`, and GCC
`04696df09633baf97cdbbdd6e9929b9d472161d3` (GCC 14.2.0).
[Setup provenance](../../tools/socdaml/spatz_grid_v2/provenance/native-setup/provenance.json)
adds observed recursive dependency/submodule revisions. These observations do
not import those repositories into this Git tree.

External execution requirements include native `env.sh`, `venv/bin/python`,
`prefix/riscv/bin` GCC/objcopy/objdump, `profiles/presentation-spatz-v1/source`
and `install`, SystemC and DRAMSys dylibs, Apple command-line tools/macOS SDK,
and Homebrew libraries such as libsndfile/libsamplerate. The original setup also
uses CMake 3.28.4, Ninja and GNU host build tools. Plotting additionally requires
Matplotlib/NumPy; read-only validation does not. Recorded Python versions and
Homebrew locations are observations, not a hermetic cross-machine lockfile.

The essential setup/build scripts formerly present only in the native kit are
now [archived with provenance](../../tools/socdaml/spatz_grid_v2/provenance/native-setup/README.md).
The repository CLI does not import them. Its `build` compiles applications only;
its `check` never patches, builds or invokes GVSoC.

## Compatibility sequence and model configuration

Base revisions alone are insufficient. Existing [compatibility snapshots](reproduction/spatz_grid_v2/compatibility/README.md),
configured-source manifest, installed-artifact manifest and dependency hashes
pin the patched environment. There are two descriptions of the same history;
**do not apply both descriptions cumulatively**:

1. **Historical setup route:** `prepare.py` applies the tracked SoftHier core/
   pulp patches, then native library/link fixes; dependency stages patch SystemC
   before building it, patch DRAMSys before building it against that SystemC,
   and apply native simulator compatibility before building the scalar model.
   `toolchain_math.py` repairs pinned GCC prerequisite aliases before its build.
2. **Aggregate source snapshots:** starting from clean pinned revisions,
   `native-core.diff`, `native-pulp.diff`, `native-softhier.diff`,
   `native-systemc.diff` and `native-dramsys.diff` represent their corresponding
   tracked modifications. Core additionally needs
   `native-core-added-header.patch`: its untracked `redmule.hpp` addition was
   omitted by the original tracked-file diff, although it also exists in the
   original SoftHier patch. Apply the aggregate core diff, then this addition;
   do not also apply the overlapping original SoftHier core patch.
3. Populate model inputs from tracked superproject sources: `Makefile`'s config
   target copies `soft_hier/flex_cluster` into pulp; DRAMSys configs come from
   `soft_hier/dramsys_configs`. These generated/copied trees need not be vendored
   from dirty submodules.
4. For the isolated Spatz profile, the retained
   `validated/presentation_spatz.py::architecture` transforms the exact base
   architecture; configure that copy, then apply
   `spatz_macos_compat.patch` (70 unsigned host-container type declarations),
   followed by `spatz_cycle_compat.patch` (cycle CSR callback). These helpers
   check exact before/after hashes. The profile builder in that historical module
   is provenance, not a new supported provisioning command.
5. Build/install the isolated model using the recorded model flags and native
   library paths. This historical stage has **not** been repeated on a fresh
   machine. Never patch the accepted environment to force a guard to pass.

[Patch-order verification](reproduction/spatz_grid_v2/compatibility/patch-order-verification.json)
reconstructed only 19 affected core target files in disposable storage, applied
the aggregate diff/addition and the two pure byte transforms, and verified their
bytes equal the pinned profile. This is an offline source check, not a model
build or complete provisioning test. Other component diffs retain their original
provenance; their cross-machine build/application has not been reverified here.

[Model configuration](../../tools/socdaml/spatz_grid_v2/reference/model-configuration/provenance.json)
now includes exact base and Spatz architecture files plus generated model
parameters. The profile uses a 4×4 grid, three cores per cluster, Spatz on core 0,
64-bit TCDM banks/ports and four west/four north HBM channels. Target matrices keep
west node 0 placement. The runtime config fingerprint (including 1-GHz clock),
compiler flags/hashes, source/dependency manifests and installed hashes remain
in `reference/`; complete runtime JSON is retained in each evidence archive.

## Raw evidence and distribution gap

Raw captures are preserved under the owner's native `logs/`, with adjacent
`.tar.gz` archives; **they are not hosted by this repository or a remote release**.
Recipients need a separately supplied copy. Identifiers/report/archive hashes:

- Original pair: [accepted-results.json](reproduction/spatz_grid_v2/accepted-results.json).
- Additional packaged pair: [packaged-workflow-verification.json](reproduction/spatz_grid_v2/packaged-workflow-verification.json).
- Controlled pair and build/review archive: [controlled-parallel-results.json](reproduction/spatz_grid_v2/controlled-parallel-results.json).

Strict offline validation needs the entire evidence directory, not only the
results JSON: `report.json` and every artifact in its hash map, raw `trace.log`,
variant ELF, target sources, `runtime-config.json`, and for `direct_parallel`
its captured `build.json`. Do not rename members or edit original reports.
Preserve adjacent archives for result generation, which records their hashes.
No installed native model is needed for `validate`; supply the original native
root as `--native-root` when checking original runtime-config path normalization,
even if that directory is absent. The evidence directory itself can be elsewhere.
Changing runtime-config strings to relocate a capture would invalidate its hashes.

Original two-observation and controlled single-observation limits, unresolved
15-cycle input difference, unexplained intermittent timeout and historical
reconstruction limits remain in their respective results reports. The principal
remaining gaps are independent fresh-machine provisioning, binary relocation,
a complete portable host lockfile, the exact historical guarded source inventory,
and a recipient-accessible raw-evidence archive.


### Exact source-inventory gap

The [source coverage audit](../../tools/socdaml/spatz_grid_v2/reference/source-coverage.json)
finds **19 legacy application sources** present in the accepted configured profile
but absent from the committed baseline, plus two generated SDK architecture
headers. The application files are not compiled into V2, yet the strict whole-tree
preservation guard requires them with their exact recorded hashes. They remain
preserved in the native snapshot/profile; they were not imported as unrelated
project work. The headers are generated configuration products.

Consequently the historical setup snapshots and this Git tree alone do **not**
establish that a newly built source tree will satisfy the current full-inventory
guard. Existing-profile execution remains verified. A future fresh-machine effort
must separately provide/reconstruct the historical guarded source inventory or
review a narrower guard without silently weakening accepted preservation checks.
No such guard change or new provisioning was attempted here.
