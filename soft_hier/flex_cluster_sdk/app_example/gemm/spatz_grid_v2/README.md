# Validated K=64 FP32 Spatz grid reconstruction

These four files are exact accepted source copies: `grid.c`, `word_export.h`,
`gemm_fp32.S` and `grid.ld`. Compile `grid.c` with `GRID_BROADCAST=0` for direct
or `1` for broadcast. Do not alter their algorithms, synchronization or probe
placement without treating the result as a new experiment.

This preserves the surviving benchmark-4/5 schedules with a reconstructed shared
Spatz kernel: A[256][64] × B[64][256], one output tile per cluster. The original
five K=256 scalar applications and recovered originals remain separate and
unchanged. Source headers preserve attribution and the historical limitations.

Use [the repository workflow](../../../../../tools/socdaml/spatz_grid_v2/README.md)
for compilation with the existing native profile, bounded runs and strict full-word
validation. Do not run the legacy application makefile expecting it to supply
this kernel's fixed-address link rule and protocol checker automatically.

[Accepted results and measurement boundaries](../../../../../docs/project/SPATZ_GRID_V2_RESULTS.md)
record one accepted run per variant and the unresolved intermittent timeout.
