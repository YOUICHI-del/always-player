#pragma once
#include <QMutex>
#include <atomic>
#include <thread>

// ─────────────────────────────────────────────────────────
// WasapiLevelMeter
//
// BitPerfect OFF → WASAPI ループバックで実 PCM RMS を取得
// BitPerfect ON  → mpv の audio-pts 変化を監視し疑似レベルを生成
//
// 外部から見たインターフェースは同一（getLevels のみ）
// ─────────────────────────────────────────────────────────

// mpv の前方宣言（mpv/client.h を .h に引き込まないため）
struct mpv_handle;

class WasapiLevelMeter
{
public:
    WasapiLevelMeter();
    ~WasapiLevelMeter();

    // mpv ハンドルを渡す（audio-pts ポーリング用）
    void setMpvHandle(mpv_handle *mpv) { m_mpv = mpv; }

    // BitPerfect 状態を通知する（MainWindow から呼ぶ）
    void setBitPerfect(bool on);

    void start();
    void stop();

    // スレッドセーフ：VUMeter の tick() から呼ぶ
    void getLevels(float &left, float &right) const;

private:
    void loopbackLoop();    // BitPerfect OFF 用
    void pseudoLoop();      // BitPerfect ON 用

    mpv_handle        *m_mpv    = nullptr;
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_bitPerfect{false};
    std::thread        m_thread;

    mutable QMutex m_mutex;
    float m_left  = 0.f;
    float m_right = 0.f;
};
