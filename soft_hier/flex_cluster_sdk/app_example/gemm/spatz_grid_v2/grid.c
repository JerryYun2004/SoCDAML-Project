/* K=64 Spatz derivatives of Jerry Yun's benchmark_4.c / benchmark_5.c.
 * The DMA order, row/column masks and serial/direct vs parallel/broadcast
 * output schedules follow the cleaned originals. The kernel is the same
 * separately compiled FP32 assembly already validated on the native Mac.
 *
 * A[256][64] * B[64][256] -> C[256][256]; each cluster owns C[64][64].
 * All HBM addresses retain the originals' hbm_addr() mapping (west node 0).
 * The original global input join is retained: this is source-scheduled
 * reconstruction, not the slide's early per-cluster compute schedule.
 *
 * Fixed, disjoint L1 buffers replace allocation outside the application
 * interval. No checksum, reference calculation, print or validation barrier
 * occurs inside that interval. All evidence is exported after final sync.
 * Existing ETH Zurich/PULP SDK authorship and licenses still apply.
 */
#include <stdint.h>
#include "flex_runtime.h"
#include "flex_printf.h"
#include "flex_dma_pattern.h"
#include "word_export.h"

#ifndef GRID_BROADCAST
#error Define GRID_BROADCAST as 0 (benchmark 4) or 1 (benchmark 5).
#endif
#define TAG "[SOCDAML_SPATZ_GRID_V2]"
#define A_OFF 0x10000u
#define B_OFF 0x14000u
#define C_OFF 0x18000u
#define SCRATCH_OFF 0x20000u
#define READBACK_OFF 0x24000u
#define TILE_BYTES 16384u
#define HBM_A 0x400u
#define HBM_B (HBM_A + 256u*64u*4u)
#define HBM_C (HBM_B + 64u*256u*4u)
extern int socdaml_spatz_gemm(const float *, const float *, float *);

static inline uint32_t cycle32(void)
{
#ifdef SOCDAML_HOST_TEST
    extern uint32_t fixture_cycle(void);
    return fixture_cycle();
#else
    uint32_t value;
    asm volatile("rdcycle %0" : "=r"(value) :: "memory");
    return value;
#endif
}
static uint32_t integer_bits(uint32_t n)
{
    uint32_t top = 31u - (uint32_t)__builtin_clz(n);
    return ((127u+top)<<23) | ((n<<(23u-top)) & 0x7fffffu);
}
static uint32_t a_value(uint32_t r, uint32_t k)
{
    return 1u+(r*3u+k*5u+(r^k)+(r>>6)*3u)%16u;
}
static uint32_t b_value(uint32_t k, uint32_t c)
{
    return 1u+(k*7u+c*11u+(k^c)+(c>>6)*5u)%16u;
}
static uint32_t c_hbm(uint32_t y, uint32_t x)
{
    return HBM_C+(y*64u*256u+x*64u)*4u;
}

/* DMA timing brackets include software issue + completion wait. They do not
 * claim to isolate bus flight time. Records are core-private stack variables.
 * A zero pair denotes an action not performed by this worker. */
typedef struct { uint32_t begin, end; } Span;
static void input_a(uint32_t y, Span *s)
{
    uint32_t off = HBM_A+y*TILE_BYTES;
    s->begin = cycle32();
    bare_dma_start_1d(local(A_OFF), hbm_addr(off), TILE_BYTES);
    bare_dma_wait_all();
    s->end = cycle32();
}
static void input_b(uint32_t x, Span *s)
{
    uint32_t off = HBM_B+x*256u;
    s->begin = cycle32();
    /* Gather 64 rows of 64 FP32 columns from a 256-column HBM matrix. */
    bare_dma_start_2d(local(B_OFF), hbm_addr(off), 256u, 256u, 1024u, 64u);
    bare_dma_wait_all();
    s->end = cycle32();
}
static void broadcast(uint32_t off, uint16_t rm, uint16_t cm, Span *s)
{
    s->begin = cycle32();
    flex_dma_async_broadcast(off, off, TILE_BYTES, rm, cm);
    flex_dma_async_wait_all();
    s->end = cycle32();
}
static void output(uint32_t y, uint32_t x, Span *s)
{
    uint32_t off = c_hbm(y,x);
    s->begin = cycle32();
    bare_dma_start_2d(hbm_addr(off), local(C_OFF), 256u, 1024u, 256u, 64u);
    bare_dma_wait_all();
    s->end = cycle32();
}

