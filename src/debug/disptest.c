/* disptest <cmd-hex> [value]: issue one /dev/disp ioctl with {0, value, 0, 0}
 * and print the result. Debug tool for finding the backlight controls. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: disptest <cmd-hex> [value]\n"); return 2; }
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) { perror("/dev/disp"); return 1; }
    unsigned long cmd = strtoul(argv[1], NULL, 16);
    unsigned long arg[4] = { 0, argc > 2 ? strtoul(argv[2], NULL, 0) : 0, 0, 0 };
    int r = ioctl(fd, cmd, arg);
    printf("ioctl(0x%lx, {0,%lu}) = %d%s%s\n", cmd, arg[1], r, r < 0 ? " " : "", r < 0 ? strerror(errno) : "");
    return 0;
}
