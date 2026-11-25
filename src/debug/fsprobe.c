/* fsprobe <blockdev>: print the filesystem signature of a partition (read-only). */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    unsigned char b[512];
    int fd = open(argc > 1 ? argv[1] : "/dev/block/vold/179:1", O_RDONLY);
    if (fd < 0 || read(fd, b, 512) != 512) { perror("read"); return 1; }
    const char *fs = "unknown";
    if (!memcmp(b + 3, "EXFAT   ", 8)) fs = "exFAT";
    else if (!memcmp(b + 3, "NTFS    ", 8)) fs = "NTFS";
    else if (!memcmp(b + 82, "FAT32   ", 8)) fs = "FAT32";
    else if (!memcmp(b + 54, "FAT1", 4)) fs = "FAT12/16";
    printf("%s  oem='%.8s' sig=%02x%02x\n", fs, b + 3, b[510], b[511]);
    return 0;
}
