#include "WasapiLevelMeter.h"
#include <QDebug>
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mpv/client.h>

// COM スマートポインタ（ATL 不要）
template<class T>
struct ComPtr {
    T *p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T **operator&() { return &p; }
    T  *operator->() { return p; }
};

WasapiLevelMeter::WasapiLevelMeter() {}

WasapiLevelMeter::~WasapiLevelMeter()
{
    stop();
}

void WasapiLevelMeter::setBitPerfect(bool on)
{
    m_bitPerfect = on;
}

void WasapiLevelMeter::start()
{
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this]{
        if (m_bitPerfect)
            pseudoLoop();
        else
            loopbackLoop();
    });
}

void WasapiLevelMeter::stop()
{
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
}

void WasapiLevelMeter::getLevels(float &left, float &right) const
{
    QMutexLocker lk(&m_mutex);
    left  = m_left;
    right = m_right;
}

// ─────────────────────────────────────────────
// BitPerfect OFF：WASAPI ループバックで実 PCM 取得
// ─────────────────────────────────────────────
void WasapiLevelMeter::loopbackLoop()
{
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) {
        qWarning() << "[WasapiLevelMeter] CoInitializeEx failed" << hrCo;
        return;
    }

    HRESULT hr;
    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          (void**)&enumerator.p);
    if (FAILED(hr)) { qWarning() << "[WasapiLevelMeter] enumerator failed" << hr; goto cleanup; }

    {
        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device.p);
        if (FAILED(hr)) { qWarning() << "[WasapiLevelMeter] GetDefaultAudioEndpoint failed" << hr; goto cleanup; }

        ComPtr<IAudioClient> audioClient;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient.p);
        if (FAILED(hr)) { qWarning() << "[WasapiLevelMeter] Activate failed" << hr; goto cleanup; }

        WAVEFORMATEX *pwfx = nullptr;
        hr = audioClient->GetMixFormat(&pwfx);
        if (FAILED(hr)) { qWarning() << "[WasapiLevelMeter] GetMixFormat failed" << hr; goto cleanup; }

        const UINT32 nCh   = pwfx->nChannels;
        const UINT32 bits  = pwfx->wBitsPerSample;
        const bool isFloat = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                             (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                              reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx)->SubFormat
                              == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK,
                                     200 * 10000, 0, pwfx, nullptr);  // ★ 200ms（元は2000*10000=2秒だった。VUメーター遅延の主因）
        CoTaskMemFree(pwfx);
        if (FAILED(hr)) { qWarning() << "[WasapiLevelMeter] Initialize failed" << hr; goto cleanup; }

        ComPtr<IAudioCaptureClient> captureClient;
        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient.p);
        if (FAILED(hr)) { qWarning() << "[WasapiLevelMeter] GetService failed" << hr; goto cleanup; }

        audioClient->Start();
        qDebug() << "[WasapiLevelMeter] loopback started (shared mode)";

        while (m_running && !m_bitPerfect) {
            Sleep(20);

            UINT32 packetSize = 0;
            if (FAILED(captureClient->GetNextPacketSize(&packetSize))) break;

            float sumL = 0.f, sumR = 0.f;
            UINT32 totalFrames = 0;

            while (packetSize > 0) {
                BYTE *pData = nullptr; UINT32 numFrames = 0; DWORD flags = 0;
                hr = captureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && pData && numFrames > 0) {
                    if (isFloat) {
                        const float *s = reinterpret_cast<const float*>(pData);
                        for (UINT32 f = 0; f < numFrames; f++) {
                            float l = s[f*nCh], r = (nCh>=2)?s[f*nCh+1]:l;
                            sumL += l*l; sumR += r*r;
                        }
                    } else if (bits == 32) {
                        const int32_t *s = reinterpret_cast<const int32_t*>(pData);
                        constexpr float kS = 1.f/2147483648.f;
                        for (UINT32 f = 0; f < numFrames; f++) {
                            float l = s[f*nCh]*kS, r = (nCh>=2)?s[f*nCh+1]*kS:l;
                            sumL += l*l; sumR += r*r;
                        }
                    } else {
                        const int16_t *s = reinterpret_cast<const int16_t*>(pData);
                        constexpr float kS = 1.f/32768.f;
                        for (UINT32 f = 0; f < numFrames; f++) {
                            float l = s[f*nCh]*kS, r = (nCh>=2)?s[f*nCh+1]*kS:l;
                            sumL += l*l; sumR += r*r;
                        }
                    }
                    totalFrames += numFrames;
                }
                captureClient->ReleaseBuffer(numFrames);
                if (FAILED(captureClient->GetNextPacketSize(&packetSize))) packetSize = 0;
            }

            QMutexLocker lk(&m_mutex);
            if (totalFrames > 0) {
                m_left  = qBound(0.f, std::sqrt(sumL / totalFrames), 1.f);
                m_right = qBound(0.f, std::sqrt(sumR / totalFrames), 1.f);
            } else {
                m_left  *= 0.85f;
                m_right *= 0.85f;
            }
        }

        audioClient->Stop();
        qDebug() << "[WasapiLevelMeter] loopback stopped";
    }

