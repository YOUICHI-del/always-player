#pragma once
// ─────────────────────────────────────────────────────────
// AsioOutput
//
// ★ v10: ASIO出力。IAudioOutputBackendの実装として、WasapiExclusiveOutputと
//   同じ使い方（Initialize → Start → WriteFrames … → Stop → Shutdown）で動く。
//
// ・上位（AudioProcessThread）からは左詰めint32(interleaved L,R)で届く。
//   WriteFrames()はそれをロックフリーのリングへ積むだけ。
// ・ドライバのコールバック(bufferSwitch)がリングから取り出し、ドライバが
//   求める形式（Int32/24/16、float等）へ変換してASIOバッファへ書く。
//   コールバック内ではロック・メモリ確保・ファイルI/Oは一切しない。
// ・ドライバの読み込み（COM）は重いので、Shutdown()ではバッファの解放だけ行い、
//   ドライバ自体は保持する（曲ごとに読み込み直さない）。完全に解放するのはUnload()。
// ・ASIOはプロセス内で同時に1つのドライバしか使えない仕様（コールバックに
//   ユーザーデータを渡せないため、アクティブなインスタンスを1つだけ記録する）。
// ・ASIO SDK 2.3.4 は Steinberg ASIO License / GPLv3 のデュアルライセンス。
//   Always Player は GPLv3 を選択して利用する。
// ─────────────────────────────────────────────────────────
#include "IAudioOutputBackend.h"
#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <cstdint>

struct IASIO;   // iasiodrv.h（cpp側でのみinclude）

class AsioOutput : public IAudioOutputBackend {
public:
    AsioOutput();
    ~AsioOutput() override;

    // HKLM\SOFTWARE\ASIO に登録されているドライバ名の一覧
    static std::vector<std::wstring> EnumerateDrivers();

    void SetDriverName(const std::wstring& name);
    const std::wstring& DriverName() const { return m_driverName; }
    // ドライバのinit()に渡すウィンドウハンドル（未指定ならデスクトップ）
    void SetSysHandle(void* hwnd) { m_sysHandle = hwnd; }

    AudioBackendResult Initialize(const AudioFormat& format) override;
    uint32_t GetBufferSize() const override { return m_bufferFrames; }
    AudioFormat GetActualFormat() const override { return m_actualFormat; }
    AudioBackendResult WriteFrames(const uint8_t* data, uint32_t frameCount) override;
    AudioBackendResult Start() override;
    AudioBackendResult Stop() override;
    void SetErrorCallback(ErrorCallback callback, void* userData) override;
    void Shutdown() override;
    const char* GetBackendName() const override { return "ASIO"; }

    uint32_t GetQueuedFrames() const override;
    uint16_t GetValidBits() const override { return m_validBits; }
    void GetLevels(float& left, float& right) const override {
        left  = m_levelL.load(std::memory_order_relaxed);
        right = m_levelR.load(std::memory_order_relaxed);
    }
    // DoP：値を1bitも変えてはいけないので、16bitやfloatのドライバでは開かない。
    void SetDopMode(bool on) override { m_dop = on; }
    // ★ v10: ネイティブDSD。次のInitialize()でドライバをDSDモードにする。
    //   Initialize()のsampleRateは「DSDレート/8」（1フレーム＝各ch 1バイト）で渡す。
    void SetNativeDsdMode(bool on) { m_nativeDsd = on; }
    bool IsNativeDsdMode() const { return m_nativeDsd; }
    // このドライバがネイティブDSDに対応しているか（ドライバを読み込んで問い合わせる）
    bool SupportsNativeDsd();
    void SetPreferredBits(int) override {}   // ASIOはドライバが形式を決める

    // ドライバ付属の設定画面（バッファサイズ等）を開く
    bool ControlPanel();
    // ドライバを完全に解放する（出力方式の切替時・終了時）
    void Unload();
    // ドライバから「設定が変わったのでリセットしてほしい」と要求されたか（読むとクリア）
    bool ConsumeResetRequest() { return m_resetRequested.exchange(false); }
    // 直近の失敗理由（ドライバのエラーメッセージ等、表示用）
    const std::string& LastError() const { return m_lastError; }
    // ★ v10: 診断。Start()以降にドライバからバッファ要求（コールバック）が来た回数と、
    //   データが足りず無音で埋めた回数。0のままならドライバが動いていない（出力先が無効等）。
    uint64_t CallbackCount() const { return m_callbackCount.load(std::memory_order_relaxed); }
    uint64_t UnderrunCount() const { return m_underrunCount.load(std::memory_order_relaxed); }
    // 診断ログの出力先（空なら書かない）。開発中の確認用。
    void SetLogPath(const std::wstring& path) { m_logPath = path; }
    void Log(const std::string& line);

    // ── 以下はASIOコールバック（ドライバのスレッド）から呼ばれる内部用 ──
    void OnBufferSwitch(long index);
    void OnResetRequest() { m_resetRequested.store(true); }

private:
    bool EnsureLoaded();
    void DisposeBuffers();

    IASIO*       m_asio = nullptr;
    std::wstring m_driverName;
    std::wstring m_loadedName;
    void*        m_sysHandle = nullptr;
    bool         m_comInitialized = false;

    // ASIOバッファ（L/R の2ch、ダブルバッファ）
    void*   m_buffers[2][2] = {};   // [ch][index]
    bool    m_buffersCreated = false;
    long    m_sampleType = -1;      // ASIOSampleType
    bool    m_postOutput = false;   // outputReady()を呼ぶドライバか
    uint32_t m_bufferFrames = 0;
    uint16_t m_validBits = 24;
    AudioFormat m_actualFormat;
    bool    m_dop = false;
    bool    m_nativeDsd = false;     // v10: 次のInitialize()でDSDモードにするか
    bool    m_driverInDsd = false;   // v10: ドライバが今DSDモードか（PCMへ戻すため）
    std::wstring m_dsdCapName;       // v10: ネイティブDSD対応を問い合わせ済みのドライバ名
    bool    m_dsdCap = false;

    // 上位→コールバックのリング（左詰めint32、interleaved）。単一生産者・単一消費者。
    std::vector<int32_t> m_ring;
    size_t m_ringCapacity = 0;      // サンプル数
    std::atomic<size_t> m_writeIndex{0};
    std::atomic<size_t> m_readIndex{0};
    std::vector<int32_t> m_tmp;     // コールバック用の作業領域（事前確保）

    std::mutex m_backpressureMutex;
    std::condition_variable m_backpressureCv;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_resetRequested{false};

    std::atomic<float> m_levelL{0.f};
    std::atomic<float> m_levelR{0.f};

    ErrorCallback m_errorCallback = nullptr;
    void* m_errorCallbackUserData = nullptr;
    std::string m_lastError;
    std::atomic<uint64_t> m_callbackCount{0};
    std::atomic<uint64_t> m_underrunCount{0};
    std::wstring m_logPath;
};
