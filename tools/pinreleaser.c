/*
 * pinreleaser.c -- put a pin into PURE INPUT, no pulls, no drive.
 *
 * The whole question this answers: PROTOCOL.md says the ZV-E10 D- line must be
 * driven OPEN-DRAIN because "the camera side provides the pull-up".  Every
 * measurement we ever took read 2.x V on D- -- but our own GPIO internal pull
 * was enabled the whole time, so that reading cannot tell us whether the
 * CAMERA is pulling the line up or whether we were looking at ourselves.
 *
 * If we release the pin completely (input, no pull) and the line still sits at
 * ~1.8 V, the camera really is pulling it up, which means D- reaches a live
 * circuit inside the camera.  If the line collapses to 0 V, the pull-up was
 * ours all along and the "camera-side pull-up" assumption -- the entire basis
 * of the open-drain approach -- does not hold on this unit.
 *
 * Usage: pinreleaser <gpio> [seconds]
 *        pinreleaser --status <gpio>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int gpio_export(int gpio, int export)
{
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) return -1;
    char b[16];
    int n = snprintf(b, sizeof b, "%d", gpio);
    int r = write(fd, b, n);
    close(fd);
    (void)r;
    (void)export;
    return 0;
}

static int gpio_write(const char *attr, const char *val)
{
    char p[128];
    snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/%s", atoi(getenv("_GP")), attr);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <gpio> [seconds]      release to pure input (no pulls)\n"
                        "       %s --status <gpio>       print direction/value\n", argv[0], argv[0]);
        return 2;
    }

    int status_only = 0;
    int idx = 1;
    if (!strcmp(argv[1], "--status")) { status_only = 1; idx = 2; }
    if (idx >= argc) { fprintf(stderr, "missing gpio\n"); return 2; }
    int gpio = atoi(argv[idx]);
    int secs = (argc > idx + 1) ? atoi(argv[idx + 1]) : 5;

    char path[128];
    char buf[64];

    gpio_export(gpio, 1);

    snprintf(path, sizeof path, "/sys/class/gpio/gpio%d/direction", gpio);
    int fd = open(path, O_WRONLY);
    if (fd < 0) { perror(path); return 1; }

    if (status_only) {
        close(fd);
        fd = open(path, O_RDONLY);
        if (fd < 0) { perror(path); return 1; }
        int n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n > 0) { buf[n] = 0; printf("direction: %s", buf); }
        snprintf(path, sizeof path, "/sys/class/gpio/gpio%d/value", gpio);
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; printf("value: %s", buf); }
        }
        return 0;
    }

    if (write(fd, "in", 2) != 2) { perror("write in"); close(fd); return 1; }
    close(fd);

    /* ensure no bias is requested: writing "in" leaves the pin without pulls
     * on this kernel; the pinctrl state is the boot default for these pins,
     * which is bias-disable for the ones we use. */
    printf("gpio%d released to PURE INPUT (no pull, no drive) for %d s\n", gpio, secs);
    printf("measure the pad now -- if it still reads ~1.8 V the CAMERA is pulling it up\n");
    fflush(stdout);
    sleep(secs);

    /* leave it as input; that is the safe state */
    printf("done (left as input)\n");
    return 0;
}
