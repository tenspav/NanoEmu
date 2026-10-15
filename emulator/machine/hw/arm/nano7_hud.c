/*
 * nano7_hud.c -- in-emulator usage HUD for the iPod nano 7G machine: a strip
 * under the display (nano7_ui.c) with live CPU and GPU load graphs.
 *
 * F10 toggles it (run.sh --hud starts with it shown). While shown:
 *   CPU busy % - a 1 ms virtual-clock sampler of whether the guest CPU is
 *     halted: the RetailOS idle thread parks in WFI (0x0862adec), so halted
 *     = idle, anything else = work.
 *   GPU busy % - an estimate from the MBX renderer's workload (nano7_mbxr.c):
 *     rasterized fragments (one per clock, PowerVR ISP / TSP) plus executed
 *     vertex-program instructions (one per clock, VGP) per guest second, over
 *     the GPU clock the guest programmed (nano7_power.c; stock 133 MHz).
 *     It is a model of the hardware's load, not the host's rendering time.
 *   FPS - frames per guest second: the larger of GL render jobs and panel
 *     updates (nano7_hud_lcd_frame(), a TE after a GRAM change), so 2D apps
 *     show a rate too.
 *   RAM - the OS main heap as the OS itself reports it (Session 59): the
 *     heap object *0x08909134; total = heap[+0xa8], used = heap[+0xc0]
 *     minus the unused part of the 6 sub-pool caches when heap[+0x1684] is
 *     set (pool p = heap[+0x1668 + 4i]: p[+8] * (p[+12] - p[+0x1c])) - the
 *     arithmetic of the OS free-size query 0x08417824 -> 0x0800e2f4 ->
 *     0x0841402a (hb_os_heap_free() in the homebrew SDK). Read-only.
 *   Second line (Session 60): CPU / GPU clocks as programmed in CLKCON, the
 *     clock profile (GUEST = the guest's own clocks; F11 cycles STOCK / SAFE
 *     / MAX) and the PMU die temperature (thermal model, nano7_power.c).
 * Windows of 250 samples (250 ms of guest time at 1x), 120 kept (~30 s).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "nano7_hud.h"
#include "nano7_ui.h"
#include "nano7_mbx.h"
#include "nano7_mbxr.h"
#include "nano7_clock.h"
#include "nano7_power.h"
#include "nano7_regfile.h"
#include "nano7_btuart.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"

#define HUD_HIST        120
#define HUD_SAMPLE_NS   1000000         /* 1 ms of QEMU_CLOCK_VIRTUAL */
#define HUD_WINDOW      250             /* samples per data point */

#define HUD_BG          0x10141au
#define HUD_GRID        0x2a3038u
#define HUD_TEXT        0xd8dce0u
#define HUD_CPU         0x4cd964u
#define HUD_GPU         0xffa030u
#define HUD_RAM         0x5aa0ffu
#define HUD_HEAP_CELL   0x08909134u     /* -> the OS main heap object */

typedef struct Hud {
    bool on;
    QEMUTimer *timer;
    unsigned samples, halted;
    int64_t t0;                 /* guest ns at the window start */
    MbxrStats st0;
    float cpu[HUD_HIST], gpu[HUD_HIST], ram[HUD_HIST];
    uint32_t heap_used, heap_total;     /* last reading, 0 = unknown */
    unsigned head, count;
    float fps;
    unsigned lcd_frames, lcd_frames0;   /* panel updates, window start */
} Hud;

static Hud hud;