cleanup:
    if (SUCCEEDED(hrCo)) CoUninitialize();

    // BitPerfect に切り替わった場合は疑似ループへ移行
    if (m_running && m_bitPerfect)
        pseudoLoop();
}

// ─────────────────────────────────────────────
// BitPerfect ON：audio-pts 変化から疑似レベルを生成
//
// mpv の audio-pts が進んでいる = 音が出ている
// pts の変化量（≒ 実時間）を基に自然なゆらぎを生成する
// ─────────────────────────────────────────────
void WasapiLevelMeter::pseudoLoop()
{
    qDebug() << "[WasapiLevelMeter] pseudo mode started (BitPerfect ON)";

    // 疑似レベル生成用の状態変数
    double t        = 0.0;   // アニメーション時間
    double levelL   = 0.3;
    double levelR   = 0.3;
    double velL     = 0.0;
    double velR     = 0.0;
    double prevPts  = -1.0;
    bool   playing  = false;
    int    silCount = 0;      // 無音フレームカウンタ

    while (m_running) {
        Sleep(20);  // 50fps

        // audio-pts の変化で再生中かどうかを判定
        if (m_mpv) {
            double pts = 0.0;
            if (mpv_get_property(m_mpv, "audio-pts", MPV_FORMAT_DOUBLE, &pts) >= 0) {
                if (prevPts >= 0.0 && pts > prevPts + 0.005) {
                    playing  = true;
                    silCount = 0;
                } else {
                    silCount++;
                    if (silCount > 10) playing = false;  // 200ms 変化なし → 停止
                }
                prevPts = pts;
            } else {
                playing = false;
            }
        }

        if (playing) {
            t += 0.02;

            // 複数の周波数成分を合成した自然なゆらぎ
            // 音楽のダイナミクスを模倣：低周波の大きなうねり + 高周波の細かい揺れ
            double base  = 0.55
                         + std::sin(t * 1.1)  * 0.18
                         + std::sin(t * 2.3)  * 0.09
                         + std::sin(t * 5.7)  * 0.04
                         + std::sin(t * 11.3) * 0.02;

            // L/R に独立したゆらぎを追加（ステレオ感）
            double targL = base + std::sin(t * 3.1 + 0.5) * 0.06;
            double targR = base + std::sin(t * 3.7 + 1.2) * 0.06;
            targL = qBound(0.05, targL, 1.0);
            targR = qBound(0.05, targR, 1.0);

            // 慣性（VU針の物理的な動き）
            velL += (targL - levelL) * 0.35; velL *= 0.70;
            velR += (targR - levelR) * 0.35; velR *= 0.70;
            levelL = qBound(0.0, levelL + velL, 1.0);
            levelR = qBound(0.0, levelR + velR, 1.0);
        } else {
            // 停止中：針を左端に向けて減衰
            levelL *= 0.88;
            levelR *= 0.88;
            velL   *= 0.5;
            velR   *= 0.5;
        }

        {
            QMutexLocker lk(&m_mutex);
            m_left  = static_cast<float>(levelL);
            m_right = static_cast<float>(levelR);
        }
    }

    qDebug() << "[WasapiLevelMeter] pseudo mode stopped";

    // 共有モードに戻った場合はループバックへ移行
    if (m_running && !m_bitPerfect)
        loopbackLoop();
}
