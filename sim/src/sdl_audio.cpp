#include "sdl_audio.h"

#include <algorithm>
#include <cstring>

namespace sim {

SdlAudio::~SdlAudio() { stop(); }

bool SdlAudio::start() {
    if (dev_) return true;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "sim audio: SDL audio init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = int(kSampleRate);
    want.format = AUDIO_S16SYS;
    want.channels = uint8_t(kChannels);
    want.samples = uint16_t(bufferFrames_);
    want.callback = &SdlAudio::sdlCallback;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(deviceName_.empty() ? nullptr : deviceName_.c_str(), 0, &want, &have, 0);
    if (!dev_) {
        std::fprintf(stderr, "sim audio: cannot open output: %s\n", SDL_GetError());
        return false;
    }
    obtainedFrames_ = have.samples;
    if (lineInWanted_) {
        SDL_AudioSpec cw = want, ch{};
        cw.callback = &SdlAudio::captureCallback;
        cap_ = SDL_OpenAudioDevice(nullptr, 1, &cw, &ch, 0);
        if (!cap_) std::fprintf(stderr, "sim audio: no capture device: %s\n", SDL_GetError());
        else SDL_PauseAudioDevice(cap_, 0);
    }
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void SdlAudio::stop() {
    if (cap_) SDL_CloseAudioDevice(cap_);
    if (dev_) SDL_CloseAudioDevice(dev_);
    cap_ = dev_ = 0;
}

void SdlAudio::captureCallback(void* ud, Uint8* stream, int len) {
    auto* self = static_cast<SdlAudio*>(ud);
    const auto* in = reinterpret_cast<const int16_t*>(stream);
    const uint32_t frames = uint32_t(len) / 4;
    uint32_t w = self->wr_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < frames; ++i, ++w) {
        if (w - self->rd_.load(std::memory_order_acquire) >= kRing) break;   // full: drop
        self->ring_[(w % kRing) * 2] = in[i * 2];
        self->ring_[(w % kRing) * 2 + 1] = in[i * 2 + 1];
    }
    self->wr_.store(w, std::memory_order_release);
}

void SdlAudio::sdlCallback(void* ud, Uint8* stream, int len) {
    static_cast<SdlAudio*>(ud)->fill(reinterpret_cast<int16_t*>(stream), len / 4);
}

void SdlAudio::fill(int16_t* out, int frames) {
    while (frames > 0) {
        if (blockPos_ >= kBlockFrames) {
            // Next line-in block, if enough has been captured.
            lineInValid_ = false;
            if (cap_) {
                uint32_t r = rd_.load(std::memory_order_relaxed);
                if (wr_.load(std::memory_order_acquire) - r >= kBlockFrames) {
                    for (uint32_t i = 0; i < kBlockFrames; ++i, ++r) {
                        lineInBlock_[i * 2] = ring_[(r % kRing) * 2];
                        lineInBlock_[i * 2 + 1] = ring_[(r % kRing) * 2 + 1];
                    }
                    rd_.store(r, std::memory_order_release);
                    lineInValid_ = true;
                }
            }
            // The app reads lineIn() from inside the callback and mixes it
            // through the engine (level, sends, mute).
            if (fn_) fn_(block_, kBlockFrames, ctx_);
            else std::memset(block_, 0, sizeof(block_));
            blockPos_ = 0;
        }
        const uint32_t n = std::min<uint32_t>(uint32_t(frames), kBlockFrames - blockPos_);
        std::memcpy(out, block_ + blockPos_ * 2, n * 4);
        out += n * 2;
        frames -= int(n);
        blockPos_ += n;
    }
}

// ---------------------------------------------------------------------------

namespace {
void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
void wavHeader(uint8_t* h, uint32_t dataBytes) {
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + dataBytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    h[20] = 1; h[21] = 0;   // PCM
    h[22] = 2; h[23] = 0;   // stereo
    put32(h + 24, ot::IAudioOut::kSampleRate);
    put32(h + 28, ot::IAudioOut::kSampleRate * 4);
    h[32] = 4; h[33] = 0;
    h[34] = 16; h[35] = 0;
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, dataBytes);
}
}  // namespace

ManualAudio::~ManualAudio() {
    if (wav_) {
        uint8_t h[44];
        wavHeader(h, wavBytes_);
        std::fseek(wav_, 0, SEEK_SET);
        std::fwrite(h, 1, 44, wav_);
        std::fclose(wav_);
    }
}

bool ManualAudio::recordWav(const char* path) {
    wav_ = std::fopen(path, "wb");
    if (!wav_) return false;
    uint8_t h[44];
    wavHeader(h, 0);
    std::fwrite(h, 1, 44, wav_);
    return true;
}

void ManualAudio::pull() {
    int16_t buf[kBlockFrames * kChannels] = {};
    if (running_ && fn_) fn_(buf, kBlockFrames, ctx_);
    for (int16_t v : buf) {
        const int a = v < 0 ? -int(v) : int(v);
        if (a > peak) peak = a;
    }
    if (wav_) {
        std::fwrite(buf, 1, sizeof(buf), wav_);   // host is little-endian
        wavBytes_ += sizeof(buf);
    }
}

}  // namespace sim
