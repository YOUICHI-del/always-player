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
    WAVEFORMATEXTENSIBLE BuildWaveFormat(const AudioFormat& fmt) {
        WAVEFORMATEXTENSIBLE wfx{};
        wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.nChannels = fmt.channels;
        wfx.Format.nSamplesPerSec = fmt.sampleRate;

        uint16_t bitsPerSample = 24;
        switch (fmt.sampleFormat) {
            case AudioSampleFormat::Int16: bitsPerSample = 16; break;
            case AudioSampleFormat::Int24: bitsPerSample = 24; break;
            case AudioSampleFormat::Int32: bitsPerSample = 32; break;
        }
        wfx.Format.wBitsPerSample = bitsPerSample;
        wfx.Format.nBlockAlign = (fmt.channels * bitsPerSample) / 8;
        wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
        wfx.Format.cbSize = 22;

        wfx.Samples.wValidBitsPerSample = bitsPerSample;
        wfx.dwChannelMask = (fmt.channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
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
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), &m_enumerator);
    if (FAILED(hr)) return AudioBackendResult::UnknownError;

    hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
    if (FAILED(hr)) return AudioBackendResult::DeviceNotFound;

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m_audioClient);
    if (FAILED(hr)) return AudioBackendResult::DeviceNotFound;

    m_waveFormat = BuildWaveFormat(format);

    hr = m_audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                           reinterpret_cast<WAVEFORMATEX*>(&m_waveFormat),
                                           nullptr);
    if (FAILED(hr)) return AudioBackendResult::FormatNotSupported;

    REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
    m_audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);

    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        minPeriod, minPeriod,
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

    if (FAILED(hr)) return AudioBackendResult::ExclusiveModeUnavailable;

    // ★ 真実の源：初期化後に確定したバッファフレーム数を取得
    UINT32 confirmedBufferFrames = 0;
    hr = m_audioClient->GetBufferSize(&confirmedBufferFrames);
    if (FAILED(hr)) return AudioBackendResult::UnknownError;

    m_bufferFrameCount = confirmedBufferFrames;
    m_bytesPerFrame = m_waveFormat.Format.nBlockAlign;

    m_actualFormat.sampleRate = m_waveFormat.Format.nSamplesPerSec;
    m_actualFormat.channels = m_waveFormat.Format.nChannels;
    m_actualFormat.sampleFormat = format.sampleFormat;

    // ★ リングバッファ容量：WASAPIバッファの8個分の余裕を確保。
    size_t bufferBytes = static_cast<size_t>(m_bufferFrameCount) * m_bytesPerFrame;
    m_ringCapacity = bufferBytes * 8 + 1;
    m_ring.assign(m_ringCapacity, 0);
    m_writeIndex.store(0, std::memory_order_relaxed);
    m_readIndex.store(0, std::memory_order_relaxed);

    hr = m_audioClient->SetEventHandle(m_renderEvent);
    if (FAILED(hr)) return AudioBackendResult::UnknownError;

    hr = m_audioClient->GetService(__uuidof(IAudioRenderClient), &m_renderClient);
    if (FAILED(hr)) return AudioBackendResult::UnknownError;

    // ===== デバッグ計測: 事前確保 =====
    // RTスレッド内でのアロケーションを避けるため、想定される最長再生時間分を
    // ここでまとめて確保しておく。44.1kHz/最小バッファ周期の場合、
    // 曲1曲(~10分)でもコールバック回数は数万〜数十万程度に収まるため、
    // 余裕を見て200万エントリ(int64_t×200万=16MB)を確保する。
    m_wakeTimestampCapacity = 2'000'000;
    m_wakeTimestamps.assign(m_wakeTimestampCapacity, 0);
    m_wakeTimestampCount.store(0, std::memory_order_relaxed);
    m_underrunCount.store(0, std::memory_order_relaxed);
    m_totalCallbacks.store(0, std::memory_order_relaxed);

    return AudioBackendResult::Ok;
}

uint32_t WasapiExclusiveOutput::GetBufferSize() const { return m_bufferFrameCount; }
AudioFormat WasapiExclusiveOutput::GetActualFormat() const { return m_actualFormat; }

AudioBackendResult WasapiExclusiveOutput::WriteFrames(const uint8_t* data, uint32_t frameCount) {
    // ★ 呼び出し元(AudioProcessThread、非RTだが優先度は高い)のみがここに入る。
    //   RTスレッド(RenderThreadProc)は絶対にこの関数を呼ばない。
    size_t bytesToWrite = static_cast<size_t>(frameCount) * m_bytesPerFrame;
    size_t written = 0;

    while (written < bytesToWrite) {
        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        size_t freeSpace = (readIdx + m_ringCapacity - writeIdx - 1) % m_ringCapacity;

        if (freeSpace == 0) {
            std::unique_lock<std::mutex> lock(m_backpressureMutex);
            m_backpressureCv.wait_for(lock, std::chrono::milliseconds(2), [&] {
                return m_readIndex.load(std::memory_order_acquire) != writeIdx;
            });
            continue;
        }

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

        BYTE* renderBuffer = nullptr;
        HRESULT hr = m_renderClient->GetBuffer(m_bufferFrameCount, &renderBuffer);
        if (FAILED(hr)) { NotifyError(AudioBackendResult::DeviceLost); continue; }

        size_t bufferByteCapacity = static_cast<size_t>(m_bufferFrameCount) * m_bytesPerFrame;

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

        m_renderClient->ReleaseBuffer(m_bufferFrameCount, 0);

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
