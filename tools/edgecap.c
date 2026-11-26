/* edgecap.c -- poor man's logic analyser for the GPIO loopback self-test.
 *
 * WHY THIS EXISTS
 * ---------------
 * The injector's log proves only that it *computed* a waveform. Every failed
 * attempt so far was diagnosed blind, because nobody could see the wire. With
 * P1_A7 (gpio39, D-) and P1_B0 (gpio40, D+) shorted by a dupont wire, this
 * tool samples one of the two pads and timestamps every transition, so these
 * numbers stop being beliefs and become measurements:
 *
 *     125.000 us   carrier half period   (12 PIO cycles @ 96 MHz)
 *     250.000 us   preamble block        (24 PIO cycles)
 *      20.833 us   data bit              ( 2 PIO cycles)
 *
 * It then decodes the first frame straight out of the sampled levels (start
 * bit, 8 data bits LSB-first, odd parity, stop bit), so framing and bit order
 * are verified too, not just the periods.
 *
 * HOW
 * ---
 * It does NOT use the kernel edge-event API: on a 5.10 kernel the v2 event
 * interface is not guaranteed and the 16-event kernel buffer is easy to
 * overrun at 48 kHz. Instead it mmaps the GPIO bank's EXT_PORT register and
 * busy-polls it, which is the same trick stage1_inject's --verify already uses
 * successfully on this board. Sample period is tens of nanoseconds, i.e. about
 * a thousand samples per data bit - a real logic analyser for our purposes.
 *
 * Because reading EXT_PORT does not claim the line, this can run next to the
 * injector with zero contention. Only the injector drives; this only looks.
 *
 * BUILD: aarch64-linux-gnu-gcc -O2 -o edgecap edgecap.c
 * USE  : ./edgecap --pin 40 --seconds 8          (injector drives gpio39)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>

/* RK3588 GPIO bank bases, same table as stage1_inject_v4.c */
static const unsigned long bank_phys[5] = {
    0xFD8A0000UL, 0xFEC20000UL, 0xFEC30000UL, 0xFEC40000UL, 0xFEC50000UL
};
#define R_EXT 0x70          /* EXT_PORT: the pad input level */

#define MAXD 16384
#define BIT_NS 20833.3125

static volatile int g_stop = 0;
static void onsig(int s) { (void)s; g_stop = 1; }

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef struct {
    unsigned long n;
    uint64_t sum, min, max;
} acc_t;

static void acc_add(acc_t *a, uint64_t v)
{
    if (!a->n || v < a->min) a->min = v;
    if (!a->n || v > a->max) a->max = v;
    a->n++; a->sum += v;
}
static double acc_mean(const acc_t *a) { return a->n ? (double)a->sum / (double)a->n : 0.0; }

