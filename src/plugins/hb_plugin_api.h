/*
 * HB Studio native plugin API, version 1  (plain C, stable ABI).
 *
 * A "source" plugin adds new video inputs (for example a capture card SDK, NDI, a network
 * protocol...). It is a shared library that exports ONE function:
 *
 *     HB_PLUGIN_EXPORT int hb_plugin_get_v1(hb_plugin_v1* out);
 *
 * The host zero-fills `out` and sets out->struct_size and out->api_version before the call; the
 * plugin fills in the function pointers and returns 0. Everything is C, no C++ types cross the
 * boundary, no exceptions may escape.
 *
 * Threading: the host serialises every call that touches the same opened source, but different
 * sources (and list_sources) may be called from different threads. render() is called on the
 * real-time path (Program clock thread / UI preview thread): it MUST return quickly (a few
 * milliseconds). Do the slow work (network, decoding) on your own thread and let render() only
 * scale the newest frame. A source whose render() is repeatedly slow or fails is disabled.
 *
 * Plugin code runs inside the HB Studio process with its privileges: only install plugins you trust.
 */
#ifndef HB_PLUGIN_API_H
#define HB_PLUGIN_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define HB_PLUGIN_EXPORT __declspec(dllexport)
#else
#define HB_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define HB_PLUGIN_API_VERSION 1
#define HB_PLUGIN_STR_MAX 128      /* UTF-8, always NUL terminated */
#define HB_PLUGIN_MAX_SOURCES 32

typedef struct hb_source_desc {
    char id[HB_PLUGIN_STR_MAX];    /* stable id, only [A-Za-z0-9._-], at most 64 chars */
    char name[HB_PLUGIN_STR_MAX];  /* name shown to the operator */
} hb_source_desc;

#define HB_RENDER_OK 0
#define HB_RENDER_NO_SIGNAL 1      /* source open but has no picture right now */

typedef struct hb_plugin_v1 {
    uint32_t struct_size;          /* host sets sizeof(hb_plugin_v1) */
    uint32_t api_version;          /* host sets HB_PLUGIN_API_VERSION */

    /* Fills up to `max` entries, returns how many (>= 0) or < 0 on error. */
    int (*list_sources)(hb_source_desc* out, int max);

    /* Opens a source by id. Returns an opaque handle or NULL. */
    void* (*open_source)(const char* id);

    /* Writes the newest frame, scaled to width x height, as BGRA8 into `bgra`
     * (`stride` bytes per row, buffer holds stride*height bytes).
     * Returns HB_RENDER_OK, HB_RENDER_NO_SIGNAL, or < 0 for an error. */
    int (*render)(void* source, uint8_t* bgra, int width, int height, int stride, uint64_t index);

    /* Optional (may be NULL): native size and frame rate, for the operator's information. */
    int (*get_info)(void* source, int* width, int* height, double* fps);

    void (*close_source)(void* source);

    /* Optional (may be NULL): called once before the library is unloaded / the app exits. */
    void (*shutdown)(void);
} hb_plugin_v1;

HB_PLUGIN_EXPORT int hb_plugin_get_v1(hb_plugin_v1* out);

typedef int (*hb_plugin_get_v1_fn)(hb_plugin_v1* out);

/* ------------------------------------------------------------------------------------------
 * Audio effect plugins ("type": "effect"). A library exports ONE function:
 *
 *     HB_PLUGIN_EXPORT int hb_plugin_get_effects_v1(hb_effects_v1* out);
 *
 * Audio is float32, INTERLEAVED, nominal range [-1, 1], processed IN PLACE in blocks (typically 10 ms).
 * process() runs on the real-time audio thread: no allocation, no locks, no I/O, never block.
 * An instance is only ever used by one thread at a time.
 * ------------------------------------------------------------------------------------------ */
#define HB_EFFECT_MAX_PARAMS 32

typedef struct hb_effect_param {
    char id[32];                    /* [a-z0-9_] */
    char label[HB_PLUGIN_STR_MAX];  /* shown to the operator */
    char unit[16];                  /* "dB", "Hz", "ms", "%", "" */
    float min_value, max_value, default_value;
    int logarithmic;                /* 1: the UI slider is logarithmic (frequencies) */
} hb_effect_param;

typedef struct hb_effect_desc {
    char type_id[64];               /* stable, [A-Za-z0-9._-] */
    char name[HB_PLUGIN_STR_MAX];
    int param_count;                /* <= HB_EFFECT_MAX_PARAMS */
} hb_effect_desc;

typedef struct hb_effects_v1 {
    uint32_t struct_size;           /* host sets sizeof(hb_effects_v1) */
    uint32_t api_version;           /* host sets HB_PLUGIN_API_VERSION */
    int (*count)(void);                                              /* number of effect types */
    int (*describe)(int index, hb_effect_desc* out, hb_effect_param* params, int max_params); /* 0 = ok */
    void* (*create)(int index, int sample_rate, int channels);       /* instance or NULL */
    void (*set_param)(void* instance, int param_index, float value); /* any thread; must be real-time safe */
    void (*process)(void* instance, float* interleaved, int frames); /* in place */
    void (*reset)(void* instance);                                   /* clear internal state */
    void (*destroy)(void* instance);
    void (*shutdown)(void);                                          /* optional */
} hb_effects_v1;

HB_PLUGIN_EXPORT int hb_plugin_get_effects_v1(hb_effects_v1* out);
typedef int (*hb_plugin_get_effects_v1_fn)(hb_effects_v1* out);

#ifdef __cplusplus
}
#endif

#endif /* HB_PLUGIN_API_H */
