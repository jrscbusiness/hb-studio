// miniaudio implementation: compiled once, with warnings disabled by the build fragment
// (third-party code, MIT, see LICENSES.md).
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "audio_io/ma_include.hpp"
