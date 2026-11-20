/* Example HB Studio source plugin: two test-pattern video sources ("Barras de cor" and "Rampa animada").
 * Plain C, no dependencies. This is also the reference for plugin authors: see docs/PLUGINS.md. */
#include <stdlib.h>
#include <string.h>

#include "hb_plugin_api.h"

typedef struct {
    int kind; /* 0 = bars, 1 = ramp */
} example_source;

static int list_sources(hb_source_desc* out, int max) {
    if (max < 2) return -1;
    memset(out, 0, sizeof(hb_source_desc) * 2);
    strcpy(out[0].id, "bars");
    strcpy(out[0].name, "Exemplo: barras de cor");
    strcpy(out[1].id, "ramp");
    strcpy(out[1].name, "Exemplo: rampa animada");
    return 2;
}

static void* open_source(const char* id) {
    example_source* s;
    int kind;
    if (!id) return NULL;
    if (strcmp(id, "bars") == 0) kind = 0;
    else if (strcmp(id, "ramp") == 0) kind = 1;
    else return NULL;
    s = (example_source*)malloc(sizeof(example_source));
    if (!s) return NULL;
    s->kind = kind;
    return s;
}

static int render(void* handle, uint8_t* bgra, int width, int height, int stride, uint64_t index) {
    static const uint8_t bars[7][3] = {{235, 235, 235}, {16, 235, 235}, {235, 235, 16}, {16, 235, 16},
                                       {235, 16, 235},  {16, 16, 235},  {235, 16, 16}}; /* B,G,R */
    example_source* s = (example_source*)handle;
    int x, y;
    if (!s || !bgra || width <= 0 || height <= 0 || stride < width * 4) return -1;
    for (y = 0; y < height; ++y) {
        uint8_t* row = bgra + (size_t)y * (size_t)stride;
        for (x = 0; x < width; ++x) {
            uint8_t* p = row + x * 4;
            if (s->kind == 0) {
                const uint8_t* c = bars[x * 7 / width];
                const int marker = y > height * 3 / 4 && (x * 40 / width) == (int)(index % 40);
                p[0] = marker ? 255 : c[0];
                p[1] = marker ? 255 : c[1];
                p[2] = marker ? 255 : c[2];
            } else {
                const int v = (int)(((unsigned)x * 255u / (unsigned)width + (unsigned)index * 4u) & 255u);
                p[0] = (uint8_t)v;
                p[1] = (uint8_t)(y * 255 / height);
                p[2] = (uint8_t)(255 - v);
            }
            p[3] = 255;
        }
    }
    return HB_RENDER_OK;
}

static int get_info(void* handle, int* width, int* height, double* fps) {
    (void)handle;
    *width = 1280;
    *height = 720;
    *fps = 30.0;
    return 0;
}

static void close_source(void* handle) { free(handle); }

HB_PLUGIN_EXPORT int hb_plugin_get_v1(hb_plugin_v1* out) {
    if (!out || out->struct_size < sizeof(hb_plugin_v1)) return -1;
    out->list_sources = list_sources;
    out->open_source = open_source;
    out->render = render;
    out->get_info = get_info;
    out->close_source = close_source;
    out->shutdown = NULL;
    return 0;
}
