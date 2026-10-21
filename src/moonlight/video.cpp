#include "moonlight/video.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <Limelight.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "casement/log.h"
#include "casement/text.h"
#include "moonlight/hresult.h"

namespace moonlight {

namespace {

/// A COM reference.
template <class T>
class Com {
public:
    Com() = default;
    Com(const Com& o) : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    Com(Com&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    ~Com() { reset(); }
    Com& operator=(Com o) noexcept {
        std::swap(p_, o.p_);
        return *this;
    }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() {
        reset();
        return &p_;
    }
    explicit operator bool() const { return p_ != nullptr; }
    template <class U>
    Com<U> as() const {
        Com<U> u;
        if (p_) p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(u.put()));
        return u;
    }

private:
    T* p_ = nullptr;
};

// D3D11_DECODER_PROFILE_H264_VLD_NOFGT and D3D11_DECODER_PROFILE_HEVC_VLD_MAIN,
// spelled out so no GUID library is needed.
constexpr GUID profile_h264 = {0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
constexpr GUID profile_hevc = {0x5b11d51b, 0x2f4c, 0x4452, {0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}};

bool software_forced() {
    char v[32];
    DWORD n = GetEnvironmentVariableA("CASEMENT_MOONLIGHT_DECODER", v, sizeof(v));
    return n > 0 && n < sizeof(v) && _stricmp(v, "software") == 0;
}

/// A D3D11 device for decoding and converting, on the default adapter.
Com<ID3D11Device> create_device() {
    Com<ID3D11Device> device;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.put(),
                                   nullptr, nullptr);
    if (FAILED(hr)) {
        casement::log::info("no D3D11 video device: {}", hresult_text(hr));
        device.reset();
    }
    return device;
}

std::string adapter_name(const Com<ID3D11Device>& device) {
    auto dxgi = device.as<IDXGIDevice>();
    Com<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (dxgi && SUCCEEDED(dxgi->GetAdapter(adapter.put())) && SUCCEEDED(adapter->GetDesc(&desc)))
        return casement::narrow(desc.Description);
    return "unknown GPU";
}

/// Converts decoded NV12 textures to BGRA with the GPU's video processor
/// and brings the result to the CPU through a staging texture.
class VideoProcessor {
public:
    bool init(ID3D11Device* device, int width, int height) {
        device->AddRef();
        *device_.put() = device;
        device->GetImmediateContext(context_.put());
        video_device_ = device_.as<ID3D11VideoDevice>();
        video_context_ = context_.as<ID3D11VideoContext>();
        if (!video_device_ || !video_context_) return false;
        width_ = width;
        height_ = height;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = static_cast<UINT>(width);
        content.InputHeight = static_cast<UINT>(height);
        content.OutputWidth = static_cast<UINT>(width);
        content.OutputHeight = static_cast<UINT>(height);
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(video_device_->CreateVideoProcessorEnumerator(&content, enumerator_.put()))) return false;
        UINT in_flags = 0, out_flags = 0;
        if (FAILED(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &in_flags)) ||
            !(in_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
            FAILED(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &out_flags)) ||
            !(out_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
            return false;
        if (FAILED(video_device_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()))) return false;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, output_.put()))) return false;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, staging_.put()))) return false;

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(video_device_->CreateVideoProcessorOutputView(output_.get(), enumerator_.get(), &view,
                                                                 output_view_.put())))
            return false;

        // Only colour conversion: no automatic enhancements.
        video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.get(), 0, FALSE);
        video_context_->VideoProcessorSetStreamFrameFormat(processor_.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
        out.RGB_Range = 0;  // full range RGB
        video_context_->VideoProcessorSetOutputColorSpace(processor_.get(), &out);
        return true;
    }

    /// Converts one decoded frame and hands its rows to `sink`.
    bool convert(const AVFrame* frame, FrameSink& sink) {
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        auto slice = static_cast<UINT>(reinterpret_cast<std::intptr_t>(frame->data[1]));
        if (!texture) return false;
        if (texture != texture_.get()) {
            // A new texture pool: views of the old one are no good.
            inputs_.clear();
            texture->AddRef();
            *texture_.put() = texture;
        }
        if (inputs_.size() <= slice) inputs_.resize(slice + 1);
        if (!inputs_[slice]) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
            desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            desc.Texture2D.ArraySlice = slice;
            if (FAILED(video_device_->CreateVideoProcessorInputView(texture, enumerator_.get(), &desc,
                                                                    inputs_[slice].put())))
                return false;
        }

        // Rec. 709 unless the stream says Rec. 601: the plugin asks for 709.
        bool bt709 = frame->colorspace != AVCOL_SPC_BT470BG && frame->colorspace != AVCOL_SPC_SMPTE170M;
        bool full = frame->color_range == AVCOL_RANGE_JPEG;
        if (bt709 != bt709_ || full != full_ || !colour_set_) {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
            in.YCbCr_Matrix = bt709 ? 1 : 0;
            in.Nominal_Range =
                full ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            video_context_->VideoProcessorSetStreamColorSpace(processor_.get(), 0, &in);
            bt709_ = bt709;
            full_ = full;
            colour_set_ = true;
        }
        RECT source{0, 0, std::min(frame->width, width_), std::min(frame->height, height_)};
        video_context_->VideoProcessorSetStreamSourceRect(processor_.get(), 0, TRUE, &source);

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = inputs_[slice].get();
        HRESULT hr = video_context_->VideoProcessorBlt(processor_.get(), output_view_.get(), 0, 1, &stream);
        if (FAILED(hr)) {
            casement::log::warn("video processor: {}", hresult_text(hr));
            return false;
        }
        context_->CopyResource(staging_.get(), output_.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        // Waits for the GPU; the decoder may reuse the frame once this returns.
        hr = context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            casement::log::warn("reading the picture: {}", hresult_text(hr));
            return false;
        }
        sink.write_frame([&](std::uint8_t* bgrx, std::size_t stride) {
            const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
            std::size_t row = std::size_t(width_) * 4;
            for (int y = 0; y < height_; ++y)
                std::memcpy(bgrx + std::size_t(y) * stride, src + std::size_t(y) * mapped.RowPitch, row);
        });
        context_->Unmap(staging_.get(), 0);
        return true;
    }