static void initialize(void)
{
    if (!flex_is_dm_core()) return;
    volatile uint32_t *a=(volatile uint32_t *)(uintptr_t)local(A_OFF);
    volatile uint32_t *b=(volatile uint32_t *)(uintptr_t)local(B_OFF);
    volatile uint32_t *c=(volatile uint32_t *)(uintptr_t)local(C_OFF);
    for (uint32_t i=0;i<4096u;++i) a[i]=b[i]=c[i]=0x7fc00000u;
    if (flex_get_cluster_id()!=0u) return;
    volatile uint32_t *row=(volatile uint32_t *)(uintptr_t)local(SCRATCH_OFF);
    for (uint32_t r=0;r<256u;++r) {
        for (uint32_t k=0;k<64u;++k) row[k]=integer_bits(a_value(r,k));
        bare_dma_start_1d(hbm_addr(HBM_A+r*256u),local(SCRATCH_OFF),256u);
        bare_dma_wait_all();
    }
    for (uint32_t k=0;k<64u;++k) {
        for (uint32_t c=0;c<256u;++c) row[c]=integer_bits(b_value(k,c));
        bare_dma_start_1d(hbm_addr(HBM_B+k*1024u),local(SCRATCH_OFF),1024u);
        bare_dma_wait_all();
    }
    /* Poison HBM C too: missing output writes cannot pass by coincidence. */
    for (uint32_t c=0;c<256u;++c) row[c]=0x7fc00000u;
    for (uint32_t r=0;r<256u;++r) {
        bare_dma_start_1d(hbm_addr(HBM_C+r*1024u),local(SCRATCH_OFF),1024u);
        bare_dma_wait_all();
    }
}
static void print_span(uint32_t cid, const char *name, Span s)
{
    if (s.begin || s.end) printf(TAG " DMA cid=%u kind=%s begin=%u end=%u\n",cid,name,s.begin,s.end);
}
static void print_words(uint32_t cid, const char *name, uint32_t off)
{
    volatile uint32_t *p=(volatile uint32_t *)(uintptr_t)local(off);
    /* Only called below INTERVAL_END. Preserve all 4,096 raw words per tile,
     * but format metadata once per block and bypass printf for the payload. */
    for (uint32_t i=0;i<4096u;i+=SOCDAML_EXPORT_BLOCK_WORDS) {
        uint32_t count=4096u-i;
        if (count>SOCDAML_EXPORT_BLOCK_WORDS) count=SOCDAML_EXPORT_BLOCK_WORDS;
        printf(TAG " WORDS64 cid=%u kind=%s index=%u count=%u ",cid,name,i,count);
        export_word_payload(p+i,count);
        _putchar('\n');
    }
}

