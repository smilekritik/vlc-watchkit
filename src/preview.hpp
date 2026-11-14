#pragma once

#include <string>

struct vout_thread_t;

namespace addon {
// Consumes a held reference to the video output.
std::string upload_video_frame(vout_thread_t* output, const std::wstring& endpoint);
}
