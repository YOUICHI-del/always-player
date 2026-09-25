#pragma once
#include "IAudioOutputBackend.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

class WasapiExclusiveOutput : public IAudioOutputBackend {
public:
    WasapiExclusiveOutput();
    ~WasapiExclusiveOutput() override;

    AudioBackendResult Initialize(const AudioFormat& format) override;
    uint32_t GetBufferSize() const override;
    AudioFormat GetActualFormat() const override;
    AudioBackendResult WriteFrames(const uint8_t* data, uint32_t frameCount) override;
    AudioBackendResult Start() override;
    AudioBackendResult Stop() override;
    void SetErrorCallback(ErrorCallback callback, void* userData) override;
    void Shutdown() override;
    const char* GetBackendName() const override { return "WASAPI Exclusive"; }

    // ===== デバッグ計測 =====
    // Stop()が呼ばれた時点で、直前のセッションの計測結果を
    // CSV(1行目=サマリ、以降=生の起床時刻ログ)に書き出す。
    // path省略時はカレントディレクトリに固定ファイル名で出力。
    void SetDiagnosticsPath(const std::wstring& path) { m_diagPath = path; }

private:
    void RenderThreadProc();
    void DumpDiagnostics();

    ComPtr<IMMDeviceEnumerator> m_enumerator;
    ComPtr<IMMDevice> m_device;
    ComPtr<IAudioClient> m_audioClient;
    ComPtr<IAudioRenderClient> m_renderClient;

    WAVEFORMATEXTENSIBLE m_waveFormat{};
    AudioFormat m_actualFormat;
    uint32_t m_bufferFrameCount = 0;
    uint32_t m_bytesPerFrame = 0;

    HANDLE m_renderEvent = nullptr;
    HANDLE m_stopEvent = nullptr;
    std::thread m_renderThread;
    std::atomic<bool> m_running{false};

    std::vector<uint8_t> m_ring;
    size_t m_ringCapacity = 0;
    std::atomic<size_t> m_writeIndex{0};
    std::atomic<size_t> m_readIndex{0};

    std::mutex m_backpressureMutex;
    std::condition_variable m_backpressureCv;

    ErrorCallback m_errorCallback = nullptr;
    void* m_errorCallbackUserData = nullptr;

    void NotifyError(AudioBackendResult error);

    // ===== デバッグ計測用メンバ =====
    // RTスレッドは新規メモリ確保をしてはいけないので、
    // 記録用バッファはInitialize()で必要なだけ事前確保しておく。
    std::atomic<uint64_t> m_underrunCount{0};
    std::atomic<uint64_t> m_totalCallbacks{0};

    // 各コールバックの「起床した瞬間」のQPCカウント値を格納する。
    // インデックスはRTスレッドだけがインクリメントする（単一書き手）。
    std::vector<int64_t> m_wakeTimestamps;
    std::atomic<size_t> m_wakeTimestampCount{0};
    size_t m_wakeTimestampCapacity = 0;

    int64_t m_qpcFrequency = 0;
    std::wstring m_diagPath;
};
