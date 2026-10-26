/* Presentation reconstruction, not a recovered historical kernel.
 * FP32 A[64][64] * B[64][64] -> C[64][64], row-major, C overwritten.
 * Standard RVV instructions; no low-precision conversion, SSR or FREP.
 * Four output rows reuse each B vector. Column strips use the returned VL.
 * All integer and FP temporaries are caller-saved under ilp32d. Vector
 * registers are caller-saved under the ordinary (non-vector) calling ABI.
 */
.section .text.socdaml_spatz_gemm,"ax",@progbits
.balign 64
.global socdaml_spatz_gemm
.type socdaml_spatz_gemm,@function
socdaml_spatz_gemm:
    li a3, 16                  /* sixteen groups of four output rows */
.Lrows:
    li a4, 64                  /* remaining columns */
    li a5, 0                   /* column offset in bytes */
.Lcolumns:
    vsetvli t0, a4, e32, m4, ta, ma
    beqz t0, .Lunsupported     /* fail visibly instead of an infinite loop */
    slli t4, t0, 2
    add t1, a1, a5             /* B[0][column] */
    mv t2, a0                  /* A[first_row][0] */
    flw fa0, 0(t2)
    flw fa1, 256(t2)
    flw fa2, 512(t2)
    flw fa3, 768(t2)
    vle32.v v0, (t1)
    vfmul.vf v8, v0, fa0       /* first product initializes each accumulator */
    vfmul.vf v12, v0, fa1
    vfmul.vf v16, v0, fa2
    vfmul.vf v20, v0, fa3
    li t3, 63
.Linner:
    addi t1, t1, 256           /* next B row; fixed K=N=64 */
    addi t2, t2, 4             /* next A column */
    flw fa0, 0(t2)
    flw fa1, 256(t2)
    flw fa2, 512(t2)
    flw fa3, 768(t2)
    vle32.v v0, (t1)
    vfmacc.vf v8, fa0, v0
    vfmacc.vf v12, fa1, v0
    vfmacc.vf v16, fa2, v0
    vfmacc.vf v20, fa3, v0
    addi t3, t3, -1
    bnez t3, .Linner
    add t5, a2, a5
    vse32.v v8, (t5)
    addi t5, t5, 256
    vse32.v v12, (t5)
    addi t5, t5, 256
    vse32.v v16, (t5)
    addi t5, t5, 256
    vse32.v v20, (t5)
    sub a4, a4, t0
    add a5, a5, t4
    bnez a4, .Lcolumns
    addi a0, a0, 1024
    addi a2, a2, 1024
    addi a3, a3, -1
    bnez a3, .Lrows
    /* Architecturally required memory ordering. The pinned simulator's
     * fence handler is a no-op: the host checker ALSO checks the last vector
     * completion timestamp against the end cycle before accepting timing.
     * Do not add an arbitrary delay to make that check pass. */
    fence rw, rw
    li a0, 0
    ret
.Lunsupported:
    li a0, 1
    ret
.size socdaml_spatz_gemm, .-socdaml_spatz_gemm