private:
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11VideoDevice> video_device_;
    Com<ID3D11VideoContext> video_context_;
    Com<ID3D11VideoProcessorEnumerator> enumerator_;
    Com<ID3D11VideoProcessor> processor_;
    Com<ID3D11Texture2D> output_;
    Com<ID3D11Texture2D> staging_;
    Com<ID3D11VideoProcessorOutputView> output_view_;
    Com<ID3D11Texture2D> texture_;
    std::vector<Com<ID3D11VideoProcessorInputView>> inputs_;
    int width_ = 0;
    int height_ = 0;
    bool bt709_ = false;
    bool full_ = false;
    bool colour_set_ = false;
};

std::string av_error(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

int sws_colorspace(const AVFrame* f) {
    switch (f->colorspace) {
        case AVCOL_SPC_BT709: return SWS_CS_ITU709;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
        default:
            // The plugin asks for Rec. 709; hosts that don't say use it too.
            return SWS_CS_ITU709;
    }
}

// moonlight-qt resets the decoder after this many failures in a row.
constexpr int failures_before_reset = 20;

}  // namespace

HardwareDecoding probe_hardware_decoding() {
    HardwareDecoding hw;
    if (software_forced()) {
        casement::log::info("software decoding (CASEMENT_MOONLIGHT_DECODER=software)");
        return hw;
    }
    Com<ID3D11Device> device = create_device();
    if (!device) return hw;
    auto video = device.as<ID3D11VideoDevice>();
    if (!video) {
        casement::log::info("the D3D11 device has no video decoder");
        return hw;
    }
    hw.adapter = adapter_name(device);
    UINT count = video->GetVideoDecoderProfileCount();
    for (UINT i = 0; i < count; ++i) {
        GUID profile{};
        if (FAILED(video->GetVideoDecoderProfile(i, &profile))) continue;
        bool* which = nullptr;
        if (IsEqualGUID(profile, profile_h264)) which = &hw.h264;
        if (IsEqualGUID(profile, profile_hevc)) which = &hw.hevc;
        if (!which) continue;
        BOOL supported = FALSE;
        if (SUCCEEDED(video->CheckVideoDecoderFormat(&profile, DXGI_FORMAT_NV12, &supported)) && supported)
            *which = true;
    }
    casement::log::info("{}: D3D11VA H.264 {}, HEVC {}", hw.adapter, hw.h264 ? "yes" : "no", hw.hevc ? "yes" : "no");
    return hw;
}

