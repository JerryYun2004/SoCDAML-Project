# Controlled direct-input / parallel-output experiment

`grid_direct_parallel.c` is a new experimental derivative of the accepted V2
reconstruction. It retains benchmark-4-derived direct input and enables the
benchmark-5-derived parallel output branch. It is **not recovered historical
benchmark 4**. Original `../grid.c`, original benchmarks 4/5, and checkpoint
`082c5e8` remain preserved.

Only a three-line attribution/policy definition and the output selection at t5
change. The original `output()` function, DMA arguments, completion wait,
t6/global completion barrier/t7, validation-after-t7, direct input schedule,
input values, FP32 workload and shared GEMM remain unchanged. Build with
`GRID_BROADCAST=0`; do not use this driver to substitute for original direct results.

The CLI name is `direct_parallel`. `common.target_sources()` enforces the exact
relationship to the immutable accepted source before compilation and validation.
The original `direct` name still selects the serialized-output accepted source;
`broadcast` still selects the accepted parallel-output source. See the repository
[workflow](../../../../../../tools/socdaml/spatz_grid_v2/README.md).
