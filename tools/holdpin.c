/* holdpin.c -- park one JP5 pin at a fixed level so the operator can measure it
 * with a multimeter. Exists because two questions can only be answered with a
 * meter, not with a logic level:
 *
 *   1) what does the camera side idles at, and
 *   2) when we pull the reset line (D+, gpio40) LOW through the 620 ohm series
 *      resistor, does the level at the plug actually reach a legal logic 0?
 *
 * The second one matters: 3.3V * 620/(620+1500) = 0.96 V, which is NOT a legal
 * low for 3.3V CMOS, while 1.8V * 620/(620+1500) = 0.53 V, which is. If the
 * reset pulse has been sitting at ~0.9 V for weeks, it never registered, and no
 * amount of protocol work would have helped.
 *
 * Open-drain is the default so the measurement matches what the injector
 * actually does (upstream insists on open-drain; the reset line is the same).
 *
 *   ./holdpin --pin 40 --level low  --ms 3000
 *   ./holdpin --pin 40 --level high --ms 3000
 *   ./holdpin --pin 40 --level low  --ms 3000 --pushpull
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

static int open_pin(int pin, int open_drain)
{
    char p[64];
    snprintf(p, sizeof(p), "/dev/gpiochip%d", pin / 32);
    int c = open(p, O_RDWR);
    if (c < 0) { perror(p); return -1; }
    struct gpio_v2_line_request r;
    memset(&r, 0, sizeof(r));
    r.offsets[0] = (uint32_t)(pin % 32);
    r.num_lines = 1;
    snprintf(r.consumer, sizeof(r.consumer), "holdpin");
    r.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    if (open_drain) r.config.flags |= GPIO_V2_LINE_FLAG_OPEN_DRAIN;
    if (ioctl(c, GPIO_V2_GET_LINE_IOCTL, &r) < 0) {
        fprintf(stderr, "GET_LINE %d: %s\n", pin, strerror(errno));
        close(c); return -1;
    }
    close(c);
    return r.fd;
}

static void setv(int fd, int v)
{
    struct gpio_v2_line_values x;
    memset(&x, 0, sizeof(x));
    x.bits = v ? 1u : 0u;
    x.mask = 1;
    if (ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &x) < 0)
        fprintf(stderr, "SET_VALUES: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
    int pin = 40, ms = 3000, level = 0, od = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pin") && i + 1 < argc) pin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ms") && i + 1 < argc) ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--level") && i + 1 < argc)
            level = strcmp(argv[++i], "high") == 0;
        else if (!strcmp(argv[i], "--pushpull")) od = 0;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: %s [--pin N] [--level low|high] [--ms N] [--pushpull]\n",
                   argv[0]);
            return 0;
        }
    }
    int fd = open_pin(pin, od);
    if (fd < 0) return 2;
    /* open-drain: 1 = released (high-Z, the far side decides), 0 = driven low */
    setv(fd, level ? 1 : 0);
    printf("gpio%d held %s (%s) for %d ms -- MEASURE NOW\n",
           pin, level ? "HIGH" : "LOW", od ? "open-drain" : "push-pull", ms);
    fflush(stdout);
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { }
    close(fd);
    printf("released\n");
    return 0;
}
