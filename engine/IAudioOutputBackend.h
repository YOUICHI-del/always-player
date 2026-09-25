#pragma once
#include <cstdint>
#include <cstddef>

enum class AudioSampleFormat {
    Int16,
    Int24,
    Int32,
};

struct AudioFormat {
    uint32_t sampleRate = 44100;
    uint16_t channels = 2;
    AudioSampleFormat sampleFormat = AudioSampleFormat::Int24;
};

enum class AudioBackendResult {
    Ok,
    DeviceNotFound,
    FormatNotSupported,
    ExclusiveModeUnavailable,
    DeviceLost,
    UnknownError,
};

class IAudioOutputBackend {
public:
    virtual ~IAudioOutputBackend() = default;

    virtual AudioBackendResult Initialize(const AudioFormat& format) = 0;

    // Initialize後に確定した実バッファサイズ（フレーム数）。
    // 上位層の全バッファ計算はこの値を起点にする（「真実の源」）。
    virtual uint32_t GetBufferSize() const = 0;
    virtual AudioFormat GetActualFormat() const = 0;

    // dataはinterleaved PCM（L,R,L,R,...）。frameCountはフレーム数。
    virtual AudioBackendResult WriteFrames(const uint8_t* data, uint32_t frameCount) = 0;

    virtual AudioBackendResult Start() = 0;
    virtual AudioBackendResult Stop() = 0;

    using ErrorCallback = void(*)(AudioBackendResult error, void* userData);
    virtual void SetErrorCallback(ErrorCallback callback, void* userData) = 0;

    virtual void Shutdown() = 0;
    virtual const char* GetBackendName() const = 0;
};
