/* lsetup.c — attach a file to a free loop device with offset/sizelimit
 * (Android-friendly; uses LOOP_CONFIGURE ioctl, kernel >= 4.0)
 * Usage: lsetup <file> <offset_bytes> [sizelimit_bytes]
 * Prints the loop device path, e.g. /dev/block/loop1
 * Detach: lsetup -d /dev/block/loopN
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/loop.h>

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "-d")) {
        if (argc < 3) { fprintf(stderr, "usage: lsetup -d <loopdev>\n"); return 2; }
        int fd = open(argv[2], O_RDONLY);
        if (fd < 0) { perror("open"); return 1; }
        if (ioctl(fd, LOOP_CLR_FD, 0) < 0) { perror("LOOP_CLR_FD"); return 1; }
        close(fd);
        printf("%s detached\n", argv[2]);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: lsetup <file> <offset_bytes> [sizelimit_bytes]\n       lsetup -d <loopdev>\n");
        return 2;
    }
    const char *img = argv[1];
    unsigned long long offset = strtoull(argv[2], NULL, 0);
    unsigned long long sizelimit = argc > 3 ? strtoull(argv[3], NULL, 0) : 0;

    int imgfd = open(img, O_RDWR);
    if (imgfd < 0) { perror("open image"); return 1; }

    char dev[64];
    int ctrl, i;
    /* find free loop device by probing /dev/block/loop0..255 */
    for (i = 0; i < 256; i++) {
        snprintf(dev, sizeof(dev), "/dev/block/loop%d", i);
        ctrl = open(dev, O_RDWR);
        if (ctrl < 0) continue;
        /* LOOP_CONFIGURE returns EBUSY if used */
        struct loop_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.fd = imgfd;
        cfg.block_size = 4096;
        cfg.info.lo_offset = (long long)offset;
        if (sizelimit) cfg.info.lo_sizelimit = (long long)sizelimit;
        if (ioctl(ctrl, LOOP_CONFIGURE, &cfg) == 0) {
            printf("%s\n", dev);
            close(ctrl);
            return 0;
        }
        close(ctrl);
    }
    fprintf(stderr, "lsetup: no free loop device or LOOP_CONFIGURE failed\n");
    return 1;
}
