#include <cstdio>
#include "WasapiExclusiveOutput.h"
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <fstream>
#include <cmath>
#include <mmsystem.h> // timeBeginPeriod/timeEndPeriod（winmm.libはCMakeLists.txtでリンク済み）

namespace {
    // ★ v10: コンテナ幅(containerBits)と有効ビット数(validBits)を明示指定する。
    //   デバイスによって受け付ける組み合わせが違うため（32/32のみ、24in32のみ、
    //   24bit詰め(3byte)のみ、16bitのみ…）、Initialize()側で順に試す。
    WAVEFORMATEXTENSIBLE BuildWaveFormat(uint32_t sampleRate, uint16_t channels,
                                         uint16_t containerBits, uint16_t validBits) {
        WAVEFORMATEXTENSIBLE wfx{};
        wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.nChannels = channels;
        wfx.Format.nSamplesPerSec = sampleRate;
        wfx.Format.wBitsPerSample = containerBits;
        wfx.Format.nBlockAlign = static_cast<WORD>((channels * containerBits) / 8);
        wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
        wfx.Format.cbSize = 22;

        wfx.Samples.wValidBitsPerSample = validBits;
        wfx.dwChannelMask = (channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
        wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        return wfx;
    }

    // ★ 配布用フリーソフトとして、エンドユーザーのPC設定変更を一切要求せず
    //   アプリ内部だけで完結する対策。このプロセスの実行中だけCPU実行速度の
    //   電源スロットリングを無効化する（Windows 10 1709+のみ有効なAPIのため、
    //   古いWindowsでは動的解決に失敗して静かに何もしない安全設計）。
    //   ユーザーの電源プラン設定そのものは変更しない。
    void DisableProcessPowerThrottling() {
        using SetProcessInformationFn =
            BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!kernel32) return;
        auto pSetProcessInformation = reinterpret_cast<SetProcessInformationFn>(
            GetProcAddress(kernel32, "SetProcessInformation"));
        if (!pSetProcessInformation) return;

        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask = 0;
        pSetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                                &state, sizeof(state));
    }
}

WasapiExclusiveOutput::WasapiExclusiveOutput() {
    m_renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_stopEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    m_qpcFrequency = freq.QuadPart;

    DisableProcessPowerThrottling();
}

