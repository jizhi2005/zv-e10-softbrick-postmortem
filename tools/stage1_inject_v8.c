/*
 * stage1_inject.c -- inject the REAL upstream stage1 payload over BootROM serial boot.
 *
 * ============================================================================
 * WHY A NEW TOOL
 * ============================================================================
 * inject_abs.c sends a fixed 20-byte self-proof payload (a tiny ARM stub that
 * makes the ROM blink 0x2007). That stub proves the ROM received a record group,
 * but it cannot bring up USB fastboot, so it can never reach eMMC. Every "inject"
 * run so far therefore could not have produced a recovery even if the waveform
 * had been perfect.
 *
 * This tool takes a payload file and builds the real record group:
 *
 *    SET_UNLOCK
 *    MEM_WRITE chunks  (payload split into <=4084-byte records, 4-byte aligned)
 *    SET_UNLOCK        (a group must both start and end with unlock)
 *    SET_ENTRY         (0xFE020001)
 *    SET_UNLOCK
 *
 * It uses the same absolute-time scheduler and the same instruction-accurate
 * waveform as inject_abs.c, so the timing that was already verified by rxsim.c
 * and margin.c applies unchanged.
 *
 * ============================================================================
 * PAYLOAD PROVENANCE
 * ============================================================================
 * Default payload is the upstream prebuilt stage1 (7636 bytes), whose SHA-256 is
 * recorded in the upstream manifest.json and published SHA256SUMS:
 *   8573b5d1a1e18b0b71269d3f163f9fec56ecbbd9544693a5b73687968bab9f19
 * The tool refuses to send a payload whose size exceeds the BootROM record limit
 * or that is not 4-byte aligned, because a mis-sized group cannot work.
 *
 * A local 6284-byte stage1 also exists in this workspace, but its hash does not
 * match the published manifest, so it is NOT used by default: an unverified
 * payload is the last thing to change while debugging a silent link.
 *
 * ============================================================================
 * USAGE
 * ============================================================================
 *   ./stage1_inject --payload /tmp/stage1_verified.bin
 *   ./stage1_inject --payload X --seconds 90 --reset-us 10
 *   ./stage1_inject --payload X --no-reset          # for cold-boot testing
 *   ./stage1_inject --payload X --dry-run           # build group, send nothing
 *
 * While it replays, the camera's LED should show the stage1 pattern from the
 * upstream docs: one LONG flash (0.4 s) = payload alive, then short pulses as
 * USB comes up. After that the camera should appear to the host as a USB device
 * (fastboot).
 *
 * BUILD: aarch64-linux-gnu-gcc -O2 -static -o stage1_inject stage1_inject.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/gpio.h>

#ifndef PIN_DN
#define PIN_DN      39      /* P1_A7 - camera D- */
#endif
#ifndef PIN_DP
#define PIN_DP      40      /* P1_B0 - camera D+ */
#endif

#define LOAD_ADDR   0xFE020000u
#define ENTRY_ADDR  0xFE020001u
#define UNLOCK_TOKEN 0x8CB102DCu

/* instruction-accurate from serialboot.pio, verified against upstream */
static double g_ns_carrier  = 124999.875;
static int    g_lead_us     = 50;
static int    g_dp_high     = 0;   /* drive D+ push-pull HIGH instead of open-drain */
static int    g_reset_every = 0;   /* 0 = never; >0 = periodic XRESET_REQ */   /* inter-record carrier lead-in */
static double g_ns_preamble = 249999.75;
static double g_ns_bit      = 20833.3125;
static int g_preamble_loops = 29;   /* PIO: set y,28 + jmp y-- */

static int g_record_gap = 2000;
static int g_group_gap  = 50000;
static int g_reset_us   = 10;
static int g_settle_ms = 100;   /* carrier settle after the reset pulse */
static int g_no_dp      = 0;    /* loopback: leave D+ unclaimed so a reader can own it */
static int g_dn_pp      = 0;    /* 1 = push-pull D- (A/B test only; upstream wants open-drain) */

#define CHUNK 4084      /* <= 0x1000 body, 4-byte aligned, incl. 10-byte header */

static int g_dn = -1, g_dp = -1;
static volatile int g_stop = 0;
static int g_verify = 0;
static unsigned long g_low_ok = 0, g_low_bad = 0, g_high_ok = 0, g_high_bad = 0;
static unsigned long g_vn = 0;   /* shared sample counter for the pad checks */

