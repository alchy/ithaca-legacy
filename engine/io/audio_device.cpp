// engine/io/audio_device.cpp — viz audio_device.h.
#include "io/audio_device.h"

#include "miniaudio.h"            // jen hlavicka; impl je v miniaudio_impl.cpp
#include "util/log.h"

namespace ithaca {

// Trampolina: miniaudio ma_device callback → nas AudioCallback. miniaudio drzi
// pUserData = AudioDevice*, takze se pres nej dostaneme k ulozenemu callbacku.
// output je interleaved stereo float32 (device je nize nakonfigurovan na
// format f32, channels 2), takze pretypovani na float* je bezpecne.
static void ma_data_callback(ma_device* dev, void* output,
                             const void* /*input*/, ma_uint32 frame_count) {
    auto* self = static_cast<AudioDevice*>(dev->pUserData);
    if (self) self->invokeCallback(static_cast<float*>(output),
                                   (uint32_t)frame_count);
}

AudioDevice::AudioDevice() {}
AudioDevice::~AudioDevice() { stop(); }

bool AudioDevice::start(AudioCallback cb, void* userdata,
                        int sample_rate, int block_size) {
    callback_ = cb;
    userdata_ = userdata;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format   = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate        = (ma_uint32)sample_rate;
    config.periodSizeInFrames = (ma_uint32)block_size;
    config.dataCallback      = ma_data_callback;
    config.pUserData         = this;

    device_ = new ma_device();
    if (ma_device_init(nullptr, &config, device_) != MA_SUCCESS) {
        delete device_; device_ = nullptr;
        log::Logger::default_().log("audio", log::Severity::Error,
                                    "ma_device_init selhalo");
        return false;
    }
    device_name_ = device_->playback.name;
    if (ma_device_start(device_) != MA_SUCCESS) {
        ma_device_uninit(device_); delete device_; device_ = nullptr;
        log::Logger::default_().log("audio", log::Severity::Error,
                                    "ma_device_start selhalo");
        return false;
    }
    running_.store(true);
    log::Logger::default_().log("audio", log::Severity::Info,
                                "Audio start: %s SR=%d block=%d",
                                device_name_.c_str(), sample_rate, block_size);
    return true;
}

void AudioDevice::stop() {
    if (device_) {
        ma_device_uninit(device_);
        delete device_;
        device_ = nullptr;
    }
    running_.store(false);
}

} // namespace ithaca