WasapiExclusiveOutput::~WasapiExclusiveOutput() {
    Shutdown();
    if (m_renderEvent) CloseHandle(m_renderEvent);
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

AudioBackendResult WasapiExclusiveOutput::Initialize(const AudioFormat& format) {
    m_lastHr = S_OK;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), &m_enumerator);
    if (FAILED(hr)) { m_lastHr = hr; return AudioBackendResult::UnknownError; }

    hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
    if (FAILED(hr)) { m_lastHr = hr; return AudioBackendResult::DeviceNotFound; }

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m_audioClient);
    if (FAILED(hr)) { m_lastHr = hr; return AudioBackendResult::DeviceNotFound; }

    m_float = false;
    if (m_shared) {
        // ★ v10: 共有モード（Bluetoothなど排他モードを受け付けない機器用）。
        //   32bit float・ステレオで開く。レートは呼び出し側がWindowsの
        //   「既定の形式」に合わせてあるので、Windows側で周波数変換は起きない
        //   （AUTOCONVERTPCMは、チャンネル数が違う機器でも開けるようにする保険）。
        //   バッファは40ms。Bluetoothは遅延が大きく揺れるため、排他より多めに取る。
        m_waveFormat = BuildWaveFormat(format.sampleRate, format.channels, 32, 32);
        m_waveFormat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
        m_audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);
        m_defaultPeriod = defaultPeriod;
        m_minPeriod     = minPeriod;
        const REFERENCE_TIME sharedBuffer = 400000; // 40ms（100ns単位）
        hr = m_audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            sharedBuffer, 0,
            reinterpret_cast<WAVEFORMATEX*>(&m_waveFormat),
            nullptr);
        if (FAILED(hr)) { m_lastHr = hr; return AudioBackendResult::UnknownError; }
        m_containerBits = 32;
        m_validBits     = 32;
        m_float         = true;
    } else {
    // ★ v10: 受け付けるコンテナ形式をデバイスに順に問い合わせる。
    //   上位（AudioProcessThread）からは常に左詰めint32で渡され、
    //   ここで決まった形式への詰め直しはWriteFrames()内で行う。
    //   優先順：32bit(32有効) → 24bit詰め(3byte) → 24bit有効/32bitコンテナ → 16bit
    struct Candidate { uint16_t container; uint16_t valid; };
    std::vector<Candidate> candidates = { {32, 32}, {24, 24}, {32, 24}, {16, 16} };
    // ★ v10: 手動指定のビット数があれば、その形式を先頭に並べ替えて優先する
    if (m_preferredBits == 16 || m_preferredBits == 24 || m_preferredBits == 32) {
        std::stable_sort(candidates.begin(), candidates.end(),
            [this](const Candidate &a, const Candidate &b) {
                return (a.valid == m_preferredBits) > (b.valid == m_preferredBits);
            });
    }
    bool found = false;
    for (const Candidate &c : candidates) {
        m_waveFormat = BuildWaveFormat(format.sampleRate, format.channels, c.container, c.valid);
        hr = m_audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                               reinterpret_cast<WAVEFORMATEX*>(&m_waveFormat),
                                               nullptr);
        if (SUCCEEDED(hr) && hr != S_FALSE) {
            m_containerBits = c.container;
            m_validBits     = c.valid;
            found = true;
            break;
        }
    }
    if (!found) { m_lastHr = hr; return AudioBackendResult::FormatNotSupported; }

    REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
    m_audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);
    m_defaultPeriod = defaultPeriod;
    m_minPeriod     = minPeriod;

    // ★ v10修正：以前は最小周期(minPeriod)で初期化していたが、AIYIMA DAC-A7では
    //   バッファ529フレーム(3ms)で合意したのに、実際のイベントは8msごとにしか
    //   来なかった。毎回3ms分しか渡せず残り5msが欠け、約125Hzの「ブー」という
    //   音になっていた（実測：起床間隔8.0ms、出力は実時間の3/8の速さ）。
    //   ドライバが実際に動く既定周期(defaultPeriod)で初期化すれば、合意した
    //   バッファ長とイベント間隔が一致する。
    const REFERENCE_TIME period = (defaultPeriod > 0) ? defaultPeriod : minPeriod;

    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        period, period,
        reinterpret_cast<WAVEFORMATEX*>(&m_waveFormat),
        nullptr);

    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 alignedFrames = 0;
        m_audioClient->GetBufferSize(&alignedFrames);
        m_audioClient.Reset();
        m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m_audioClient);

        REFERENCE_TIME alignedPeriod = static_cast<REFERENCE_TIME>(
            10000000.0 * alignedFrames / m_waveFormat.Format.nSamplesPerSec + 0.5);

        hr = m_audioClient->Initialize(
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            alignedPeriod, alignedPeriod,
            reinterpret_cast<WAVEFORMATEX*>(&m_waveFormat),
            nullptr);
    }

    if (FAILED(hr)) { m_lastHr = hr; return AudioBackendResult::ExclusiveModeUnavailable; }
    } // if (m_shared) else

    // ★ 真実の源：初期化後に確定したバッファフレーム数を取得
    UINT32 confirmedBufferFrames = 0;
    hr = m_audioClient->GetBufferSize(&confirmedBufferFrames);
    if (FAILED(hr)) { m_lastHr = hr; return AudioBackendResult::UnknownError; }

    m_bufferFrameCount = confirmedBufferFrames;
    m_bytesPerFrame = m_waveFormat.Format.nBlockAlign;

    m_actualFormat.sampleRate = m_waveFormat.Format.nSamplesPerSec;
    m_actualFormat.channels = m_waveFormat.Format.nChannels;
    m_actualFormat.sampleFormat = (m_validBits == 16) ? AudioSampleFormat::Int16
                                : (m_validBits == 24) ? AudioSampleFormat::Int24
                                                      : AudioSampleFormat::Int32;
    // 変換用バッファ（非RTスレッドで使う。32/32以外のとき）
    m_convBuf.assign(static_cast<size_t>(m_bufferFrameCount) * m_bytesPerFrame, 0);


    // ★ リングバッファ容量：WASAPIバッファの8個分の余裕を確保。
    size_t bufferBytes = static_cast<size_t>(m_bufferFrameCount) * m_bytesPerFrame;
    m_ringCapacity = bufferBytes * 8 + 1;
    m_ring.assign(m_ringCapacity, 0);
    m_writeIndex.store(0, std::memory_order_relaxed);
    m_readIndex.store(0, std::memory_order_relaxed);

    hr = m_audioClient->SetEventHandle(m_renderEvent);
    if (FAILED(hr)) { m_lastHr = hr; return AudioBackendResult::UnknownError; }

    hr = m_audioClient->GetService(__uuidof(IAudioRenderClient), &m_renderClient);
    if (FAILED(hr)) { m_lastHr = hr; return AudioBackendResult::UnknownError; }

    // ===== デバッグ計測: 事前確保 =====
    // RTスレッド内でのアロケーションを避けるため、想定される最長再生時間分を
    // ここでまとめて確保しておく。44.1kHz/最小バッファ周期の場合、
    // 曲1曲(~10分)でもコールバック回数は数万〜数十万程度に収まるため、
    // 余裕を見て200万エントリ(int64_t×200万=16MB)を確保する。
    // ★ v10: 計測は診断出力先が指定されたとき（開発時）だけ行う。配布版では
    //   16MBの確保もRTスレッドでの記録も行わない（容量0なら記録処理は素通り）。
    if (!m_diagPath.empty()) {
        m_wakeTimestampCapacity = 2'000'000;
        m_wakeTimestamps.assign(m_wakeTimestampCapacity, 0);
    } else {
        m_wakeTimestampCapacity = 0;
        m_wakeTimestamps.clear();
        m_wakeTimestamps.shrink_to_fit();
    }
    m_wakeTimestampCount.store(0, std::memory_order_relaxed);
    m_underrunCount.store(0, std::memory_order_relaxed);
    m_totalCallbacks.store(0, std::memory_order_relaxed);

    return AudioBackendResult::Ok;
}