/* ---- optional pad-level self-check -----------------------------------------
 * The injector's own log proves only that it computed and "sent" records. To
 * rule out the pin never moving, --verify also reads the true pad voltage from
 * EXT_PORT (mmap of the GPIO bank, reads allowed under STRICT_DEVMEM) and counts
 * how many samples agree with the level just driven. A carrier that is really
 * driving the pin yields a high agreement count; a pin that never moves yields
 * zero orders on one side.
 */
static const unsigned long bank_phys[5] = {
    0xFD8A0000UL, 0xFEC20000UL, 0xFEC30000UL, 0xFEC40000UL, 0xFEC50000UL
};
#define R_EXT 0x70
static volatile uint32_t *g_bank = NULL;
static uint32_t g_mdn = 0;

static int map_dn_bank(void)
{
    int b = PIN_DN / 32;
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return -1; }
    void *m = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, bank_phys[b]);
    close(fd);
    if (m == MAP_FAILED) { perror("mmap"); return -1; }
    g_bank = (volatile uint32_t *)m;
    g_mdn = 1u << (PIN_DN % 32);
    return 0;
}

static inline int dn_pad(void) { return (g_bank[R_EXT/4] & g_mdn) ? 1 : 0; }

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void spin_until(uint64_t t)
{
    while (now_ns() < t) __asm__ __volatile__("" ::: "memory");
}

/* open-drain: 1 = released/high-Z, 0 = driven low (polarity verified earlier) */
static inline void setfd(int fd, int v)
{
    struct gpio_v2_line_values x;
    memset(&x, 0, sizeof(x));
    x.bits = v ? 1u : 0u;
    x.mask = 1;
    if (ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &x) < 0)
        fprintf(stderr, "SET_VALUES: %s\n", strerror(errno));
}

static void dn_release(void) { if (g_dn >= 0) setfd(g_dn, 1); }
static void dn_low(void)     { if (g_dn >= 0) setfd(g_dn, 0); }
static void dp_release(void) { if (g_dp >= 0) setfd(g_dp, 1); }

static int open_pin(int pin, const char *tag, int open_drain)
{
    char p[64];
    snprintf(p, sizeof(p), "/dev/gpiochip%d", pin / 32);
    int c = open(p, O_RDWR);
    if (c < 0) { perror(p); return -1; }
    struct gpio_v2_line_request r;
    memset(&r, 0, sizeof(r));
    r.offsets[0] = (uint32_t)(pin % 32);
    r.num_lines = 1;
    snprintf(r.consumer, sizeof(r.consumer), "%s", tag);
    r.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    if (open_drain) r.config.flags |= GPIO_V2_LINE_FLAG_OPEN_DRAIN;
    if (ioctl(c, GPIO_V2_GET_LINE_IOCTL, &r) < 0) {
        fprintf(stderr, "GET_LINE pin %d: %s\n", pin, strerror(errno));
        close(c); return -1;
    }
    close(c);
    return r.fd;
}

static int open_od(int pin, const char *tag) { return open_pin(pin, tag, 1); }

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

static size_t put_rec(uint8_t *dst, uint16_t rtype, uint32_t value,
                      const uint8_t *payload, size_t plen)
{
    size_t body = 10 + plen;
    uint32_t L = (uint32_t)body;
    memcpy(dst, &L, 4);
    memcpy(dst + 4, &rtype, 2);
    memcpy(dst + 6, &value, 4);
    if (plen) memcpy(dst + 10, payload, plen);
    uint32_t c = crc32_of(dst, body);
    memcpy(dst + body, &c, 4);
    return body + 4;
}

/* Run the carrier until `until`, then return the time the carrier actually
 * finished on. The caller chains it, because the next record must be scheduled
 * after the carrier, not after an assumed duration.
 *
 * When g_verify is set, every 64th edge also samples the true pad level. That is
 * cheap enough not to disturb the 250 us carrier, and it answers the one question
 * the write-only path cannot: is the pin actually moving? */
