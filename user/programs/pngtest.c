/*
 * pngtest <files...>: decode PNGs and print each one's size and a checksum
 * of its pixels (FNV-1a over the premultiplied ARGB values), to compare
 * against a reference decoder.
 */

#include <image.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int failures = 0;
    for (int i = 1; i < argc; i++) {
        const char *error = NULL;
        uint64_t t0 = uptime_ms();
        struct gfx_surface *s = image_load_png(argv[i], &error);
        uint64_t ms = uptime_ms() - t0;
        const char *name = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        if (!s) {
            printf("%s FAILED: %s\n", name, error ? error : "?");
            failures++;
            continue;
        }
        uint32_t hash = 0x811C9DC5u;
        for (int y = 0; y < s->height; y++) {
            for (int x = 0; x < s->width; x++) {
                hash = (hash ^ gfx_row(s, y)[x]) * 16777619u;
            }
        }
        printf("%s %dx%d %08x (%lu ms)\n", name, s->width, s->height, hash, ms);
        gfx_surface_destroy(s);
    }
    return failures ? 1 : 0;
}