// ★ v10: 既定の出力デバイスの「既定の形式」（Windowsのサウンド設定→
//   デバイスのプロパティ→詳細→既定の形式）のサンプルレートを返す。
//   共有モードではこのレートで出せば、Windows側の周波数変換が起きない。
uint32_t WasapiExclusiveOutput::QueryMixRate() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), &enumerator))) return 0;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return 0;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return 0;
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix) return 0;
    const uint32_t rate = mix->nSamplesPerSec;
    CoTaskMemFree(mix);
    return rate;
}

uint32_t WasapiExclusiveOutput::GetBufferSize() const { return m_bufferFrameCount; }
AudioFormat WasapiExclusiveOutput::GetActualFormat() const { return m_actualFormat; }

AudioBackendResult WasapiExclusiveOutput::WriteFrames(const uint8_t* data, uint32_t frameCount) {
    // ★ 呼び出し元(AudioProcessThread、非RTだが優先度は高い)のみがここに入る。
    //   RTスレッド(RenderThreadProc)は絶対にこの関数を呼ばない。
    // ★ v10: 上位からは常に左詰めint32(4byte/sample)で届く。デバイスと合意した
    //   コンテナが32/32以外なら、ここで詰め直してからリングへ書く。
    //   ・24bit詰め：上位24bitを3byteで（24bit音源ならビットパーフェクト）
    //   ・24in32   ：下位8bitを0にした32bit（同上）
    //   ・16bit    ：上位16bit（16bit音源ならビットパーフェクト）
    // ★ v10: VUメーター用に、このチャンクのRMSを上位から届いた左詰めint32で測る
    //   （詰め直し前＝全フォーマット共通の値）。終端位置（累計バイト数）と一緒に
    //   FIFOへ積み、RTスレッドがその位置まで渡し終えた時点で現在値にする。
    {
        const int32_t* in = reinterpret_cast<const int32_t*>(data);
        const uint32_t ch = m_waveFormat.Format.nChannels;
        double sumL = 0.0, sumR = 0.0;
        constexpr double kS = 1.0 / 2147483648.0;
        for (uint32_t f = 0; f < frameCount; ++f) {
            const double l = in[f * ch] * kS;
            const double r = (ch >= 2) ? in[f * ch + 1] * kS : l;
            sumL += l * l;
            sumR += r * r;
        }
        const size_t head = m_levelHead.load(std::memory_order_relaxed);
        const size_t tail = m_levelTail.load(std::memory_order_acquire);
        m_bytesWrittenTotal += static_cast<uint64_t>(frameCount) * m_bytesPerFrame;
        if (head - tail < kLevelFifoSize && frameCount > 0) {
            LevelEntry &e = m_levelFifo[head % kLevelFifoSize];
            e.endByte = m_bytesWrittenTotal;
            e.l = static_cast<float>(std::sqrt(sumL / frameCount));
            e.r = static_cast<float>(std::sqrt(sumR / frameCount));
            m_levelHead.store(head + 1, std::memory_order_release);
        }
    }

    if (m_float) {
        // ★ v10: 共有モード。左詰めint32 → 32bit float（-1.0〜+1.0）
        const size_t samples = static_cast<size_t>(frameCount) * m_waveFormat.Format.nChannels;
        const size_t needed  = static_cast<size_t>(frameCount) * m_bytesPerFrame;
        if (m_convBuf.size() < needed) m_convBuf.resize(needed);
        const int32_t* in = reinterpret_cast<const int32_t*>(data);
        float* o = reinterpret_cast<float*>(m_convBuf.data());
        constexpr float kToFloat = 1.0f / 2147483648.0f;
        for (size_t i = 0; i < samples; ++i)
            o[i] = static_cast<float>(in[i]) * kToFloat;
        data = m_convBuf.data();
    } else if (!(m_containerBits == 32 && m_validBits == 32)) {
        const size_t samples = static_cast<size_t>(frameCount) * m_waveFormat.Format.nChannels;
        const size_t needed  = static_cast<size_t>(frameCount) * m_bytesPerFrame;
        if (m_convBuf.size() < needed) m_convBuf.resize(needed);
        const int32_t* in = reinterpret_cast<const int32_t*>(data);
        uint8_t* out = m_convBuf.data();
        if (m_containerBits == 24) {
            for (size_t i = 0; i < samples; ++i) {
                const uint32_t v = static_cast<uint32_t>(in[i]);
                out[i * 3 + 0] = static_cast<uint8_t>(v >> 8);
                out[i * 3 + 1] = static_cast<uint8_t>(v >> 16);
                out[i * 3 + 2] = static_cast<uint8_t>(v >> 24);
            }
        } else if (m_containerBits == 32) { // 24有効/32コンテナ
            int32_t* o = reinterpret_cast<int32_t*>(out);
            for (size_t i = 0; i < samples; ++i)
                o[i] = static_cast<int32_t>(static_cast<uint32_t>(in[i]) & 0xFFFFFF00u);
        } else { // 16bit
            int16_t* o = reinterpret_cast<int16_t*>(out);
            for (size_t i = 0; i < samples; ++i)
                o[i] = static_cast<int16_t>(in[i] >> 16);
        }
        data = m_convBuf.data();
    }

    size_t bytesToWrite = static_cast<size_t>(frameCount) * m_bytesPerFrame;
    size_t written = 0;
    // v10: 空き待ちが始まった時刻（時間で判定する。wait_forは述語が即成立して
    //   すぐ戻ることが多く、回数で数えると通常の待ちでも一瞬で上限に達してしまう）
    std::chrono::steady_clock::time_point stallStart{};

    while (written < bytesToWrite) {
        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        size_t freeSpace = (readIdx + m_ringCapacity - writeIdx - 1) % m_ringCapacity;

        if (freeSpace == 0) {
            // ★ v10修正（フリーズ対策の保険）：以前はここに抜け出す条件が無く、
            //   出力停止中やデバイス喪失（USB DAC抜去・Bluetooth切断）でリングが
            //   捌けなくなると永久に待ち続け、呼び出し元スレッドのjoin()で
            //   アプリ全体が固まっていた。
            if (!m_running.load()) return AudioBackendResult::Ok;           // 出力停止中は待たない
            const auto now = std::chrono::steady_clock::now();
            if (stallStart == std::chrono::steady_clock::time_point{}) {
                stallStart = now;
            } else if (now - stallStart > std::chrono::milliseconds(500)) {
                return AudioBackendResult::DeviceLost;                       // 0.5秒進まなければ諦める
            }
            std::unique_lock<std::mutex> lock(m_backpressureMutex);
            m_backpressureCv.wait_for(lock, std::chrono::milliseconds(2), [&] {
                return m_readIndex.load(std::memory_order_acquire) != writeIdx;
            });
            continue;
        }
        stallStart = std::chrono::steady_clock::time_point{};

        size_t chunk = std::min(bytesToWrite - written, freeSpace);
        size_t firstPart = std::min(chunk, m_ringCapacity - writeIdx);
        memcpy(&m_ring[writeIdx], data + written, firstPart);
        if (chunk > firstPart) {
            memcpy(&m_ring[0], data + written + firstPart, chunk - firstPart);
        }
        m_writeIndex.store((writeIdx + chunk) % m_ringCapacity, std::memory_order_release);
        written += chunk;
    }
    return AudioBackendResult::Ok;
}

