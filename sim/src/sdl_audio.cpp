#include "sdl_audio.h"

#include <algorithm>
#include <cstring>

namespace sim {

SdlAudio::~SdlAudio() { stop(); }

// [I] USB in (#97) usually wants a "Monitor of ..." source (the computer's
// own output). SDL lists those only through its PulseAudio backend (PipeWire
// serves it too) and with the monitors hint; without PulseAudio, the default
// backend.
static bool initAudioWithMonitors() {
    if (SDL_WasInit(SDL_INIT_AUDIO)) return true;
    SDL_SetHint("SDL_AUDIO_INCLUDE_MONITORS", "1");
    if (!SDL_getenv("SDL_AUDIODRIVER") && SDL_AudioInit("pulseaudio") == 0) {
        SDL_AudioQuit();
        SDL_SetHint("SDL_AUDIODRIVER", "pulseaudio");
    }
    return SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
}

bool SdlAudio::start() {
    if (dev_) return true;
    const bool ok = usb_.wanted ? initAudioWithMonitors()
                                : SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
    if (!ok) {
        std::fprintf(stderr, "sim audio: SDL audio init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = int(kSampleRate);
    want.format = AUDIO_S16SYS;
    want.channels = uint8_t(kChannels);
    want.samples = 256;
    want.callback = &SdlAudio::sdlCallback;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev_) {
        std::fprintf(stderr, "sim audio: cannot open output: %s\n", SDL_GetError());
        return false;
    }
    if (line_.wanted) line_.open(want, "line in");
    if (usb_.wanted) usb_.open(want, "USB in");
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void SdlAudio::stop() {
    for (Capture* c : {&line_, &usb_}) {
        if (c->dev) SDL_CloseAudioDevice(c->dev);
        c->dev = 0;
    }
    if (dev_) SDL_CloseAudioDevice(dev_);
    dev_ = 0;
}

void SdlAudio::listCaptureDevices() {
    if (!initAudioWithMonitors()) {
        std::fprintf(stderr, "sim audio: SDL audio init failed: %s\n", SDL_GetError());
        return;
    }
    const int n = SDL_GetNumAudioDevices(1);
    for (int i = 0; i < n; ++i) std::printf("%s\n", SDL_GetAudioDeviceName(i, 1));
}

void SdlAudio::Capture::open(const SDL_AudioSpec& want, const char* what) {
    SDL_AudioSpec cw = want, ch{};
    cw.callback = &SdlAudio::captureCallback;
    cw.userdata = this;
    dev = SDL_OpenAudioDevice(name.empty() ? nullptr : name.c_str(), 1, &cw, &ch, 0);
    if (!dev) {
        std::fprintf(stderr, "sim audio: no capture device for %s (%s): %s\n", what,
                     name.empty() ? "default" : name.c_str(), SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(dev, 0);
}

void SdlAudio::Capture::next() {
    valid = false;
    if (!dev) return;
    uint32_t r = rd.load(std::memory_order_relaxed);
    if (wr.load(std::memory_order_acquire) - r < kBlockFrames) return;
    for (uint32_t i = 0; i < kBlockFrames; ++i, ++r) {
        block[i * 2] = ring[(r % kRing) * 2];
        block[i * 2 + 1] = ring[(r % kRing) * 2 + 1];
    }
    rd.store(r, std::memory_order_release);
    valid = true;
}

void SdlAudio::captureCallback(void* ud, Uint8* stream, int len) {
    auto* self = static_cast<Capture*>(ud);
    const auto* in = reinterpret_cast<const int16_t*>(stream);
    const uint32_t frames = uint32_t(len) / 4;
    uint32_t w = self->wr.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < frames; ++i, ++w) {
        if (w - self->rd.load(std::memory_order_acquire) >= Capture::kRing) break;   // full: drop
        self->ring[(w % Capture::kRing) * 2] = in[i * 2];
        self->ring[(w % Capture::kRing) * 2 + 1] = in[i * 2 + 1];
    }
    self->wr.store(w, std::memory_order_release);
}

void SdlAudio::sdlCallback(void* ud, Uint8* stream, int len) {
    static_cast<SdlAudio*>(ud)->fill(reinterpret_cast<int16_t*>(stream), len / 4);
}

void SdlAudio::fill(int16_t* out, int frames) {
    while (frames > 0) {
        if (blockPos_ >= kBlockFrames) {
            // Next line-in / USB-in block, if enough has been captured.
            line_.next();
            usb_.next();
            // The app reads lineIn() / usbIn() from inside the callback and
            // mixes them through the engine (level, sends, mute).
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
