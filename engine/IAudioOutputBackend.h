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

    // ★ v10: WASAPI排他とASIOを同じように扱うための共通機能（既定は「何もしない」）。
    //   内部リングにまだ残っている（これから鳴る）フレーム数
    virtual uint32_t GetQueuedFrames() const { return 0; }
    //   デバイスと合意した有効ビット数（表示用）
    virtual uint16_t GetValidBits() const { return 24; }
    //   VUメーター用の現在レベル（RMS、0〜1）
    virtual void GetLevels(float& left, float& right) const { left = right = 0.f; }
    //   DoP出力（値をビット単位で保つ）。24bit以上の整数形式でしか開かない
    virtual void SetDopMode(bool) {}
    //   手動ビットパーフェクトで優先する有効ビット数（0=自動）
    virtual void SetPreferredBits(int) {}
};