struct VideoDecoder::Impl {
    explicit Impl(FrameSink& s, bool hw) : sink(s), allow_hardware(hw) {}

    FrameSink& sink;
    bool allow_hardware;
    int format = 0;
    int width = 0;
    int height = 0;
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* sw_frame = nullptr;
    SwsContext* sws = nullptr;
    AVBufferRef* hw_device = nullptr;
    Com<ID3D11Device> device;
    std::unique_ptr<VideoProcessor> processor;
    bool processor_failed = false;
    bool hardware = false;
    std::vector<std::uint8_t> buffer;
    int failures = 0;
    std::atomic<std::uint64_t> frames{0};

    static AVPixelFormat get_format(AVCodecContext* c, const AVPixelFormat* formats) {
        auto* self = static_cast<Impl*>(c->opaque);
        if (self->hw_device) {
            for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p)
                if (*p == AV_PIX_FMT_D3D11) return *p;
        }
        // FFmpeg asks again without the hardware format when its set-up
        // fails; decode in software then.
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
            if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
                if (self->hardware) casement::log::warn("hardware decoding failed to start; decoding in software");
                self->hardware = false;
                return *p;
            }
        }
        return AV_PIX_FMT_NONE;
    }

    bool start_hardware() {
        device = create_device();
        if (!device) return false;
        hw_device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hw_device) return false;
        auto* d3d = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hw_device->data)->hwctx);
        d3d->device = device.get();
        device->AddRef();  // FFmpeg releases its reference
        if (int r = av_hwdevice_ctx_init(hw_device); r < 0) {
            casement::log::warn("D3D11VA: {}", av_error(r));
            av_buffer_unref(&hw_device);
            return false;
        }
        return true;
    }

    bool open(bool use_hardware) {
        close_decoder();
        ctx = avcodec_alloc_context3(codec);
        if (!ctx) return false;
        // As Moonlight: no frame delay, show what can be decoded, and report
        // errors so the host sends a key frame.
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_OUTPUT_CORRUPT;
        ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
        ctx->err_recognition = AV_EF_EXPLODE;
        ctx->width = width;
        ctx->height = height;
        ctx->pkt_timebase = AVRational{1, 90000};
        ctx->opaque = this;
        ctx->get_format = get_format;
        hardware = use_hardware && start_hardware();
        if (hardware) {
            ctx->hw_device_ctx = av_buffer_ref(hw_device);
            ctx->thread_count = 1;
        } else {
            ctx->thread_type = FF_THREAD_SLICE;
            ctx->thread_count = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
        }
        if (int r = avcodec_open2(ctx, codec, nullptr); r < 0) {
            casement::log::error("can't open the {} decoder: {}", codec->name, av_error(r));
            return false;
        }
        casement::log::info("decoding {} {}x{} in {}", codec->name, width, height,
                            hardware ? "hardware (D3D11VA)" : "software");
        return true;
    }

    void close_decoder() {
        avcodec_free_context(&ctx);
        av_buffer_unref(&hw_device);
        processor.reset();
        processor_failed = false;
        device.reset();
    }

    void convert_software(const AVFrame* f) {
        sws = sws_getCachedContext(sws, f->width, f->height, static_cast<AVPixelFormat>(f->format), width, height,
                                   AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            casement::log::error("can't convert {} pictures",
                                 av_get_pix_fmt_name(static_cast<AVPixelFormat>(f->format)));
            return;
        }
        int full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
        sws_setColorspaceDetails(sws, sws_getCoefficients(sws_colorspace(f)), full, sws_getCoefficients(SWS_CS_DEFAULT),
                                 1, 0, 1 << 16, 1 << 16);
        sink.write_frame([&](std::uint8_t* bgrx, std::size_t stride) {
            std::uint8_t* dst[4] = {bgrx, nullptr, nullptr, nullptr};
            int dst_stride[4] = {static_cast<int>(stride), 0, 0, 0};
            sws_scale(sws, f->data, f->linesize, 0, f->height, dst, dst_stride);
        });
    }

    void present(AVFrame* f) {
        ++frames;
        if (f->width != width || f->height != height) {
            casement::log::info("the stream is now {}x{}", f->width, f->height);
            width = f->width;
            height = f->height;
            processor.reset();
            processor_failed = false;
            sink.set_size(width, height);
        }
        if (f->format == AV_PIX_FMT_D3D11) {
            if (!processor && !processor_failed) {
                processor = std::make_unique<VideoProcessor>();
                if (!processor->init(device.get(), width, height)) {
                    casement::log::warn("no D3D11 video processor; converting on the CPU");
                    processor.reset();
                    processor_failed = true;
                }
            }
            if (processor && processor->convert(f, sink)) return;
            av_frame_unref(sw_frame);
            if (int r = av_hwframe_transfer_data(sw_frame, f, 0); r < 0) {
                casement::log::warn("can't read the decoded picture: {}", av_error(r));
                return;
            }
            convert_software(sw_frame);
            av_frame_unref(sw_frame);
            return;
        }
        convert_software(f);
    }

    int failed(const char* what, int err) {
        casement::log::warn("{}: {}", what, av_error(err));
        if (++failures >= failures_before_reset) {
            // The decoder is in a bad state: start a new one, in software if
            // the hardware one keeps failing.
            casement::log::warn("resetting the decoder after {} failures", failures);
            bool was_hardware = hardware;
            open(false);
            if (was_hardware) casement::log::warn("continuing with software decoding");
            failures = 0;
        }
        return DR_NEED_IDR;
    }
};

