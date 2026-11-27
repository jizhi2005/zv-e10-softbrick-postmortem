/*
 * setbias.c -- request a GPIO line with an explicit BIAS setting and read it.
 *
 * WHY THIS IS THE DECISIVE EXPERIMENT
 * -----------------------------------
 * PROTOCOL.md for the ZV-E10 BootROM serial boot says the D- (A7) line must be
 * driven OPEN-DRAIN "borrowing the camera-side pull-up".  Every voltage we ever
 * measured on that pad (2.x V) was taken with the RK3588's OWN internal pull-up
 * enabled -- pinctrl reported "input bias pull up" on gpio1-7 the whole time.
 * So those readings could never tell us whether the CAMERA provides a pull-up.
 *
 * If we turn our internal pull-up off and the line still sits high, the camera
 * really is pulling it up: A7 reaches a live circuit inside the camera, and the
 * open-drain premise holds.
 *
 * If the line collapses to 0 with our pull-up off, nothing on the camera side
 * is biased: A7 is floating, i.e. it does NOT reach BOOT_SERIAL_IN.  That is a
 * physical, hardware-level cause -- no amount of waveform tuning can help.
 *
 * Usage:
 *   setbias <chip> <offset> <disabled|pullup|pulldown> [seconds] [hz]
 *   setbias --list <chip>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define GPIO_V2_LINES_MAX 64
#define GPIO_MAX_NAME_SIZE 32
#define GPIO_V2_LINE_NUM_ATTRS_MAX 10

struct gpio_v2_line_config_attribute {
    uint64_t attr;
    uint64_t mask;
    uint64_t flags;
};

struct gpio_v2_line_config {
    uint64_t flags;
    uint32_t num_attrs;
    uint32_t padding[5];
    struct gpio_v2_line_config_attribute attrs[GPIO_V2_LINE_NUM_ATTRS_MAX];
};

struct gpio_v2_line_request {
    uint32_t offsets[GPIO_V2_LINES_MAX];
    char consumer[GPIO_MAX_NAME_SIZE];
    struct gpio_v2_line_config config;
    uint32_t num_lines;
    uint32_t event_buffer_size;
    uint32_t padding[5];
    int32_t fd;
};

struct gpio_v2_line_values {
    uint64_t bits;
    uint64_t mask;
};

struct gpio_v2_line_info {
    char name[GPIO_MAX_NAME_SIZE];
    char consumer[GPIO_MAX_NAME_SIZE];
    uint32_t offset;
    uint32_t num_attrs;
    uint64_t flags;
    uint32_t padding[4];
    struct gpio_v2_line_config_attribute attrs[GPIO_V2_LINE_NUM_ATTRS_MAX];
};

#define GPIO_V2_GET_LINE_IOCTL       _IOWR(0xB4, 0x07, struct gpio_v2_line_request)
#define GPIO_V2_LINE_GET_VALUES_IOCTL _IOWR(0xB4, 0x0E, struct gpio_v2_line_values)
#define GPIO_V2_GET_LINEINFO_IOCTL   _IOWR(0xB4, 0x05, struct gpio_v2_line_info)

#define GPIO_V2_LINE_FLAG_INPUT             (1ULL << 2)
#define GPIO_V2_LINE_FLAG_OUTPUT            (1ULL << 3)
#define GPIO_V2_LINE_FLAG_BIAS_DISABLED     (1ULL << 6)
#define GPIO_V2_LINE_FLAG_BIAS_PULL_UP      (1ULL << 7)
#define GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN    (1ULL << 8)

static const char *flag_name(uint64_t f)
{
    if (f & GPIO_V2_LINE_FLAG_BIAS_DISABLED) return "BIAS_DISABLED";
    if (f & GPIO_V2_LINE_FLAG_BIAS_PULL_UP)  return "BIAS_PULL_UP";
    if (f & GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN)return "BIAS_PULL_DOWN";
    return "(no bias flag)";
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: %s <gpiochipN> <offset> <disabled|pullup|pulldown> [seconds] [hz]\n"
            "       %s --info <gpiochipN> <offset>\n", argv[0], argv[0]);
        return 2;
    }

    char path[64];
    int info_only = 0;
    int argbase = 1;

    if (!strcmp(argv[1], "--info")) { info_only = 1; argbase = 2; }
    snprintf(path, sizeof path, "/dev/gpiochip%s", argv[argbase]);

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        /* allow passing "32" or "gpiochip32" */
        snprintf(path, sizeof path, "/dev/%s", argv[argbase]);
        fd = open(path, O_RDWR | O_CLOEXEC);
    }
    if (fd < 0) { perror(path); return 1; }

    unsigned offset = (unsigned)strtoul(argv[argbase + 1], NULL, 0);

    if (info_only) {
        struct gpio_v2_line_info li;
        memset(&li, 0, sizeof li);
        li.offset = offset;
        if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &li) < 0) { perror("GET_LINEINFO"); return 1; }
        printf("offset %u name='%s' consumer='%s' flags=0x%llx -> %s\n",
               offset, li.name, li.consumer,
               (unsigned long long)li.flags, flag_name(li.flags));
        close(fd);
        return 0;
    }

    if (argc < argbase + 3) { fprintf(stderr, "missing bias\n"); return 2; }
    const char *bias = argv[argbase + 2];
    int secs = (argc > argbase + 3) ? atoi(argv[argbase + 3]) : 3;
    int hz   = (argc > argbase + 4) ? atoi(argv[argbase + 4]) : 5;
    if (hz <= 0) hz = 5;

    uint64_t flags = GPIO_V2_LINE_FLAG_INPUT;
    if (!strcmp(bias, "disabled"))      flags |= GPIO_V2_LINE_FLAG_BIAS_DISABLED;
    else if (!strcmp(bias, "pullup"))   flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    else if (!strcmp(bias, "pulldown")) flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    else { fprintf(stderr, "bad bias '%s'\n", bias); return 2; }

    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof req);
    req.offsets[0] = offset;
    req.num_lines = 1;
    snprintf(req.consumer, sizeof req.consumer, "setbias");
    req.config.flags = flags;
    req.config.num_attrs = 0;
    req.fd = -1;

    if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        perror("GET_LINE_IOCTL");
        fprintf(stderr, "hint: unexport the pin first if it is owned via sysfs\n");
        close(fd);
        return 1;
    }

    printf("line %u on %s requested: INPUT + %s\n", offset, argv[argbase], bias);
    printf("reading for %d s at %d Hz -- report the PAD voltage with the meter now\n",
           secs, hz);
    fflush(stdout);

    int n = secs * hz;
    int highs = 0;
    for (int i = 0; i < n; i++) {
        struct gpio_v2_line_values v;
        v.bits = 0; v.mask = 1;
        if (ioctl(req.fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) == 0) {
            int bit = (int)(v.bits & 1);
            highs += bit;
            printf("  [%3d] value=%d\n", i, bit);
        } else {
            perror("GET_VALUES");
            break;
        }
        fflush(stdout);
        usleep(1000000 / hz);
    }
    printf("--- summary: %d/%d samples HIGH ---\n", highs, n);

    close(req.fd);
    close(fd);
    return 0;
}
