# Contribution boundaries and provenance

## Course-project scope

The project work is concentrated in `app_example/` and the project helpers
`runtime/include/fixed_proj.h` and `runtime/include/soc_daml.h`. The experiments
explore direct versus broadcast data movement and common free-region discovery.
This describes the scope; it is not a claim that every line in these paths
was independently originated. Retain file-level credits for adapted code.

## Inherited components

GVSoC, SoftHier architecture models, existing SDK/runtime components, bundled
examples, SystemC, DRAMSys, and RISC-V toolchains retain their upstream
authorship and licensing. Existing GEMM, broadcast, symmetric allocation,
and synchronization techniques should be acknowledged as established work.
Do not apply a new blanket license to the entire inherited tree.

## Cleanup changes

The cleanup renames application files, adds explicit source selection, and
provides documentation and validation scripts. These are maintenance changes,
not new performance results or new allocator algorithms. Documentation and
scripts were prepared with AI assistance and require normal maintainer review.

## Baseline

- Course repository commit: `707cc6f0db370d16f608de7b9ff86e9c758d95e9`
- `core` submodule: `bda268a4808708a48eab21981c7d6bbced1577d7`
- `pulp` submodule: `3ca2016d056dda72317e27c4319fc6174b5f8c4d`
- Local preservation tag: `pre-cleanup-2026-10-21`

The Linux runner records the actual source snapshot and submodule revisions
used for its run. Build-time patches can make submodule worktrees dirty;
that does not by itself mean their recorded gitlink revisions have changed.