/* 3x5 glyphs, rows top to bottom, 3 bits each (bit 2 = left column) */
static const struct {
    char c;
    uint8_t row[5];
} hud_font[] = {
    { '0', { 7, 5, 5, 5, 7 } }, { '1', { 2, 6, 2, 2, 7 } },
    { '2', { 7, 1, 7, 4, 7 } }, { '3', { 7, 1, 7, 1, 7 } },
    { '4', { 5, 5, 7, 1, 1 } }, { '5', { 7, 4, 7, 1, 7 } },
    { '6', { 7, 4, 7, 5, 7 } }, { '7', { 7, 1, 1, 1, 1 } },
    { '8', { 7, 5, 7, 5, 7 } }, { '9', { 7, 5, 7, 1, 7 } },
    { '%', { 5, 1, 2, 4, 5 } }, { 'C', { 7, 4, 4, 4, 7 } },
    { 'P', { 7, 5, 7, 4, 4 } }, { 'U', { 5, 5, 5, 5, 7 } },
    { 'G', { 7, 4, 5, 5, 7 } }, { 'R', { 7, 5, 6, 5, 5 } },
    { 'A', { 2, 5, 7, 5, 5 } }, { 'M', { 5, 7, 7, 5, 5 } },
    { 'F', { 7, 4, 6, 4, 4 } }, { 'S', { 7, 4, 7, 1, 7 } },
    { 'N', { 5, 7, 7, 7, 5 } }, { '/', { 1, 1, 2, 4, 4 } },
    { '.', { 0, 0, 0, 0, 2 } }, { '-', { 0, 0, 7, 0, 0 } },
    { 'T', { 7, 2, 2, 2, 2 } }, { 'O', { 7, 5, 5, 5, 7 } },
    { 'K', { 5, 5, 6, 5, 5 } }, { 'E', { 7, 4, 6, 4, 7 } },
    { 'X', { 5, 5, 2, 5, 5 } }, { 'H', { 5, 5, 7, 5, 5 } },
    { 'Z', { 7, 1, 2, 4, 7 } }, { 'L', { 4, 4, 4, 4, 7 } },
    { 'D', { 6, 5, 5, 5, 6 } },
};

static const uint8_t *hud_glyph(char c)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(hud_font); i++) {
        if (hud_font[i].c == c) {
            return hud_font[i].row;
        }
    }
    return NULL;                /* space and unknown characters */
}

static void hud_text(uint32_t *buf, int w, int x, int y, const char *s,
                     uint32_t col)
{
    for (; *s; s++, x += 4) {
        const uint8_t *g = hud_glyph(*s);
        int gx, gy;

        for (gy = 0; g && gy < 5; gy++) {
            for (gx = 0; gx < 3; gx++) {
                if ((g[gy] >> (2 - gx)) & 1 && x + gx < w) {
                    buf[(y + gy) * w + x + gx] = col;
                }
            }
        }
    }
}

