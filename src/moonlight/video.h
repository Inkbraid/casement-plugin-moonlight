// Decoding the stream's H.264 or HEVC into BGRX pictures.
//
// The preferred path is D3D11VA: FFmpeg decodes on the GPU into NV12
// textures, a D3D11 video processor converts them to BGRA, and a staging
// texture brings the picture to the CPU for Casement's surface. Without a
// usable GPU decoder FFmpeg decodes in software and swscale converts.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "moonlight/frames.h"

struct _DECODE_UNIT;

namespace moonlight {

/// Which codecs this computer's GPU can decode through D3D11VA.
struct HardwareDecoding {
    bool h264 = false;
    bool hevc = false;
    std::string adapter;  // the GPU's name, for the log
};

/// Asks the GPU driver; nothing when D3D11 video isn't available (as under
/// Wine) or CASEMENT_MOONLIGHT_DECODER=software.
HardwareDecoding probe_hardware_decoding();

class VideoDecoder {
public:
    explicit VideoDecoder(FrameSink& sink, bool allow_hardware);
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /// moonlight-common-c's setup(): VIDEO_FORMAT_*, the stream's size.
    bool setup(int video_format, int width, int height);
    /// submitDecodeUnit(): DR_OK, or DR_NEED_IDR when decoding failed.
    int submit(_DECODE_UNIT* unit);
    void cleanup();

    bool hardware() const;
    std::uint64_t frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moonlight