int main(void)
{
    uint32_t cid=flex_get_cluster_id(), core=flex_get_core_id();
    uint32_t dm=flex_is_dm_core();
    FlexPosition pos=get_pos(cid);
    Span a={0}, b={0}, ba={0}, bb={0}, out={0};
    uint32_t compute_begin=0, compute_end=0, status=0, vl=0, vtype=0;
    uint32_t t[8];
    flex_barrier_xy_init();
    flex_global_barrier_xy();
    initialize();
    if (core==0u) {
#ifdef SOCDAML_HOST_TEST
        vl=64; vtype=0xd2;
#else
        asm volatile("vsetvli %0, %1, e32, m4, ta, ma" : "=r"(vl) : "r"(64) : "memory");
        asm volatile("csrr %0, vtype" : "=r"(vtype));
#endif
    }
    flex_global_barrier_xy();
    if (cid==0u && dm) printf(TAG " BEGIN broadcast=%u M=256 K=64 N=256\n",GRID_BROADCAST);
    flex_global_barrier_xy();
    /* Contiguous boundaries: application = t[7]-t[0], with no untimed gaps.
     * t0..t1: HBM input schedule. t1..t2: leader/input completion join.
     * t2..t3: broadcast schedule (empty for direct).
     * t3..t4: broadcast completion join (empty for direct).
     * t4..t5: compute + required global completion join.
     * t5..t6: output schedule. t6..t7: final output completion join.
     * Interspersed original scheduling barriers stay in their schedule phase.
     */
    t[0]=cycle32();
    if (!GRID_BROADCAST) {
        for (uint32_t y=0;y<4u;++y) for (uint32_t x=0;x<4u;++x) {
            flex_global_barrier_xy();
            if (dm && pos.y==y && pos.x==x) { input_a(y,&a); input_b(x,&b); }
            flex_global_barrier_xy();
        }
    } else {
        for (uint32_t y=0;y<4u;++y) {
            flex_global_barrier_xy();
            if (dm && pos.x==0u && pos.y==y) input_a(y,&a);
        }
        flex_global_barrier_xy();
        for (uint32_t x=0;x<4u;++x) {
            flex_global_barrier_xy();
            if (dm && pos.y==0u && pos.x==x) input_b(x,&b);
        }
    }
    t[1]=cycle32();
    flex_global_barrier_xy();
    t[2]=cycle32();
    if (GRID_BROADCAST) {
        for (uint32_t y=0;y<4u;++y) {
            flex_global_barrier_xy();
            if (dm && pos.x==0u && pos.y==y) broadcast(A_OFF,0u,15u,&ba);
        }
        flex_global_barrier_xy();
        for (uint32_t x=0;x<4u;++x) {
            flex_global_barrier_xy();
            if (dm && pos.y==0u && pos.x==x) broadcast(B_OFF,15u,0u,&bb);
        }
        t[3]=cycle32();
        flex_global_barrier_xy();
        t[4]=cycle32();
    } else { t[3]=t[2]; t[4]=t[2]; }
    /* The input join already synchronizes all three local cores. Only core 0
     * calls Spatz; DM core 2 remains responsible for DMA and global barriers. */
    if (core==0u) {
        compute_begin=cycle32();
        status=(uint32_t)socdaml_spatz_gemm((const float *)(uintptr_t)local(A_OFF),
            (const float *)(uintptr_t)local(B_OFF),(float *)(uintptr_t)local(C_OFF));
        compute_end=cycle32();
    }
    flex_global_barrier_xy();
    t[5]=cycle32();
    if (!GRID_BROADCAST) {
        for (uint32_t y=0;y<4u;++y) for (uint32_t x=0;x<4u;++x) {
            flex_global_barrier_xy();
            if (dm && pos.y==y && pos.x==x) output(y,x,&out);
            flex_global_barrier_xy();
        }
    } else if (dm) output(pos.y,pos.x,&out);
    t[6]=cycle32();
    flex_global_barrier_xy();
    t[7]=cycle32();
#ifdef SOCDAML_HOST_TEST
    extern void fixture_interval_done(void);
    fixture_interval_done();
#endif
    /* Everything below is untimed testbench work. */
    flex_global_barrier_xy();
    if (cid==0u && dm) printf(TAG " INTERVAL_END\n");
    for (uint32_t worker=0;worker<16u;++worker) {
        flex_global_barrier_xy();
        if (cid==worker && core==0u)
            printf(TAG " COMPUTE cid=%u begin=%u end=%u joined=%u status=%u vl=%u vtype=%08x\n",
                cid,compute_begin,compute_end,t[5],status,vl,vtype);
        flex_global_barrier_xy();
        if (cid==worker && dm) {
            printf(TAG " TIMES cid=%u %u %u %u %u %u %u %u %u\n",cid,t[0],t[1],t[2],t[3],t[4],t[5],t[6],t[7]);
            print_span(cid,"A",a); print_span(cid,"B",b);
            print_span(cid,"BA",ba); print_span(cid,"BB",bb); print_span(cid,"OUT",out);
            print_words(cid,"A",A_OFF); print_words(cid,"B",B_OFF); print_words(cid,"C",C_OFF);
            bare_dma_start_2d(local(READBACK_OFF),hbm_addr(c_hbm(pos.y,pos.x)),256u,256u,1024u,64u);
            bare_dma_wait_all();
            print_words(cid,"HBM_C",READBACK_OFF);
        }
    }
    flex_global_barrier_xy();
    if (cid==0u && dm) { printf(TAG " END\n"); flex_eoc(0); }
    return 0;
}
