/* biasread.c -- use the SoC's own internal pull resistors as a crude ohmmeter.
 *
 * For each pin we request it as an INPUT three times, with BIAS_PULL_DOWN,
 * BIAS_PULL_UP and BIAS_DISABLE, and read the level each time.
 *
 *   bare pin (nothing attached)   : pulldown -> LOW, pullup -> HIGH
 *   pin tied to a stiff external node (device pull-up/down, or a driven pin)
 *                                 : pulldown and pullup agree with the node
 *
 * So "pulldown still reads HIGH" means something on the far side is holding it
 * up harder than the SoC's own pulldown - i.e. the wire really is connected to
 * a live circuit. That is the test we cannot do with a level reading alone.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

static int read_with_bias(int pin, uint64_t bias, int *level)
{
    char p[64];
    snprintf(p, sizeof(p), "/dev/gpiochip%d", pin / 32);
    int c = open(p, O_RDWR);
    if (c < 0) return -1;
    struct gpio_v2_line_request r;
    memset(&r, 0, sizeof(r));
    r.offsets[0] = (uint32_t)(pin % 32);
    r.num_lines = 1;
    snprintf(r.consumer, sizeof(r.consumer), "biasread");
    r.config.flags = GPIO_V2_LINE_FLAG_INPUT | bias;
    if (ioctl(c, GPIO_V2_GET_LINE_IOCTL, &r) < 0) { close(c); return -1; }
    close(c);
    struct gpio_v2_line_values v;
    memset(&v, 0, sizeof(v));
    v.mask = 1;
    int ok = (ioctl(r.fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) == 0);
    *level = (v.bits & 1) ? 1 : 0;
    close(r.fd);
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    int pins[8], n = 0;
    if (argc < 2) { fprintf(stderr, "usage: biasread pin...\n"); return 2; }
    for (int i = 1; i < argc && n < 8; i++) pins[n++] = atoi(argv[i]);

    printf("%-6s %-10s %-10s %-10s   %s\n", "pin", "PULLDOWN", "PULLUP", "DISABLE", "reading");
    for (int i = 0; i < n; i++) {
        int a = -1, b = -1, d = -1;
        int ra = read_with_bias(pins[i], GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN, &a);
        int rb = read_with_bias(pins[i], GPIO_V2_LINE_FLAG_BIAS_PULL_UP,   &b);
        int rd = read_with_bias(pins[i], GPIO_V2_LINE_FLAG_BIAS_DISABLED,   &d);
        printf("%-6d %-10s %-10s %-10s   ", pins[i],
               ra ? "ERR" : (a ? "1(HIGH)" : "0(low)"),
               rb ? "ERR" : (b ? "1(HIGH)" : "0(low)"),
               rd ? "ERR" : (d ? "1(HIGH)" : "0(low)"));
        if (ra || rb || rd) printf("(ioctl refused)\n");
        else if (a == 1 && b == 1) printf("tied HIGH by the far side -> CONNECTED to something pulling up\n");
        else if (a == 0 && b == 0) printf("tied LOW by the far side  -> CONNECTED to something pulling down / driving low\n");
        else if (a == 0 && b == 1) printf("follows our bias          -> looks BARE / floating\n");
        else printf("unexpected\n");
    }
    return 0;
}