static uint32_t hud_ldl(uint32_t a)
{
    return address_space_ldl_le(&address_space_memory, a,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

static bool hud_ram_ok(uint32_t a)
{
    return a >= 0x08000000u && a < 0x0c000000u && !(a & 3);
}

/* the OS heap's used / total bytes (see the header); false if unreadable */
static bool hud_heap(uint32_t *used, uint32_t *total)
{
    uint32_t heap = hud_ldl(HUD_HEAP_CELL), u, t, i;

    if (!hud_ram_ok(heap)) {
        return false;
    }
    t = hud_ldl(heap + 0xa8);
    u = hud_ldl(heap + 0xc0);
    if (!t || u > t) {
        return false;
    }
    if (address_space_ldub(&address_space_memory, heap + 0x1684,
                           MEMTXATTRS_UNSPECIFIED, NULL)) {
        for (i = 0; i < 6; i++) {
            uint32_t pool = hud_ldl(heap + 0x1668 + 4 * i), a, b, c;

            if (!hud_ram_ok(pool)) {
                continue;
            }
            a = hud_ldl(pool + 8);
            b = hud_ldl(pool + 12);
            c = hud_ldl(pool + 0x1c);
            if (b) {
                uint32_t unused = (uint32_t)((uint64_t)a * b / b) * (b - c);

                u = unused < u ? u - unused : 0;
            }
        }
    }
    *used = u;
    *total = t;
    return true;
}

/* one data point per HUD_WINDOW samples */
static void hud_window(void)
{
    int64_t now = nano7_now_ns(), dt = now - hud.t0;
    MbxrStats st;
    double gpu = 0;

    nano7_mbx_stats(&st);
    if (dt > 0) {
        double cycles = (double)(st.frags - hud.st0.frags) +
                        (double)(st.vgp_ins - hud.st0.vgp_ins);

        gpu = cycles / (nano7_power_gpu_mhz() * 1e6 * dt / 1e9);
        hud.fps = MAX((st.frames - hud.st0.frames),
                      hud.lcd_frames - hud.lcd_frames0) / (dt / 1e9);
    }
    hud.lcd_frames0 = hud.lcd_frames;
    hud.cpu[hud.head] = hud.samples ?
        1.f - (float)hud.halted / hud.samples : 0;
    hud.gpu[hud.head] = MIN(gpu, 1.0);
    if (!hud_heap(&hud.heap_used, &hud.heap_total)) {
        hud.heap_used = hud.heap_total = 0;
    }
    hud.ram[hud.head] = hud.heap_total ?
        (float)hud.heap_used / hud.heap_total : 0;
    if (getenv("NANO7_HUD_LOG")) {
        uint32_t c0 = 0, c1c = 0;
        unsigned m0 = nano7_regfile_peek("clkcon", 0, &c0);
        unsigned m1 = nano7_regfile_peek("clkcon", 0x1c, &c1c);

        fprintf(stderr, "[nano7-hud] clkcon %08x/%x +1c %08x/%x\n",
                c0, m0, c1c, m1);
        fprintf(stderr, "[nano7-hud] cpu %.3f gpu %.3f fps %.1f heap used %u "
                "total %u free %u clk %.0f/%.0f %s %.1fC\n", hud.cpu[hud.head],
                hud.gpu[hud.head], hud.fps, hud.heap_used, hud.heap_total,
                hud.heap_total - hud.heap_used, nano7_power_cpu_mhz(),
                nano7_power_gpu_mhz(),
                nano7_power_profile_name(nano7_power_profile()),
                nano7_power_temp());
    }
    hud.head = (hud.head + 1) % HUD_HIST;
    hud.count = MIN(hud.count + 1, HUD_HIST);
    hud.samples = hud.halted = 0;
    hud.t0 = now;
    hud.st0 = st;
    nano7_ui_redraw();
}

static void hud_tick(void *opaque)
{
    hud.samples++;
    if (first_cpu && first_cpu->halted) {
        hud.halted++;
    }
    if (hud.samples >= HUD_WINDOW) {
        hud_window();
    }
    timer_mod_ns(hud.timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + HUD_SAMPLE_NS);
}

bool nano7_hud_on(void)
{
    return hud.on;
}

void nano7_hud_set(bool on)
{
    if (!hud.timer) {
        hud.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, hud_tick, NULL);
    }
    hud.on = on;
    if (on) {
        hud.samples = hud.halted = 0;
        hud.head = hud.count = 0;
        hud.fps = 0;
        hud.lcd_frames0 = hud.lcd_frames;
        hud.t0 = nano7_now_ns();
        nano7_mbx_stats(&hud.st0);
        timer_mod_ns(hud.timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + HUD_SAMPLE_NS);
    } else {
        timer_del(hud.timer);
    }
}

/* a polyline of the history (oldest left, newest right) */
static void hud_graph(uint32_t *buf, int w, int y0, int gh, const float *v,
                      uint32_t col)
{
    int i, prev = -1;

    for (i = 0; i < (int)hud.count; i++) {
        unsigned idx = (hud.head + HUD_HIST - hud.count + i) % HUD_HIST;
        int x0 = (HUD_HIST - hud.count + i) * (w - 1) / (HUD_HIST - 1);
        int x1 = (HUD_HIST - hud.count + i + 1) * (w - 1) / (HUD_HIST - 1);
        int y = y0 + gh - 1 - (int)(v[idx] * (gh - 1) + .5f), x, yy;

        for (x = x0; x <= x1 && x < w; x++) {
            buf[y * w + x] = col;
        }
        if (prev >= 0) {        /* vertical joins */
            for (yy = MIN(prev, y); yy <= MAX(prev, y); yy++) {
                buf[yy * w + x0] = col;
            }
        }
        prev = y;
    }
}

void nano7_hud_draw(uint32_t *buf, int w)
{
    const int gy = 16, gh = NANO7_HUD_ROWS - gy - 2;
    unsigned last = (hud.head + HUD_HIST - 1) % HUD_HIST;
    char line[64];
    int i, x;

    for (i = 0; i < w * NANO7_HUD_ROWS; i++) {
        buf[i] = HUD_BG;
    }
    for (x = 0; x < w; x++) {   /* 0 %, 50 %, 100 % lines */
        buf[gy * w + x] = HUD_GRID;
        buf[(gy + gh / 2) * w + x] = HUD_GRID;
        buf[(gy + gh - 1) * w + x] = HUD_GRID;
    }
    if (hud.count) {
        hud_graph(buf, w, gy, gh, hud.ram, HUD_RAM);
        hud_graph(buf, w, gy, gh, hud.gpu, HUD_GPU);
        hud_graph(buf, w, gy, gh, hud.cpu, HUD_CPU);
    }
    snprintf(line, sizeof(line), "CPU %3d%%", hud.count ?
             (int)(hud.cpu[last] * 100 + .5f) : 0);
    hud_text(buf, w, 2, 2, line, HUD_CPU);
    snprintf(line, sizeof(line), "GPU %3d%% %3dFPS", hud.count ?
             (int)(hud.gpu[last] * 100 + .5f) : 0, (int)(hud.fps + .5f));
    hud_text(buf, w, 40, 2, line, HUD_GPU);
    if (hud.heap_total) {
        snprintf(line, sizeof(line), "RAM %.1f/%.1fM",
                 hud.heap_used / 1048576.0, hud.heap_total / 1048576.0);
        hud_text(buf, w, 108, 2, line, HUD_RAM);
    } else {
        hud_text(buf, w, 108, 2, "RAM N/A", HUD_TEXT);
    }
    snprintf(line, sizeof(line), "CLK %.0f/%.0fMHZ %s %.0fC",
             nano7_power_cpu_mhz(), nano7_power_gpu_mhz(),
             nano7_power_profile_name(nano7_power_profile()),
             nano7_power_temp());
    hud_text(buf, w, 2, 9, line, HUD_TEXT);
    if (nano7_ui_state(NANO7_UI_ROTATE)) {      /* screen rotation */
        int x0 = 2 + 4 * (int)strlen(line) + 4;

        snprintf(line, sizeof(line), "ROT %d", nano7_ui_state(NANO7_UI_ROTATE));
        hud_text(buf, w, x0, 9, line, HUD_TEXT);
    }
    if (nano7_btuart_pad_state()) {     /* F8 pad mode: link state */
        hud_text(buf, w, w - 30, 9, nano7_btuart_pad_state() == 2 ?
                 "PAD OK" : "PAD --", nano7_btuart_pad_state() == 2 ?
                 HUD_CPU : HUD_GPU);
    }
}

void nano7_hud_stats(float *fps, float *cpu, float *gpu, uint32_t *heap_used,
                     uint32_t *heap_total)
{
    unsigned last = (hud.head + HUD_HIST - 1) % HUD_HIST;

    *fps = hud.fps;
    *cpu = hud.count ? hud.cpu[last] : 0;
    *gpu = hud.count ? hud.gpu[last] : 0;
    *heap_used = hud.heap_used;
    *heap_total = hud.heap_total;
}

void nano7_hud_lcd_frame(void)
{
    hud.lcd_frames++;
}

void nano7_hud_init(void)
{
    const char *e = getenv("NANO7_HUD");

    if (e && *e && strcmp(e, "0")) {
        nano7_hud_set(true);
    }
}