int main(int argc, char **argv)
{
    int pin = 40, seconds = 10, maxd = 4000, raw = 70;
    int trigger = 1;
    long window_us = 4000;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pin") && i + 1 < argc) pin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) maxd = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window-us") && i + 1 < argc) window_us = atol(argv[++i]);
        else if (!strcmp(argv[i], "--no-trigger")) trigger = 0;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: %s [--pin 39|40] [--seconds N] [--max transitions] "
                   "[--window-us US] [--raw N] [--no-trigger]\n", argv[0]);
            return 0;
        }
    }
    if (maxd > MAXD) maxd = MAXD;
    int bank = pin / 32;
    if (bank > 4) { fprintf(stderr, "bad pin %d\n", pin); return 1; }
    uint32_t mask = 1u << (pin % 32);

    /* Sampler on CPU5 (a big core, away from the injector's CPU7) at FIFO 50,
     * so nothing normal can preempt the sample loop. */
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(5, &set);
    if (sched_setaffinity(0, sizeof(set), &set) < 0)
        fprintf(stderr, "note: could not pin to cpu5 (%s)\n", strerror(errno));
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 50;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
        fprintf(stderr, "note: SCHED_FIFO refused (%s)\n", strerror(errno));
    /* The kernel throttles continuous RT to 950 ms per second. A capture that
     * waits for its trigger longer than that gets frozen for 50 ms mid-run, so
     * turn throttling off (same thing stage1_inject does). */
    { int t = open("/proc/sys/kernel/sched_rt_runtime_us", O_WRONLY);
      if (t >= 0) { (void)!write(t, "-1\n", 3); close(t); } }

    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 2; }
    void *m = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, bank_phys[bank]);
    close(fd);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    volatile uint32_t *reg = (volatile uint32_t *)m;

    printf("edgecap: sampling gpio%d (bank%d @0x%08lX EXT_PORT bit %d)\n",
           pin, bank, bank_phys[bank], pin % 32);
    printf("         %d s, trigger=%s, max %d transitions, window %ld us\n",
           seconds, trigger ? "first 250us block" : "none", maxd, window_us);
    {
        uint32_t v = reg[R_EXT / 4];
        printf("         EXT_PORT=0x%08X -> gpio%d reads %d\n",
               v, pin, (v & mask) ? 1 : 0);
    }
    if (reg[R_EXT / 4] == 0xFFFFFFFFu) printf("         (warning: register reads all-ones)\n");
    fflush(stdout);

    signal(SIGINT, onsig);
    signal(SIGTERM, onsig);

    static uint64_t del[MAXD], ts[MAXD];
    static int lvl[MAXD];
    int nd = 0, triggered = 0;
    unsigned long ntrans = 0;
    uint64_t end = now_ns() + (uint64_t)seconds * 1000000000ull;
    uint64_t prev_t = now_ns();
    int prev_v = (reg[R_EXT / 4] & mask) ? 1 : 0;
    uint64_t trig_t = 0;

    while (!g_stop && now_ns() < end) {
        uint64_t t = now_ns();
        int v = (reg[R_EXT / 4] & mask) ? 1 : 0;
        if (v == prev_v) continue;

        uint64_t d = t - prev_t;
        prev_t = t;
        prev_v = v;
        ntrans++;

        if (!triggered) {
            /* The 250 us preamble block is the only interval in this window.
             * The carrier is 125 us and the short lead-in before a record is
             * 50 us, so +/-8% around 250 us cannot be confused with either. */
            if (!trigger || (d >= 230000ull && d <= 270000ull)) {
                triggered = 1;
                trig_t = t;
                del[nd] = d; ts[nd] = t; lvl[nd] = v; nd++;
            }
            continue;
        }
        del[nd] = d; ts[nd] = t; lvl[nd] = v; nd++;
        if (nd >= maxd) break;
        if (window_us > 0 && t - trig_t > (uint64_t)window_us * 1000ull) break;
    }
    munmap(m, 0x1000);

    printf("transitions seen=%lu  recorded=%d  triggered=%d\n", ntrans, nd, triggered);
    if (!nd) {
        printf("NOTHING CAPTURED\n");
        printf("  if the injector was running: no contact on the short, or the\n");
        printf("  sampler pin is not on the same net as the driven pin.\n");
        return 3;
    }

    /* ---- interval histogram ---- */
    acc_t bit = {0}, lead = {0}, car = {0}, pre = {0}, mid = {0}, big = {0};
    for (int i = 0; i < nd; i++) {
        uint64_t d = del[i];
        if (d < 5000ull) continue;
        else if (d < 40000ull) acc_add(&bit, d);
        else if (d < 100000ull) acc_add(&lead, d);
        else if (d < 170000ull) acc_add(&car, d);
        else if (d < 400000ull) acc_add(&pre, d);
        else if (d < 1000000ull) acc_add(&mid, d);
        else acc_add(&big, d);
    }
    printf("\n-- interval histogram --\n");
    printf("  data bit  5-40 us  : n=%lu mean=%.1f ns  min=%llu max=%llu  (expect 20833)\n",
           bit.n, acc_mean(&bit), (unsigned long long)bit.min, (unsigned long long)bit.max);
    printf("  lead     40-100 us : n=%lu mean=%.1f ns  (expect 50000)\n",
           lead.n, acc_mean(&lead));
    printf("  carrier 100-170 us : n=%lu mean=%.1f ns  min=%llu max=%llu  (expect 125000)\n",
           car.n, acc_mean(&car), (unsigned long long)car.min, (unsigned long long)car.max);
    printf("  preamble 170-400 us: n=%lu mean=%.1f ns  min=%llu max=%llu  (expect 250000)\n",
           pre.n, acc_mean(&pre), (unsigned long long)pre.min, (unsigned long long)pre.max);
    printf("  gap     400us-1ms  : n=%lu mean=%.1f ns\n", mid.n, acc_mean(&mid));
    printf("  huge      >1ms     : n=%lu mean=%.1f ns\n", big.n, acc_mean(&big));

    int i = 0, nslow = 0;
    while (i < nd && del[i] >= 230000ull && del[i] <= 270000ull) { nslow++; i++; }
    printf("\n-- preamble --\n");
    printf("  consecutive 250 us blocks from trigger: %d (expect 60 = 2 + 29*2)\n", nslow);
    if (i < nd)
        printf("  first interval after preamble: %llu ns (expect 20833)\n",
               (unsigned long long)del[i]);

    if (nslow >= 1 && i < nd) {
        /* ts[nslow-1] is the edge that ENDS the last 250 us block; that edge is
         * the start of the start bit. (Using ts[nslow] here would be one bit
         * late and decode garbage.) The sampler may have joined mid-preamble,
         * so any run length is decodable - only the LAST block matters. */
        int e0 = nslow - 1;
        uint64_t t0 = ts[e0];
        int lv[11], ok = 1;
        for (int b = 0; b < 11; b++) {
            uint64_t sample = t0 + (uint64_t)((b + 0.5) * BIT_NS);
            int k = e0;
            while (k + 1 < nd && ts[k + 1] <= sample) k++;
            if (k + 1 >= nd) { ok = 0; break; }
            lv[b] = lvl[k];
        }
        if (ok) {
            unsigned v = 0;
            int ones = 0;
            for (int b = 1; b <= 8; b++) { v |= (unsigned)lv[b] << (b - 1); ones += lv[b]; }
            int want = (ones & 1) ? 0 : 1;   /* odd parity overall */
            printf("\n-- first frame decoded --\n");
            printf("  start=%d (want 0)  byte=0x%02X  parity=%d  stop=%d (want 1)\n",
                   lv[0], v, lv[9], lv[10]);
            printf("  odd parity: %s (data ones=%d)\n",
                   (lv[9] == want) ? "OK" : "MISMATCH", ones);
        } else {
            printf("\n  (capture too short to decode a full frame)\n");
        }
    }

    if (raw > nd) raw = nd;
    printf("\n-- first %d intervals (level after the edge) --\n", raw);
    for (int k = 0; k < raw; k++)
        printf("  %4d %8llu ns  %s\n", k, (unsigned long long)del[k],
               lvl[k] ? "HIGH" : "LOW ");
    printf("\nDONE\n");
    return 0;
}