VideoDecoder::VideoDecoder(FrameSink& sink, bool allow_hardware)
    : impl_(std::make_unique<Impl>(sink, allow_hardware)) {}

VideoDecoder::~VideoDecoder() { cleanup(); }

bool VideoDecoder::setup(int video_format, int width, int height) {
    Impl& d = *impl_;
    d.format = video_format;
    d.width = width;
    d.height = height;
    if (video_format & VIDEO_FORMAT_MASK_H265) d.codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    else if (video_format & VIDEO_FORMAT_MASK_H264) d.codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    else d.codec = nullptr;
    if (!d.codec) {
        casement::log::error("no decoder for video format {:#x}", video_format);
        return false;
    }
    d.packet = av_packet_alloc();
    d.frame = av_frame_alloc();
    d.sw_frame = av_frame_alloc();
    if (!d.packet || !d.frame || !d.sw_frame) return false;
    if (!d.open(d.allow_hardware) && !(d.allow_hardware && d.open(false))) return false;
    d.sink.set_size(width, height);
    return true;
}

int VideoDecoder::submit(_DECODE_UNIT* unit) {
    Impl& d = *impl_;
    if (!d.ctx) return DR_NEED_IDR;
    auto size = static_cast<std::size_t>(unit->fullLength);
    d.buffer.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);
    std::size_t offset = 0;
    for (PLENTRY e = unit->bufferList; e; e = e->next) {
        auto n = static_cast<std::size_t>(e->length);
        if (offset + n > size) break;
        std::memcpy(d.buffer.data() + offset, e->data, n);
        offset += n;
    }
    std::memset(d.buffer.data() + offset, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    d.packet->data = d.buffer.data();
    d.packet->size = static_cast<int>(offset);
    d.packet->flags = unit->frameType == FRAME_TYPE_IDR ? AV_PKT_FLAG_KEY : 0;

    if (int r = avcodec_send_packet(d.ctx, d.packet); r < 0) return d.failed("decoding a frame", r);
    for (;;) {
        int r = avcodec_receive_frame(d.ctx, d.frame);
        if (r == AVERROR(EAGAIN)) break;
        if (r < 0) return d.failed("decoding a frame", r);
        d.failures = 0;
        d.present(d.frame);
        av_frame_unref(d.frame);
    }
    return DR_OK;
}

void VideoDecoder::cleanup() {
    Impl& d = *impl_;
    d.close_decoder();
    av_packet_free(&d.packet);
    av_frame_free(&d.frame);
    av_frame_free(&d.sw_frame);
    sws_freeContext(d.sws);
    d.sws = nullptr;
}

bool VideoDecoder::hardware() const { return impl_->hardware; }

std::uint64_t VideoDecoder::frames() const { return impl_->frames.load(); }

}  // namespace moonlight