AudioBackendResult WasapiExclusiveOutput::Start() {
    if (m_running) return AudioBackendResult::Ok;

    // ★ システムタイマー分解能を1msに上げる。既定(15.6ms程度)のままだと
    //   スレッドスケジューリングやタイムアウト系APIの粒度が粗く、
    //   DPC/スケジューリング起因のジッターを助長しうるため
    //   （配布用フリーソフトとして、アプリ内部だけで完結する対策）。
    //   Stop()側のtimeEndPeriod(1)と対で呼ぶこと。
    timeBeginPeriod(1);

    // ★ v10修正：Start()の前に、最初の1バッファ分をデバイスへ先に積んでおく
    //   （Microsoftのドキュメントが求める手順）。これをしないと、ドライバに
    //   よっては「1バッファ鳴らし終える→イベント→こちらが詰める→また鳴らし
    //   始める」の一段積みになり、毎回ドライバの処理時間(DAC-A7で約5ms)ぶん
    //   音が途切れる（実測：10ms周期なのにイベントが15msごと）。
    //   先に1つ積んでおけば常に1バッファ先行する二段積みになり、途切れない。
    //   （一時停止からの再開時など、既に積まれていてGetBufferが失敗する
    //    場合は何もしない）
    if (m_renderClient && m_bufferFrameCount > 0) {
        BYTE* prefill = nullptr;
        if (SUCCEEDED(m_renderClient->GetBuffer(m_bufferFrameCount, &prefill))) {
            m_renderClient->ReleaseBuffer(m_bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    m_running = true;
    m_renderThread = std::thread(&WasapiExclusiveOutput::RenderThreadProc, this);

    HRESULT hr = m_audioClient->Start();
    if (FAILED(hr)) {
        m_running = false;
        NotifyError(AudioBackendResult::UnknownError);
        timeEndPeriod(1);
        return AudioBackendResult::UnknownError;
    }
    return AudioBackendResult::Ok;
}

AudioBackendResult WasapiExclusiveOutput::Stop() {
    if (!m_running) return AudioBackendResult::Ok;
    m_running = false;
    SetEvent(m_stopEvent);
    if (m_renderThread.joinable()) m_renderThread.join();
    if (m_audioClient) m_audioClient->Stop();

    // ★ Start()で上げたタイマー分解能を元に戻す（対で呼ぶ）。
    timeEndPeriod(1);

    // ★ 両スレッドとも止まった後なので、素のstoreで安全にリセットできる。
    m_writeIndex.store(0, std::memory_order_relaxed);
    m_readIndex.store(0, std::memory_order_relaxed);
    // v10: VUメーター用の状態もリセット（両スレッド停止後なので安全）
    m_levelHead.store(0, std::memory_order_relaxed);
    m_levelTail.store(0, std::memory_order_relaxed);
    m_bytesWrittenTotal = 0;
    m_bytesReadTotal = 0;
    m_levelL.store(0.f, std::memory_order_relaxed);
    m_levelR.store(0.f, std::memory_order_relaxed);

    // ===== デバッグ計測: セッション終了時にCSVへ書き出す =====
    DumpDiagnostics();

    return AudioBackendResult::Ok;
}

void WasapiExclusiveOutput::RenderThreadProc() {
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    HANDLE waitHandles[2] = { m_stopEvent, m_renderEvent };

    while (m_running) {
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 2000);
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult != WAIT_OBJECT_0 + 1) continue;

        // ===== デバッグ計測: 起床時刻を記録 =====
        // 事前確保済みバッファに書くだけなので、RTスレッドでも安全。
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            size_t idx = m_wakeTimestampCount.fetch_add(1, std::memory_order_relaxed);
            if (idx < m_wakeTimestampCapacity) {
                m_wakeTimestamps[idx] = now.QuadPart;
            }
            // 容量を超えた分は記録しない（カウンタだけは回るので後で気づける）。
        }

        // ★ v10: 排他モードは毎回バッファ全体を渡す。共有モードは、Windowsの
        //   ミキサーがまだ使い切っていない分(padding)を除いた空きだけを渡す。
        UINT32 frames = m_bufferFrameCount;
        if (m_shared) {
            UINT32 padding = 0;
            if (FAILED(m_audioClient->GetCurrentPadding(&padding))) {
                NotifyError(AudioBackendResult::DeviceLost);
                continue;
            }
            frames = (padding < m_bufferFrameCount) ? (m_bufferFrameCount - padding) : 0;
            if (frames == 0) continue;
        }

        BYTE* renderBuffer = nullptr;
        HRESULT hr = m_renderClient->GetBuffer(frames, &renderBuffer);
        if (FAILED(hr)) { NotifyError(AudioBackendResult::DeviceLost); continue; }

        size_t bufferByteCapacity = static_cast<size_t>(frames) * m_bytesPerFrame;

        // ★ ここはRTスレッド。ミューテックスは一切取らない。
        size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        size_t available = (writeIdx + m_ringCapacity - readIdx) % m_ringCapacity;

        size_t toCopy = std::min(available, bufferByteCapacity);
        size_t firstPart = std::min(toCopy, m_ringCapacity - readIdx);
        memcpy(renderBuffer, &m_ring[readIdx], firstPart);
        if (toCopy > firstPart) {
            memcpy(renderBuffer + firstPart, &m_ring[0], toCopy - firstPart);
        }
        if (toCopy < bufferByteCapacity) {
            // ★ データが足りない分は無音で埋める（アンダーラン時のプチノイズ回避）。
            memset(renderBuffer + toCopy, 0, bufferByteCapacity - toCopy);

            // ===== デバッグ計測: アンダーラン発生を記録 =====
            m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        }
        m_readIndex.store((readIdx + toCopy) % m_ringCapacity, std::memory_order_release);

        // ★ v10: VUメーター。今デバイスへ渡し終えた位置までのレベルを現在値にする
        //   （FIFOから値を読むだけ。RTスレッドでの負担はごくわずか）。
        m_bytesReadTotal += toCopy;
        {
            size_t tail = m_levelTail.load(std::memory_order_relaxed);
            const size_t head = m_levelHead.load(std::memory_order_acquire);
            bool updated = false;
            float l = 0.f, r = 0.f;
            while (tail != head && m_levelFifo[tail % kLevelFifoSize].endByte <= m_bytesReadTotal) {
                l = m_levelFifo[tail % kLevelFifoSize].l;
                r = m_levelFifo[tail % kLevelFifoSize].r;
                updated = true;
                ++tail;
            }
            m_levelTail.store(tail, std::memory_order_release);
            if (updated) {
                m_levelL.store(l, std::memory_order_relaxed);
                m_levelR.store(r, std::memory_order_relaxed);
            } else if (toCopy == 0) {
                // 音が来ていない間は針を下ろしていく
                m_levelL.store(m_levelL.load(std::memory_order_relaxed) * 0.85f, std::memory_order_relaxed);
                m_levelR.store(m_levelR.load(std::memory_order_relaxed) * 0.85f, std::memory_order_relaxed);
            }
        }

        m_renderClient->ReleaseBuffer(frames, 0);

        // ★ ミューテックスを保持せずにnotify_oneを呼ぶ。
        if (toCopy > 0) {
            m_backpressureCv.notify_one();
        }

        // ===== デバッグ計測 =====
        m_totalCallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
}

void WasapiExclusiveOutput::SetErrorCallback(ErrorCallback callback, void* userData) {
    m_errorCallback = callback;
    m_errorCallbackUserData = userData;
}

void WasapiExclusiveOutput::NotifyError(AudioBackendResult error) {
    if (m_errorCallback) m_errorCallback(error, m_errorCallbackUserData);
}

void WasapiExclusiveOutput::Shutdown() {
    Stop();
    m_renderClient.Reset();
    m_audioClient.Reset();
    m_device.Reset();
    m_enumerator.Reset();
}

void WasapiExclusiveOutput::DumpDiagnostics() {
    // 呼び出し元(Stop())は非RTスレッドなので、ここでファイルI/Oをしても
    // オーディオのタイミングには影響しない。
    // ★ v10: 配布版ではファイルを書かない（SetDiagnosticsPath()で出力先を
    //   指定したときだけ書く。開発中の計測用）。
    if (m_diagPath.empty()) return;
    uint64_t totalCallbacks = m_totalCallbacks.load(std::memory_order_relaxed);
    uint64_t underruns = m_underrunCount.load(std::memory_order_relaxed);
    size_t wakeCount = m_wakeTimestampCount.load(std::memory_order_relaxed);
    size_t usableWakeCount = std::min(wakeCount, m_wakeTimestampCapacity);

    if (totalCallbacks == 0 && wakeCount == 0) {
        // Start()すらされていないセッションなら何も書かない。
        return;
    }

    // 理想周期(ミリ秒) = バッファフレーム数 / サンプルレート
    double idealPeriodMs = 0.0;
    if (m_actualFormat.sampleRate > 0) {
        idealPeriodMs = 1000.0 * m_bufferFrameCount / m_actualFormat.sampleRate;
    }

    // 起床間隔(ms)を計算し、ジッター統計を出す。
    double sumDeltaMs = 0.0;
    double sumSqDeltaMs = 0.0;
    double maxDeltaMs = 0.0;
    double minDeltaMs = (usableWakeCount >= 2) ? 1.0e9 : 0.0;
    size_t deltaCount = 0;

    std::wstring path = m_diagPath.empty() ? L"wasapi_diag_v9.csv" : m_diagPath;
    std::wofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) return;

    ofs << L"summary_field,value\n";
    ofs << L"backend,v9_lockfree_ring\n";
    ofs << L"sample_rate," << m_actualFormat.sampleRate << L"\n";
    ofs << L"buffer_frame_count," << m_bufferFrameCount << L"\n";
    ofs << L"ideal_period_ms," << idealPeriodMs << L"\n";
    ofs << L"total_callbacks," << totalCallbacks << L"\n";
    ofs << L"underrun_count," << underruns << L"\n";
    ofs << L"wake_timestamps_recorded," << usableWakeCount << L"\n";
    ofs << L"wake_timestamps_dropped_capacity_exceeded,"
        << (wakeCount > m_wakeTimestampCapacity ? (wakeCount - m_wakeTimestampCapacity) : 0) << L"\n";

    if (usableWakeCount >= 2 && m_qpcFrequency > 0) {
        for (size_t i = 1; i < usableWakeCount; ++i) {
            int64_t deltaTicks = m_wakeTimestamps[i] - m_wakeTimestamps[i - 1];
            double deltaMs = 1000.0 * static_cast<double>(deltaTicks) / static_cast<double>(m_qpcFrequency);
            sumDeltaMs += deltaMs;
            sumSqDeltaMs += deltaMs * deltaMs;
            maxDeltaMs = std::max(maxDeltaMs, deltaMs);
            minDeltaMs = std::min(minDeltaMs, deltaMs);
            ++deltaCount;
        }
        double meanMs = sumDeltaMs / deltaCount;
        double varianceMs = (sumSqDeltaMs / deltaCount) - (meanMs * meanMs);
        double stddevMs = (varianceMs > 0.0) ? std::sqrt(varianceMs) : 0.0;

        ofs << L"wake_interval_mean_ms," << meanMs << L"\n";
        ofs << L"wake_interval_stddev_ms," << stddevMs << L"\n";
        ofs << L"wake_interval_min_ms," << minDeltaMs << L"\n";
        ofs << L"wake_interval_max_ms," << maxDeltaMs << L"\n";
        ofs << L"wake_interval_max_deviation_from_ideal_ms," << (maxDeltaMs - idealPeriodMs) << L"\n";
    }

    // 生ログ（起床ごとの間隔）も残しておく。あとでヒストグラムを作る場合用。
    ofs << L"\nraw_wake_interval_ms\n";
    if (usableWakeCount >= 2 && m_qpcFrequency > 0) {
        for (size_t i = 1; i < usableWakeCount; ++i) {
            int64_t deltaTicks = m_wakeTimestamps[i] - m_wakeTimestamps[i - 1];
            double deltaMs = 1000.0 * static_cast<double>(deltaTicks) / static_cast<double>(m_qpcFrequency);
            ofs << deltaMs << L"\n";
        }
    }

    ofs.close();
}