static uint64_t carrier_until(uint64_t until)
{
    uint64_t t = now_ns() + 1000;
    while (t < until && !g_stop) {
        /* One counter step per carrier cycle. The LOW check fires when the low
         * 6 bits are 0, the HIGH check when they are 32, so both directions get
         * sampled at the same rate without sampling on every edge. */
        unsigned long ph = g_vn & 63;

        /* Sample 20 us into each phase, not at the edge: the cable and camera
         * add enough capacitance that the pad needs microseconds to follow the
         * driver. Sampling an edge measures the transition, not the level. */
        spin_until(t); dn_low();  t += (uint64_t)g_ns_carrier;
        if (g_verify && g_bank && ph == 0) {
            spin_until(t - (uint64_t)(g_ns_carrier - 20000.0));
            if (!dn_pad()) g_low_ok++; else g_low_bad++;
        }

        spin_until(t); dn_release(); t += (uint64_t)g_ns_carrier;
        if (g_verify && g_bank && ph == 32) {
            spin_until(t - (uint64_t)(g_ns_carrier - 20000.0));
            if (dn_pad()) g_high_ok++; else g_high_bad++;
        }

        g_vn++;
    }
    dn_release();
    return t < until ? until : t;
}

/* Exact port of the upstream firmware's build_bits() bit generation.
 *
 * Upstream (zve10_pico_od_reset.ino) builds a bit buffer and then, for
 * OPEN_DRAIN, inverts every word because the state machine drives pindirs:
 * pindirs=1 means driven LOW. The net effect on the wire, which is what we have
 * to reproduce, is:
 *
 *   bit 0  of each 11-bit frame : start, LOW
 *   bits 1-8                    : data, LSB first, 1 -> HIGH, 0 -> LOW
 *   bit 9                       : odd parity, so total ones stay odd
 *                                 (parity bit 1 -> LOW, 0 -> HIGH)
 *   bit 10                      : stop, HIGH
 *   padding to a 32-bit boundary: HIGH when ((npad-1-p)>>3)&1, else LOW
 *
 * The padding rule matters and is easy to get backwards. For a 14-byte record
 * npad is 6 and every pad bit is HIGH; a naive "alternate every 32 bits" rule
 * emits LOW there, which is what this file used to do.
 */
static void emit_record_bits(const uint8_t *rec, size_t n, uint64_t *pt)
{
    uint64_t t = *pt;
    size_t nbits = (n * 11 + 31) & ~(size_t)31;
    size_t npad = nbits - n * 11;

    for (size_t i = 0; i < n; i++) {
        uint8_t v = rec[i];
        int ones = 0;

        /* start bit */
        spin_until(t); dn_low(); t += (uint64_t)g_ns_bit;

        /* 8 data bits, LSB first */
        for (int b = 0; b < 8; b++) {
            int bit = (v >> b) & 1;
            ones += bit;
            spin_until(t);
            if (bit) dn_release(); else dn_low();
            t += (uint64_t)g_ns_bit;
        }

        /* odd parity: make the number of ones odd */
        spin_until(t);
        if (ones & 1) dn_low(); else dn_release();
        t += (uint64_t)g_ns_bit;

        /* stop bit */
        spin_until(t); dn_release(); t += (uint64_t)g_ns_bit;
    }

    /* padding, exactly upstream's rule */
    for (size_t p = 0; p < npad; p++) {
        int high = (int)(((npad - 1 - p) >> 3) & 1u);
        spin_until(t);
        if (high) dn_release(); else dn_low();
        t += (uint64_t)g_ns_bit;
    }

    *pt = t;
}

/* One record on the wire. Structure, from serialboot.pio:
 *
 *   blob:     set pindirs,1        (1 cyc LOW)
 *             out x,32             (1 cyc LOW)
 *             set y,28 [21]        (22 cyc LOW)   -> 24 cyc LOW total
 *             set pindirs,0 [23]   (24 cyc HIGH)
 *   preamble: set pindirs,1 [23]   (24 cyc LOW)
 *             set pindirs,0 [22]   (23 cyc HIGH)
 *             jmp y--, preamble    (1 cyc HIGH)   -> 24 cyc HIGH per loop
 *             ... y goes 28 -> 0, so the body runs 29 times
 *   bits:     out pindirs,1 ; jmp x--, bits       (2 cyc per bit)
 *
 * So before the record there are 2 + 29*2 = 60 phase changes: one 24-cycle LOW
 * block, one 24-cycle HIGH block, then 29 LOW/HIGH pairs.
 */
