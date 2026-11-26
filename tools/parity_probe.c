/*
 * parity_probe.c -- port of upstream uart_boot_probe.ino to RK3588 GPIO.
 *
 * ============================================================================
 * WHY THIS EXISTS  (read this before changing anything)
 * ============================================================================
 * Every test so far sent a *legal* SET_UNLOCK record and then looked for a
 * reaction. When nothing happened, that told us nothing: we could not tell
 *
 *   (a) the signal never reached the BootROM serial receiver, from
 *   (b) the signal arrived but the record structure/timing was rejected.
 *
 * Upstream (DavidBuchanan314/ILCE-7M4-RE tools/uart_boot_probe) solves exactly
 * this ambiguity by sending *deliberate garbage*: a plain 233 us / 233 us
 * square wave with no start bits, no framing, nothing. That is guaranteed to
 * fail odd parity, and the camera then reports ROM error 0x100D = "13 short
 * flashes" (1 long + 13 short). That error code is a *self-report from the
 * BootROM*, so it proves the signal reached the receiver.
 *
 *   parity flashes observed  -> signal reaches BootROM, wiring/CC is fine,
 *                               and any remaining problem is record framing.
 *   no parity flashes        -> the signal is NOT reaching the receiver, and no
 *                               amount of payload work can help.
 *
 * Upstream note: "If your camera 'dies', don't panic, just pull the battery."
 * Here we send a short bounded burst instead of an infinite loop, and always
 * restore the line to high-Z afterwards.
 *
 * ============================================================================
 * WHAT IT DRIVES
 * ============================================================================
 *   D-  (JP5 P1_A7 = gpio39) : the square wave, open-drain, toggling.
 *   D+  (JP5 P1_B0 = gpio40) : one 10 us low reset pulse first (via series R).
 *
 * Upstream asserts the wave BEFORE the reset and lets it run continuously, so
 * the ROM finds a busy line when it starts sampling. We do the same: carrier
 * starts, reset fires, then we alternate 233/233 for --wave-ms.
 *
 * ============================================================================
 * USAGE
 * ============================================================================
 *   ./parity_probe                      # 4 attempts, 800 ms wave each
 *   ./parity_probe --attempts 1 --wave-ms 1500
 *   ./parity_probe --no-reset           # for testing reset-vs-wave separately
 *   ./parity_probe --gap-us 233         # match upstream exactly (default)
 *
 * Watch the camera's LED (or run camtrack) for the 13-short-flash pattern.
 *
 * BUILD: aarch64-linux-gnu-gcc -O2 -static -o parity_probe parity_probe.c
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
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

/* JP5 header pins that the ZV-E10 wiring lives on (see project notes). */
#ifndef PIN_DN
#define PIN_DN 39   /* P1_A7 -- camera D-  (data)  */
#endif
#ifndef PIN_DP
#define PIN_DP 40   /* P1_B0 -- camera D+  (reset) */
#endif

static int fd_dn = -1, fd_dp = -1;
static volatile int stop = 0;

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

/* open-drain: value 1 = released/high-Z, value 0 = driven low.
 * This polarity was verified on this hardware earlier in the project. */
static inline void setfd(int fd, int v)
{
    struct gpio_v2_line_values x;
    memset(&x, 0, sizeof(x));
    x.bits = v ? 1u : 0u;
    x.mask = 1;
    if (ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &x) < 0)
        fprintf(stderr, "SET_VALUES: %s\n", strerror(errno));
}

static int open_od(int pin, const char *tag)
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
    r.config.flags = GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_OPEN_DRAIN;
    if (ioctl(c, GPIO_V2_GET_LINE_IOCTL, &r) < 0) {
        fprintf(stderr, "GET_LINE pin %d: %s\n", pin, strerror(errno));
        close(c); return -1;
    }
    close(c);
    return r.fd;
}

