#pragma once
// engine/io/audio_device.h
// ------------------------
// miniaudio playback wrapper. Forward-declaruje ma_device (zadna miniaudio
// dependency v hlavicce). Caller dodava callback, ktery plni interleaved
// stereo float32 buffer. Adaptovano z icr2 player/audio_device.

#include <atomic>
#include <cstdint>
#include <string>

struct ma_device;

namespace ithaca {

// Callback: naplni `output` (interleaved stereo float32) `frames` framy.
using AudioCallback = void(*)(void* userdata, float* output, uint32_t frames);

class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Init + start. Vrati false on failure.
    bool start(AudioCallback cb, void* userdata, int sample_rate, int block_size);
    void stop();

    bool isRunning() const { return running_.load(); }
    const std::string& deviceName() const { return device_name_; }

    // Interni: vola ulozeny callback. Verejne jen kvuli C-trampoline v .cpp
    // (miniaudio callback je free funkce a potrebuje se dostat k callback_).
    void invokeCallback(float* output, uint32_t frames) {
        if (callback_) callback_(userdata_, output, frames);
    }

private:
    ma_device*        device_   = nullptr;
    std::atomic<bool> running_  {false};
    AudioCallback     callback_ = nullptr;
    void*             userdata_ = nullptr;
    std::string       device_name_;
};

} // namespace ithaca