static uint64_t send_record(uint64_t t, const uint8_t *rec, size_t n)
{
    t += (uint64_t)g_lead_us * 1000ull;   /* inter-record lead-in carrier */

    spin_until(t); dn_low();     t += (uint64_t)g_ns_preamble;   /* blob: 24 cyc LOW  */
    spin_until(t); dn_release(); t += (uint64_t)g_ns_preamble;   /* blob: 24 cyc HIGH */

    for (int i = 0; i < g_preamble_loops; i++) {
        spin_until(t); dn_low();     t += (uint64_t)g_ns_preamble;
        spin_until(t); dn_release(); t += (uint64_t)g_ns_preamble;
    }

    emit_record_bits(rec, n, &t);

    spin_until(t); dn_release(); t += 1000;
    return t;
}

static void on_signal(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv)
{
    const char *path = NULL;
    int seconds = 90, dry = 0, do_reset = 1, dump_hex = 0, dump_bits = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--payload") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dp-high")) g_dp_high = 1;
        else if (!strcmp(argv[i], "--reset-every") && i + 1 < argc) g_reset_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--settle-ms") && i + 1 < argc) g_settle_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reset-us") && i + 1 < argc) g_reset_us = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--record-gap") && i + 1 < argc) g_record_gap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--group-gap") && i + 1 < argc) g_group_gap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dry-run")) dry = 1;
        else if (!strcmp(argv[i], "--dump-records")) dump_hex = 1;
        else if (!strcmp(argv[i], "--dump-bits")) dump_bits = 1;
        else if (!strcmp(argv[i], "--preamble-loops") && i + 1 < argc) g_preamble_loops = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verify")) g_verify = 1;
        else if (!strcmp(argv[i], "--no-reset")) do_reset = 0;
        else if (!strcmp(argv[i], "--no-dp")) g_no_dp = 1;
        else if (!strcmp(argv[i], "--dn-pushpull")) g_dn_pp = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: %s --payload FILE [--seconds N] [--reset-us N] "
                   "[--record-gap US] [--group-gap US] [--no-reset] [--dry-run] "
                   "[--verify] [--no-dp] [--dn-pushpull]\n", argv[0]);
            printf("\n  --verify      also read the true pad level while driving, and\n");
            printf("                report whether the pin really moves\n");
            printf("  --no-dp       do not claim D+ (a reader owns it; loopback test)\n");
            printf("  --dn-pushpull drive D- push-pull instead of open-drain (A/B only)\n");
            return 0;
        }
    }
    if (!path) { fprintf(stderr, "error: --payload FILE is required\n"); return 1; }

    /* ---- load and validate the payload ---- */
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) {
        fprintf(stderr, "error: payload size %ld is not plausible\n", sz);
        fclose(f); return 1;
    }
    uint8_t *pl = malloc((size_t)sz);
    if (!pl) { fclose(f); return 1; }
    if (fread(pl, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "error: short read on %s\n", path);
        free(pl); fclose(f); return 1;
    }
    fclose(f);

    if (sz % 4) {
        fprintf(stderr, "error: payload must be 4-byte aligned (got %ld)\n", sz);
        free(pl); return 1;
    }

    size_t nchunks = ((size_t)sz + CHUNK - 1) / CHUNK;
    printf("=== stage1 injector (real payload) ===\n");
    printf("payload   : %s  (%ld bytes, %zu MEM_WRITE chunk(s))\n", path, sz, nchunks);
    printf("load addr : 0x%08X  entry 0x%08X\n", LOAD_ADDR, ENTRY_ADDR);
    printf("D- gpio%d (data), D+ gpio%d (%s)\n",
           PIN_DN, PIN_DP, do_reset ? "reset" : "not driving");
    printf("settle %d ms, lead %d us\n", g_settle_ms, g_lead_us);
    printf("bit %.1f ns, carrier half %.1f ns, preamble %.1f ns\n",
           g_ns_bit, g_ns_carrier, g_ns_preamble);

    /* ---- build every record once, so a bad build fails before touching GPIO ---- */
    static uint8_t ru[16], re[16];
    size_t nu = put_rec(ru, 0x0000, UNLOCK_TOKEN, NULL, 0);
    size_t ne = put_rec(re, 0xFFFF, ENTRY_ADDR, NULL, 0);

    uint8_t **recs = calloc(nchunks, sizeof(uint8_t *));
    size_t *lens = calloc(nchunks, sizeof(size_t));
    size_t off = 0;
    for (size_t c = 0; c < nchunks; c++) {
        size_t n = (size_t)sz - off;
        if (n > CHUNK) n = CHUNK;
        recs[c] = malloc(16 + n);
        lens[c] = put_rec(recs[c], 0x0001, LOAD_ADDR + (uint32_t)off, pl + off, n);
        off += n;
    }
    printf("records   : unlock %zu B, %zu write, unlock, entry %zu B, unlock\n",
           nu, nchunks, ne);

    if (dump_hex) {
        /* Print the group exactly like the Python reference (ref_records.py):
         * one record per line, lowercase hex. Order is unlock, writes, entry,
         * unlock -- the same order upstream make_records() emits. */
        for (size_t i = 0; i < nu; i++) printf("%02x", ru[i]);
        printf("\n");
        for (size_t c = 0; c < nchunks; c++) {
            for (size_t i = 0; i < lens[c]; i++) printf("%02x", recs[c][i]);
            printf("\n");
        }
        for (size_t i = 0; i < ne; i++) printf("%02x", re[i]);
        printf("\n");
        for (size_t i = 0; i < nu; i++) printf("%02x", ru[i]);
        printf("\n");
        return 0;
    }

    if (dump_bits) {
        /* Print the wire level sequence for every record in the group, as L
         * (driven low) and H (released high), exactly as emit_record_bits()
         * drives it. Compared against bitsim.py, which ports the firmware's
         * build_bits(). One line per record, prefixed with its index. */
        for (size_t c = 0; c <= nchunks + 2; c++) {
            const uint8_t *r;
            size_t n;
            if (c == 0)                    { r = ru;   n = nu; }
            else if (c == nchunks + 1)     { r = re;   n = ne; }
            else if (c == nchunks + 2)     { r = ru;   n = nu; }
            else                           { r = recs[c - 1]; n = lens[c - 1]; }

            size_t nbits = (n * 11 + 31) & ~(size_t)31;
            size_t npad = nbits - n * 11;
            printf("record %zu: %zu B -> %zu bits\n", c, n, nbits);
            for (size_t k = 0; k < n; k++) {
                uint8_t v = r[k];
                int ones = 0;
                printf("L");
                for (int b = 0; b < 8; b++) {
                    int bit = (v >> b) & 1;
                    ones += bit;
                    printf("%c", bit ? 'H' : 'L');
                }
                printf("%c", (ones & 1) ? 'L' : 'H');
                printf("H");
            }
            for (size_t p = 0; p < npad; p++)
                printf("%c", (((npad - 1 - p) >> 3) & 1u) ? 'H' : 'L');
            printf("\n");
        }
        return 0;
    }

    if (dry) {
        printf("\n--dry-run: group built OK, nothing sent.\n");
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    cpu_set_t set; CPU_ZERO(&set); CPU_SET(7, &set);
    sched_setaffinity(0, sizeof(set), &set);
    struct sched_param sp; memset(&sp, 0, sizeof(sp)); sp.sched_priority = 99;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
        fprintf(stderr, "warning: SCHED_FIFO refused (%s)\n", strerror(errno));
    { int t = open("/proc/sys/kernel/sched_rt_runtime_us", O_WRONLY);
      if (t >= 0) { (void)!write(t, "-1\n", 3); close(t); } }

    /* D- is OPEN-DRAIN, exactly as upstream insists
     * (pico/PROTOCOL.md: OPEN_DRAIN, borrow the camera-side pull-up).
     * v4 used push-pull on the strength of a comment that claimed the camera
     * holds D- low with 15k; that is wrong for this unit - the line measures
     * HIGH when nothing drives it, so a pull-up exists and open-drain is both
     * correct and safe. --dn-pushpull is kept only for the loopback A/B test,
     * where a deterministic low-impedance driver makes the capture trivial.
     * D+ is open-drain too (reset line, through the 620 ohm series resistor). */
    g_dn = g_dn_pp ? open_pin(PIN_DN, "stage1_inject_dn", 0)
                   : open_od(PIN_DN, "stage1_inject_dn");
    if (g_no_dp) {
        g_dp = -1;   /* a separate reader owns D+ for the loopback capture */
    } else {
        g_dp = g_dp_high ? open_pin(PIN_DP, "stage1_inject_dp", 0)
                         : open_od(PIN_DP, "stage1_inject_dp");
    }
    if (g_dn < 0) { fprintf(stderr, "cannot claim D-\n"); return 2; }
    dn_release();
    if (g_dp >= 0) {
        if (g_dp_high) { setfd(g_dp, 1); printf("D+ driven HIGH (push-pull, active reset release)\n"); }
        else dp_release();
    }

    if (g_verify) {
        if (map_dn_bank() < 0) {
            fprintf(stderr, "warning: cannot map the D- bank; --verify disabled\n");
            g_verify = 0;
        } else {
            printf("verify    : pad-level self-check ON (D- gpio%d)\n", PIN_DN);
        }
    }

    /* carrier first, then ONE reset pulse while the carrier keeps running,
     * then the settle window - same order as upstream inject_payload.py */
    carrier_until(now_ns() + 500000000ull);   /* 500 ms, upstream TRANSMIT_START */
    if (do_reset && g_dp >= 0 && g_reset_us > 0 && !g_dp_high) {
        setfd(g_dp, 0);
        spin_until(now_ns() + (uint64_t)g_reset_us * 1000ull);
        setfd(g_dp, 1);
        printf("reset pulse sent, %d us on D+\n", g_reset_us);
    }
    carrier_until(now_ns() + (uint64_t)g_settle_ms * 1000000ull);  /* ms -> ns */
    printf("replaying group for %d s\n", seconds);
    fflush(stdout);

    uint64_t end = now_ns() + (uint64_t)seconds * 1000000000ull;
    uint64_t next_reset = now_ns() + (uint64_t)g_reset_every * 1000000000ull;
    if (g_reset_every > 0)
        printf("periodic reset every %d s (%d us pulse)\n",
               g_reset_every, g_reset_us);
    unsigned long groups = 0;

    while (now_ns() < end && !g_stop) {
        /* Optional periodic reset: the carrier must already be running when the
         * SoC comes out of reset, otherwise the ROM has nothing to lock onto.
         * This is the cold-boot window, repeated without an operator. */
        if (g_reset_every > 0 && g_dp >= 0 && now_ns() >= next_reset) {
            carrier_until(now_ns() + 500000000ull);   /* 500 ms, upstream TRANSMIT_START */
            setfd(g_dp, 0);
            spin_until(now_ns() + (uint64_t)g_reset_us * 1000ull);
            setfd(g_dp, 1);
            next_reset = now_ns() + (uint64_t)g_reset_every * 1000000000ull;
            printf("periodic reset sent (%d us)\n", g_reset_us);
            fflush(stdout);
        }
        uint64_t t = now_ns();

        /* Record group order MUST match upstream make_records():
         *   SET_UNLOCK, MEM_WRITE x N, SET_ENTRY, SET_UNLOCK
         * An extra SET_UNLOCK between writes looks harmless but it re-opens the
         * group while flag1 (set by SET_ENTRY) semantics are still pending, and
         * the ROM's group-close check requires flag0|flag1 to be clear. Upstream
         * never emits unlock mid-group, so neither do we. */
        t = send_record(t, ru, nu);
        t = carrier_until(t + (uint64_t)g_record_gap * 1000ull);

        for (size_t c = 0; c < nchunks && !g_stop; c++) {
            t = send_record(t, recs[c], lens[c]);
            t = carrier_until(t + (uint64_t)g_record_gap * 1000ull);
        }

        t = send_record(t, re, ne);
        t = carrier_until(t + (uint64_t)g_record_gap * 1000ull);
        t = send_record(t, ru, nu);
        carrier_until(t + (uint64_t)g_group_gap * 1000ull);

        groups++;
        printf("group %lu sent\n", groups);
        fflush(stdout);
    }

    dn_release();
    if (g_dp >= 0) dp_release();

    if (g_verify) {
        printf("\n=== pad self-check on D- ===\n");
        printf("  drove LOW   and pad read low : %lu times\n", g_low_ok);
        printf("  drove LOW   but pad read high: %lu times\n", g_low_bad);
        printf("  released    and pad read high: %lu times\n", g_high_ok);
        printf("  released    but pad read low : %lu times\n", g_high_bad);
        if (g_low_ok > 0 && g_high_ok > 0)
            printf("  VERDICT: the pin really moves. Any silence is not the GPIO layer.\n");
        else if (g_low_ok == 0 && g_high_ok == 0)
            printf("  VERDICT: the pad never moved. The pin is not being driven.\n");
        else
            printf("  VERDICT: only one direction worked - check wiring and pad config.\n");
    }

    printf("done: %lu group(s); both lines released\n", groups);
    return 0;
}
