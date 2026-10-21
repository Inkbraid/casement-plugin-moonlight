// The stream's sound: Opus packets decoded with libopus and played through
// WASAPI in shared mode on the default output device, following the
// default when the user changes it.

#pragma once

#include <memory>

struct _OPUS_MULTISTREAM_CONFIGURATION;

namespace moonlight {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    /// moonlight-common-c's init(): the negotiated Opus layout. Returns 0;
    /// a missing output device only means silence.
    int init(const _OPUS_MULTISTREAM_CONFIGURATION& config);
    /// decodeAndPlaySample(), on the library's audio thread. A null packet
    /// asks for packet loss concealment.
    void play(const char* data, int length);
    void cleanup();

    /// From any thread: muted output stops, the stream keeps running.
    void set_muted(bool muted);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moonlight
