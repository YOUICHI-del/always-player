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
#include <cstdio>

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

    // ★ v10: 内部リングにまだ残っている（＝これから鳴る）フレーム数。
    //   曲の終端で「本当に鳴り終わったか」を判定するために使う。
    //   PcmDualEngine側のリングが空になっても、ここにはまだ数十ms分の
    //   音が残っているため、これを待たずに次曲へ進むと曲尾が切れてしまう。
    // ★ v10診断用：直近のInitialize()で失敗したHRESULT（成功ならS_OK）
    // ★ v10: ビットパーフェクト手動指定用。次のInitialize()で優先して試す
    //   有効ビット数（16/24/32）。0なら自動（32→24→24in32→16の順）。
    void SetPreferredBits(int bits) { m_preferredBits = bits; }
    long GetLastHr() const { return m_lastHr; }
    // v10診断用：合意したコンテナ幅／有効ビット数
    uint16_t GetContainerBits() const { return m_containerBits; }
    uint16_t GetValidBits() const { return m_validBits; }
    long long GetDefaultPeriod100ns() const { return m_defaultPeriod; }
    long long GetMinPeriod100ns() const { return m_minPeriod; }

    // ★ v10: VUメーター用。今まさにDACへ渡された音のレベル（RMS、0〜1）。
    //   排他モードの音はWindowsのループバックでは取れないため、エンジン自身が
    //   測る。WriteFrames()でチャンクごとに測った値を、RTスレッドがその
    //   チャンクをデバイスへ渡した瞬間に「現在値」として公開するので、
    //   表示が実際の音と揃う。
    void GetLevels(float& left, float& right) const {
        left  = m_levelL.load(std::memory_order_relaxed);
        right = m_levelR.load(std::memory_order_relaxed);
    }

    uint32_t GetQueuedFrames() const {
        if (m_bytesPerFrame == 0 || m_ringCapacity == 0) return 0;
        const size_t w = m_writeIndex.load(std::memory_order_acquire);
        const size_t r = m_readIndex.load(std::memory_order_acquire);
        const size_t bytes = (w + m_ringCapacity - r) % m_ringCapacity;
        return static_cast<uint32_t>(bytes / m_bytesPerFrame);
    }

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
    long m_lastHr = 0;
    // v10: デバイスと合意したコンテナ幅／有効ビット数と、詰め直し用バッファ
    uint16_t m_containerBits = 32;
    uint16_t m_validBits = 32;
    int m_preferredBits = 0;
    long long m_defaultPeriod = 0;
    long long m_minPeriod = 0;
    std::vector<uint8_t> m_convBuf;

    // v10: VUメーター用レベルFIFO（単一生産者＝WriteFrames、単一消費者＝RTスレッド）
    struct LevelEntry { uint64_t endByte; float l; float r; };
    static constexpr size_t kLevelFifoSize = 64;
    LevelEntry m_levelFifo[kLevelFifoSize] = {};
    std::atomic<size_t> m_levelHead{0};   // 生産者が進める
    std::atomic<size_t> m_levelTail{0};   // 消費者が進める
    uint64_t m_bytesWrittenTotal = 0;     // WriteFrames側だけが触る
    uint64_t m_bytesReadTotal = 0;        // RTスレッド側だけが触る
    std::atomic<float> m_levelL{0.f};
    std::atomic<float> m_levelR{0.f};
    std::wstring m_diagPath;
};