static void on_signal(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
    int attempts = 4;
    double wave_ms = 800.0;
    double gap_us = 233.0;
    int do_reset = 1;
    double pre_ms = 200.0;   /* upstream: 200 ms between reset and starting TX */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--attempts") && i + 1 < argc) attempts = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wave-ms") && i + 1 < argc) wave_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gap-us") && i + 1 < argc) gap_us = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pre-ms") && i + 1 < argc) pre_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--no-reset")) do_reset = 0;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: %s [--attempts N] [--wave-ms MS] [--gap-us US] "
                   "[--pre-ms MS] [--no-reset]\n", argv[0]);
            return 0;
        }
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

    fd_dn = open_od(PIN_DN, "parity_probe_dn");
    fd_dp = open_od(PIN_DP, "parity_probe_dp");
    if (fd_dn < 0) { fprintf(stderr, "cannot claim D- (gpio%d)\n", PIN_DN); return 2; }
    if (do_reset && fd_dp < 0) {
        fprintf(stderr, "cannot claim D+ (gpio%d) for reset; use --no-reset\n", PIN_DP);
        return 2;
    }

    setfd(fd_dn, 1);                 /* release both lines before starting */
    if (fd_dp >= 0) setfd(fd_dp, 1);

    printf("=== BootROM parity probe (port of uart_boot_probe.ino) ===\n");
    printf("D- gpio%d toggling %g us / %g us, %d attempt(s), %g ms wave each\n",
           PIN_DN, gap_us, gap_us, attempts, wave_ms);
    printf("D+ gpio%d %s\n", PIN_DP, do_reset ? "10 us reset pulse first" : "NOT driving reset");
    printf("\nThis sends DELIBERATE GARBAGE. A healthy BootROM receiver replies with\n");
    printf("error 0x100D = 1 long + 13 short flashes. If you see those flashes, the\n");
    printf("signal is reaching the serial receiver and the wiring is proven.\n\n");

    const uint64_t half_ns = (uint64_t)(gap_us * 1000.0);
    const uint64_t wave_ns = (uint64_t)(wave_ms * 1000000.0);

    for (int a = 0; a < attempts && !stop; a++) {
        printf("--- attempt %d/%d ---\n", a + 1, attempts);
        fflush(stdout);

        /* 1. Get the line busy FIRST, so the ROM samples a toggling input
         *    the moment it comes up (upstream does the same ordering). */
        uint64_t t = now_ns() + 1000;
        uint64_t wave_end = t + wave_ns;
        while (t < wave_end && !stop) {
            spin_until(t); setfd(fd_dn, 0); t += half_ns;
            spin_until(t); setfd(fd_dn, 1); t += half_ns;
        }
        setfd(fd_dn, 1);

        /* 2. Reset pulse: 10 us driven low, then back to high-Z.
         *    Never driven high -- open-drain only pulls down. */
        if (do_reset && !stop) {
            setfd(fd_dp, 0);
            spin_until(now_ns() + 10000ull);
            setfd(fd_dp, 1);
            printf("    reset pulse sent, waiting %g ms\n", pre_ms);
            fflush(stdout);
            usleep((useconds_t)(pre_ms * 1000.0));
        }

        /* 3. Now the garbage wave, which is what the ROM should choke on. */
        if (!stop) {
            t = now_ns() + 1000;
            wave_end = t + wave_ns;
            while (t < wave_end && !stop) {
                spin_until(t); setfd(fd_dn, 0); t += half_ns;
                spin_until(t); setfd(fd_dn, 1); t += half_ns;
            }
            setfd(fd_dn, 1);
        }

        printf("    wave finished; watch the camera LED now\n");
        fflush(stdout);
        if (a + 1 < attempts) usleep(3000000);
    }

    setfd(fd_dn, 1);
    if (fd_dp >= 0) setfd(fd_dp, 1);
    printf("\n=== done; both lines released (high-Z) ===\n");
    return 0;
}
