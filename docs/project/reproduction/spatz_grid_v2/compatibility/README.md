# Native patch provenance

These are retained snapshots of the existing native compatibility diffs, not
new patches applied during consolidation. The runner requires the accepted
configured source and installed/dependency hashes; it never applies a patch.

- `native-core.diff`, `native-pulp.diff`: native loader/link/plugin integration,
  memory-memcheck service dispatch and FP receiver/offload build separation.
- `native-dramsys.diff`, `native-systemc.diff`: the existing integration/native
  dependency changes.
- `native-softhier.diff`: the variadic LightRedmule diagnostic repair.
- `macos-compatibility.json`: the isolated profile's 70 unsigned bit-container
  declarations changed to `uint64_t` for Apple arm64 pointer-type compatibility.
- `cycle-compatibility.json`: the cycle CSR callback registration using the
  existing simulated clock rather than inert register storage.

Exact reviewed compatibility helper source is retained under
`tools/socdaml/spatz_grid_v2/validated/` at repository root, alongside the
configured-source, installed-artifact and dependency manifests in `reference/`.
Base Git revisions alone do not describe the accepted patched binaries.


Publication audit found that the aggregate core diff omits an untracked added
ISA header. `native-core-added-header.patch` preserves that exact addition, already
represented in the original SoftHier patch. Use one route only: aggregate core
diff + this addition, then unsigned-container and cycle-CSR helpers; do not also
apply the overlapping original SoftHier core patch. `patch-order-verification.json`
records a disposable-file check against the pinned profile. See the
[sharing guide](../../../SHARING.md) for the complete sequence and limitations.
