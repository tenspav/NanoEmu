/* Bare-metal harness: runs the REAL ramscope.c logic in the nano7 emulator,
 * user mode, MMU on (L1 table at 0x08b28000 like the OS), SVC #70 stubbed. */
#include <stdint.h>
#include <stdbool.h>
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_raw_surface.h"

static void sh(int op, const void *p)
{
    register int r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = p;
    __asm__ volatile("svc 0x123456" : "+r"(r0) : "r"(r1) : "memory");
}
static void out(const char *s) { sh(4, s); }
static void dec(uint32_t v) { char b[12]; int n=0; char t[12]; if(!v)t[n++]='0'; while(v){t[n++]='0'+v%10;v/=10;} int i=0; while(n)b[i++]=t[--n]; b[i]=0; out(b); }

/* ---- SDK stubs ---- */
static uint32_t g_clk, g_heap = 0x09000000u;
uint32_t hb_time_uptime_ms(void) { return g_clk += 1; }
uint32_t hb_time_uptime_us(void) { return g_clk += 1; }
bool hb_fs_mkdir(const char *p) { (void)p; return true; }
bool hb_fs_write(const char *path, const void *d, uint32_t n)
{
    static char buf[4096];
    out("--- FILE "); out(path); out(" ---\n");
    if (n >= sizeof buf) n = sizeof buf - 1;
    for (uint32_t i = 0; i < n; i++) buf[i] = ((const char *)d)[i];
    buf[n] = 0;
    out(buf);
    return true;
}
void hb_trace_log(const char *t, uint32_t a, uint32_t b) { (void)t; (void)a; (void)b; }
uint32_t hb_os_heap_free(void) { return 0x1000000; }
uint32_t hb_os_heap_largest(void) { return 0x800000; }
void *hb_os_alloc(uint32_t n) { uint32_t p = g_heap; g_heap += (n + 63) & ~63u; return (void *)p; }
void hb_os_free(void *p) { (void)p; }
void hb_raw_fill(uint32_t c) { (void)c; }
void hb_raw_fill_rect(int x, int y, int w, int h, uint32_t c) { (void)x;(void)y;(void)w;(void)h;(void)c; }
void hb_raw_rect_outline(int x, int y, int w, int h, int t, uint32_t c) { (void)x;(void)y;(void)w;(void)h;(void)t;(void)c; }

#define DMC_BASE 0x09e00000u   /* fake DMC in RAM so the enable logic is testable */
#include "ramscope.c"

/* ---- boot: page tables + MMU + vectors + drop to user mode ---- */
__attribute__((naked, section(".text.start"))) void _start(void)
{
    __asm__ volatile(
        "ldr sp, =0x0a000000\n"
        "bl  setup\n"
        "cps #0x1f\n"
        "ldr sp, =0x09f00000\n"
        "cps #0x13\n"
        "ldr lr, =user_main\n"
        "msr spsr_cxsf, #0x10\n"
        "movs pc, lr\n");
}

__attribute__((naked, used)) void v_svc(void)  { __asm__ volatile("mov r0, #0\n movs pc, lr\n"); }
__attribute__((used)) void abort_report(void) { out("*** CPU EXCEPTION (abort/undef) ***\n"); sh(0x18, (void *)0x20026); for (;;) {} }
__attribute__((naked, used)) void v_abort(void) { __asm__ volatile("ldr sp, =0x0a000000\n b abort_report\n"); }
__attribute__((naked, aligned(32))) static void vectors(void)
{
    __asm__ volatile(
        "b v_abort\n b v_abort\n b v_svc\n b v_abort\n b v_abort\n nop\n b v_abort\n b v_abort\n");
}

void setup(void)
{
    volatile uint32_t *t = (volatile uint32_t *)0x08b28000u;
    for (uint32_t i = 0; i < 4096; i++) t[i] = 0;
    for (uint32_t i = 0x080; i < 0x0c0; i++) t[i] = (i << 20) | 0x85c06u;           /* cached DRAM  */
    for (uint32_t i = 0x080; i < 0x0c0; i++) t[0x800 + i] = (i << 20) | 0x84c12u;   /* uncached     */
    for (uint32_t i = 0x380; i < 0x400; i++) t[i] = (i << 20) | 0x82c12u;           /* peripherals  */
    t[0x220] = (0x220u << 20) | 0x85c06u;                                           /* IRAM         */
    __asm__ volatile("mcr p15,0,%0,c12,c0,0" :: "r"((uint32_t)&vectors));          /* VBAR         */
    __asm__ volatile("mcr p15,0,%0,c2,c0,0" :: "r"(0x08b28000u));                  /* TTBR0        */
    __asm__ volatile("mcr p15,0,%0,c3,c0,0" :: "r"(1u));                           /* DACR client  */
    uint32_t sctlr;
    __asm__ volatile("mrc p15,0,%0,c1,c0,0" : "=r"(sctlr));
    sctlr |= 1u;                                                                     /* M            */
    sctlr &= ~(1u << 13);                                                            /* V=0          */
    __asm__ volatile("mcr p15,0,%0,c1,c0,0\n isb" :: "r"(sctlr));
}

void user_main(void)
{
    hb_spoint_t up = { 0, 0, 0 }, dn = { 50, 150, 1 };
    out("harness: start, cpsr mode user\n");
    *(volatile uint32_t *)0x09e00008u = 0x1u;   /* MEMCONFIG0 as seen on device */
    hb_raw_init(240, 320);
    hb_raw_frame(&up);
    hb_raw_frame(&dn);
    hb_raw_frame(&up);
    for (int i = 0; i < 40 && g_state != S_RESULT && g_state != S_FAIL; i++) hb_raw_frame(&up);
    out("=== state="); dec((uint32_t)g_state);
    out(" verdict="); out(vtxt(g_verdict));
    out(" fail="); out(g_fail); out("\n");
    sh(0x18, (void *)0x20026);
    for (;;) {}
}
