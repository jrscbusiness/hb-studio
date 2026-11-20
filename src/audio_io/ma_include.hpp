#pragma once
// Single place where miniaudio is configured. Every translation unit that needs the miniaudio
// declarations must include this header (and never <miniaudio.h> directly) so that all of them
// see the same feature macros as the one that holds the implementation (ma_impl.cpp).
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include <miniaudio.h>
