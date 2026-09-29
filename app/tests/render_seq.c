/* Dump frames for a promo render: render_seq <out.raw> <start hh:mm:ss> <seconds> <fps> <depth> <glow> <colour> <show_seconds>
 * Raw RGB frames, 72x16x3 each, back to back. */
#include "../nixie_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
    if(argc < 9) { fprintf(stderr, "args\n"); return 1; }
    int hh, mm, ss; sscanf(argv[2], "%d:%d:%d", &hh, &mm, &ss);
    const int seconds = atoi(argv[3]), fps = atoi(argv[4]);
    NixiePreset p; nixie_preset_make(&p, (NixieColour)atoi(argv[7]));
    nixie_preset_set_depth(&p, (uint8_t)atoi(argv[5]));
    nixie_preset_set_glow(&p, (NixieGlow)atoi(argv[6]));
    const int show_seconds = atoi(argv[8]);
    FILE* o = fopen(argv[1], "wb");
    uint8_t f[NIXIE_FRAME_BYTES];
    const int64_t t0 = hh * 3600 + mm * 60 + ss;
    for(int i = 0; i < seconds * fps; i++) {
        const int64_t ms = (int64_t)i * 1000 / fps;
        nixie_render(f, t0 + ms / 1000, (uint32_t)(ms % 1000), &p, show_seconds != 0);
        fwrite(f, 1, sizeof f, o);
    }
    fclose(o);
    return 0;
}
