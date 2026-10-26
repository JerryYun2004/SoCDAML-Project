"""Repository paths and guards for the existing native Spatz profile.

The validated modules are copied byte-for-byte and imported as libraries only.
Their historical setup/run entrypoints are not the packaged workflow.
"""
from pathlib import Path
import hashlib
import json
import os
import sys

sys.dont_write_bytecode = True
PACKAGE = Path(__file__).resolve().parent
REPO = PACKAGE.parents[2]
ASSETS = REPO/'soft_hier/flex_cluster_sdk/app_example/gemm/spatz_grid_v2'
REFERENCE = PACKAGE/'reference'
sys.path.insert(0, str(PACKAGE/'validated'))
import presentation_grid as grid
import presentation_spatz as native
import grid_diagnose as diagnostic


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path):
    return json.loads(Path(path).read_text())


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True)+'\n')


def metadata():
    return read(REFERENCE/'metadata.json')


def verify_artifacts(root, manifest):
    root = Path(root).resolve()
    for name, digest in manifest.items():
        path = root/name
        require(not Path(name).is_absolute() and path.resolve().is_relative_to(root),
                'Artifact escapes evidence root: '+name)
        require(native.sha(path) == digest, 'Artifact changed: '+str(path))


def verify_package():
    meta = metadata()
    for name, digest in meta['exact_source_sha256'].items():
        require(native.sha(ASSETS/name) == digest, 'Validated target source changed: '+name)
    for name, digest in meta['validated_module_sha256'].items():
        require(native.sha(PACKAGE/'validated'/name) == digest, 'Validated module changed: '+name)
    for name, digest in meta['original_benchmark_sha256'].items():
        require(native.sha(REPO/'docs/project/reproduction/originals'/name) == digest,
                'Recovered original changed: '+name)
    return meta


def model_guard(root):
    """Read-only: no setup, patching, compilation, or simulator launch."""
    root = Path(root).resolve()
    meta = verify_package()
    profile = root/'profiles'/meta['profile']
    installed = read(REFERENCE/'installed-artifacts.json')
    verify_artifacts(profile, installed)
    expected = read(REFERENCE/'configured-source.json')
    require(native.tree_manifest(profile/'source') == expected, 'Pinned profile source differs.')
    captured_root = Path(meta['native_root_at_capture'])
    for old, digest in read(REFERENCE/'dependencies.json').items():
        relative = Path(old).relative_to(captured_root)
        require(native.sha(root/relative) == digest, 'Native dependency differs: '+str(relative))
    compiler = root/'prefix/riscv/bin/riscv32-unknown-elf-gcc'
    require(native.sha(compiler) == read(REFERENCE/'compiler.json')['compiler_sha256'], 'Compiler differs.')
    for path in [compiler, root/'prefix/riscv/bin/riscv32-unknown-elf-objcopy',
                 root/'prefix/riscv/bin/riscv32-unknown-elf-objdump', profile/'install/bin/gvsoc']:
        require(path.is_file() and os.access(path, os.X_OK), 'Missing executable: '+str(path))
    return profile


def config_fingerprint(config, root):
    from broadcast_checks import canonical_config
    elf = config['target']['chip']['cluster_0']['loader']['binary'][0]
    value = canonical_config(config, elf)
    # Only workspace paths and already-reviewed observation/ELF metadata are
    # normalized. Hardware, ISA, latencies, clocks and topology remain checked.
    def rebase(item):
        if isinstance(item, dict):
            return {k:rebase(v) for k,v in item.items()}
        if isinstance(item, list):
            return [rebase(v) for v in item]
        return item.replace(str(Path(root).resolve()), '<NATIVE_ROOT>') if isinstance(item, str) else item
    return hashlib.sha256(json.dumps(rebase(value), sort_keys=True, separators=(',', ':')).encode()).hexdigest()
