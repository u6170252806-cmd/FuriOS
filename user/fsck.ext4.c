#include "user.h"

static void usage(void) {
    puts("usage: fsck.ext4 [-n|-p|-y] <device>\n");
}

int main(int argc, char **argv) {
    unsigned long flags = 0;
    char *target = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            flags |= FSCK_EXT4_F_NO_REPAIR;
            continue;
        }
        if (strcmp(argv[i], "-p") == 0) {
            flags |= FSCK_EXT4_F_PREEN;
            continue;
        }
        if (strcmp(argv[i], "-y") == 0) {
            flags |= FSCK_EXT4_F_YES;
            continue;
        }
        if (argv[i][0] == '-' || target) {
            usage();
            return 1;
        }
        target = argv[i];
    }

    if (!target) {
        usage();
        return 1;
    }

    if ((flags & FSCK_EXT4_F_NO_REPAIR) &&
        (flags & (FSCK_EXT4_F_PREEN | FSCK_EXT4_F_YES))) {
        usage();
        return 1;
    }
    if ((flags & FSCK_EXT4_F_PREEN) && (flags & FSCK_EXT4_F_YES)) {
        usage();
        return 1;
    }

    int rc = sys_fsckext4(target, flags);
    if (rc < 0) {
        puts("fsck.ext4: failed\n");
        return 1;
    }
    if (rc == 0) {
        puts("fsck.ext4: clean\n");
    } else {
        puts("fsck.ext4: repaired\n");
    }
    return 0;
}
