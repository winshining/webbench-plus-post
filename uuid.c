#include <stdlib.h>
#include <stdio.h>

#define UUID_SIZE 36

char *random_uuid(char buf[UUID_SIZE + 1])
{
    int i, b;
    const char *c = "89ab";
    char *p = buf;

    for(i = 0; i < 16; ++i) {
        b = rand() % 255;

        switch (i) {
        case 6:
            sprintf(p, "4%x", b % 15);
            break;
        case 8:
            sprintf(p, "%c%x", c[rand() % strlen(c)], b % 15);
            break;
        default:
            sprintf(p, "%02x", b);
        }

        p += 2;
        switch (i) {
        case 3:
        case 5:
        case 7:
        case 9:
            *p++ = '-';
        }
    }

    *p = '\n';

    return buf;
}

