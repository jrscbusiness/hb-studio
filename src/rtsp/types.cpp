#include "rtsp/types.hpp"
namespace hb::rtsp {
const char* codecName(Codec c) { return c == Codec::H264 ? "H264" : "H265"; }
}  // namespace hb::rtsp
