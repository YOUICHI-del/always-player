#include "MfPcmDecoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propidl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace {
    constexpr DWORD kAudioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

    // ★ COMの初期化について
    //   Open()はUIスレッド（QtのSTA）から、ReadFrames()はPcmDualEngineの
    //   デコードスレッド（COM未初期化）から呼ばれる。CoIncrementMTAUsage()で
    //   プロセス全体の「暗黙のMTA」を一度だけ確保しておくと、COM未初期化の
    //   スレッドも自動的にMTAの一員として扱われ、デコードスレッド側で個別に
    //   CoInitializeEx/CoUninitializeを呼ぶ必要がなくなる（Windows 8以降）。
    //   IMFSourceReaderはフリースレッドなので、スレッドをまたいで使ってよい。
    void EnsureProcessMta() {
        static std::once_flag once;
        std::call_once(once, []{
            CO_MTA_USAGE_COOKIE cookie = nullptr;
            CoIncrementMTAUsage(&cookie); // 意図的に解放しない（プロセス終了まで保持）
        });
    }

    template <class T> void SafeRelease(T*& p) {
        if (p) { p->Release(); p = nullptr; }
    }

    // 可逆（整数PCMのまま受け取るべき）形式かどうか
    bool IsLosslessSubtype(const GUID& sub) {
        return sub == MFAudioFormat_PCM
            || sub == MFAudioFormat_ALAC
            || sub == MFAudioFormat_FLAC;
    }

    inline int32_t FloatToS32(float f) {
        // ±1.0 を 2^31 スケールへ（16bit<<16 と同じ基準）。+1.0ちょうどはクリップ。
        double v = static_cast<double>(f) * 2147483648.0;
        if (v >=  2147483647.0) return INT32_MAX;
        if (v <= -2147483648.0) return INT32_MIN;
        return static_cast<int32_t>(std::lrint(v));
    }
}

bool MfPcmDecoder::Open(const std::wstring& filePath) {
    Close();
    EnsureProcessMta();

    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    m_mfStarted = true;

    if (FAILED(MFCreateSourceReaderFromURL(filePath.c_str(), nullptr, &m_reader))) {
        Close();
        return false;
    }

    // 最初の音声ストリームだけを選択（埋め込みアートワーク等は読まない）
    m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(m_reader->SetStreamSelection(kAudioStream, TRUE))) {
        Close();
        return false;
    }

    // ── 元の（圧縮された）形式を調べる
    GUID    nativeSub      = GUID_NULL;
    UINT32  nativeChannels = 0;
    UINT32  nativeBits     = 0;
    {
        IMFMediaType* native = nullptr;
        if (FAILED(m_reader->GetNativeMediaType(kAudioStream, 0, &native))) {
            Close();
            return false;
        }
        native->GetGUID(MF_MT_SUBTYPE, &nativeSub);
        native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &nativeChannels);
        native->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &nativeBits);
        SafeRelease(native);
    }
    const bool lossless = IsLosslessSubtype(nativeSub);

    // ── 受け取る形式を指定する（部分的なメディアタイプ。残りはMFが埋める）
    auto trySetOutput = [&](bool asInt, UINT32 bits) -> bool {
        IMFMediaType* want = nullptr;
        if (FAILED(MFCreateMediaType(&want))) return false;
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE, asInt ? MFAudioFormat_PCM : MFAudioFormat_Float);
        want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, asInt ? bits : 32);
        if (nativeChannels > 2) {
            // 5.1ch等：デコーダー側でステレオへダウンミックスしてもらう
            want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        }
        HRESULT hr = m_reader->SetCurrentMediaType(kAudioStream, nullptr, want);
        SafeRelease(want);
        return SUCCEEDED(hr);
    };

    bool ok = false;
    if (lossless && (nativeBits == 16 || nativeBits == 24 || nativeBits == 32)) {
        ok = trySetOutput(true, nativeBits);   // 可逆：元のビット深度の整数のまま
    }
    if (!ok) ok = trySetOutput(false, 32);     // 非可逆（または整数指定失敗）：float
    if (!ok) { Close(); return false; }

    // ── 実際に決まった出力形式を読み取る
    {
        IMFMediaType* cur = nullptr;
        if (FAILED(m_reader->GetCurrentMediaType(kAudioStream, &cur))) {
            Close();
            return false;
        }
        GUID   sub = GUID_NULL;
        UINT32 sr = 0, ch = 0, bits = 0;
        cur->GetGUID(MF_MT_SUBTYPE, &sub);
        cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
        cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        cur->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        SafeRelease(cur);

        if (sub == MFAudioFormat_Float && bits == 32)       m_kind = SampleKind::Float32;
        else if (sub == MFAudioFormat_PCM && bits == 16)    m_kind = SampleKind::Int16;
        else if (sub == MFAudioFormat_PCM && bits == 24)    m_kind = SampleKind::Int24;
        else if (sub == MFAudioFormat_PCM && bits == 32)    m_kind = SampleKind::Int32;
        else { Close(); return false; }

        if (sr == 0 || (ch != 1 && ch != 2)) { Close(); return false; }

        m_sampleRate    = sr;
        m_srcChannels   = ch;
        m_bitsPerSample = (m_kind == SampleKind::Float32) ? 0 : bits;
    }

    // ── 長さ（100ns単位）→ フレーム数
    {
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(m_reader->GetPresentationAttribute(
                static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var))
            && var.vt == VT_UI8) {
            const uint64_t dur100ns = var.uhVal.QuadPart;
            m_totalFrames = (dur100ns * m_sampleRate + 5000000ULL) / 10000000ULL;
        }
        PropVariantClear(&var);
    }

    m_pending.clear();
    m_pendingPos      = 0;
    m_seekTargetFrame = -1;
    m_eof             = false;
    return true;
}

void MfPcmDecoder::Close() {
    SafeRelease(m_reader);
    if (m_mfStarted) {
        MFShutdown(); // MFStartupと対になる参照カウント方式
        m_mfStarted = false;
    }
    m_sampleRate = m_srcChannels = m_bitsPerSample = 0;
    m_totalFrames = 0;
    m_kind = SampleKind::Float32;
    m_pending.clear();
    m_pendingPos = 0;
    m_seekTargetFrame = -1;
    m_eof = false;
}

bool MfPcmDecoder::FillPending() {
    if (!m_reader || m_eof) return false;

    DWORD      flags  = 0;
    LONGLONG   ts     = 0;
    IMFSample* sample = nullptr;
    HRESULT hr = m_reader->ReadSample(kAudioStream, 0, nullptr, &flags, &ts, &sample);
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
        SafeRelease(sample);
        m_eof = true;
        return false;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        SafeRelease(sample);
        m_eof = true;
        return false;
    }
    if (!sample) return true; // データなしの通知（ストリームティック等）。次を読む

    IMFMediaBuffer* buf = nullptr;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) {
        SafeRelease(sample);
        return true;
    }

    BYTE* data = nullptr;
    DWORD bytes = 0;
    if (SUCCEEDED(buf->Lock(&data, nullptr, &bytes))) {
        const size_t bytesPerSample =
            (m_kind == SampleKind::Int16) ? 2 : (m_kind == SampleKind::Int24) ? 3 : 4;
        const size_t frameBytes = bytesPerSample * m_srcChannels;
        size_t frames = bytes / frameBytes;
        size_t skip   = 0;

        // ★ シーク直後：このサンプルの先頭位置から目標位置までを読み捨てる
        if (m_seekTargetFrame >= 0) {
            const int64_t startFrame = static_cast<int64_t>(
                (static_cast<long double>(ts) * m_sampleRate) / 10000000.0L + 0.5L);
            const int64_t diff = m_seekTargetFrame - startFrame;
            if (diff >= static_cast<int64_t>(frames)) {
                skip = frames;                 // まだ目標より手前。丸ごと捨てて次へ
            } else {
                skip = (diff > 0) ? static_cast<size_t>(diff) : 0;
                m_seekTargetFrame = -1;        // 目標に到達
            }
        }

        // 読み終わった分を詰めてから追記する
        if (m_pendingPos > 0) {
            m_pending.erase(m_pending.begin(), m_pending.begin() + m_pendingPos);
            m_pendingPos = 0;
        }
        const size_t useFrames = frames - skip;
        const size_t base = m_pending.size();
        m_pending.resize(base + useFrames * 2);
        int32_t* dst = m_pending.data() + base;
        const BYTE* src = data + skip * frameBytes;

        for (size_t f = 0; f < useFrames; ++f) {
            int32_t s[2] = {0, 0};
            for (uint32_t c = 0; c < m_srcChannels; ++c) {
                const BYTE* p = src + (f * m_srcChannels + c) * bytesPerSample;
                switch (m_kind) {
                case SampleKind::Float32: {
                    float v; std::memcpy(&v, p, 4);
                    s[c] = FloatToS32(v);
                    break;
                }
                case SampleKind::Int16: {
                    int16_t v; std::memcpy(&v, p, 2);
                    s[c] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(v)) << 16);
                    break;
                }
                case SampleKind::Int24:
                    s[c] = static_cast<int32_t>((static_cast<uint32_t>(p[2]) << 24)
                                              | (static_cast<uint32_t>(p[1]) << 16)
                                              | (static_cast<uint32_t>(p[0]) << 8));
                    break;
                case SampleKind::Int32:
                    std::memcpy(&s[c], p, 4);
                    break;
                }
            }
            if (m_srcChannels == 1) s[1] = s[0]; // モノラル：同じ値をL/Rへ
            dst[f * 2]     = s[0];
            dst[f * 2 + 1] = s[1];
        }
        buf->Unlock();
    }
    SafeRelease(buf);
    SafeRelease(sample);
    return true;
}

uint64_t MfPcmDecoder::ReadFrames(int32_t* out, uint64_t frameCount) {
    if (!m_reader || frameCount == 0) return 0;

    uint64_t got = 0;
    while (got < frameCount) {
        const size_t availSamples = m_pending.size() - m_pendingPos;
        if (availSamples == 0) {
            if (!FillPending()) break; // EOF
            continue;
        }
        const uint64_t availFrames = availSamples / 2;
        const uint64_t n = std::min<uint64_t>(availFrames, frameCount - got);
        std::memcpy(out + got * 2, m_pending.data() + m_pendingPos,
                    static_cast<size_t>(n) * 2 * sizeof(int32_t));
        m_pendingPos += static_cast<size_t>(n) * 2;
        got += n;
    }
    return got;
}

bool MfPcmDecoder::SeekToFrame(uint64_t frame) {
    if (!m_reader || m_sampleRate == 0) return false;
    if (m_totalFrames > 0 && frame > m_totalFrames) frame = m_totalFrames;

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = static_cast<LONGLONG>(
        (static_cast<long double>(frame) * 10000000.0L) / m_sampleRate);
    HRESULT hr = m_reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);
    if (FAILED(hr)) return false;

    m_pending.clear();
    m_pendingPos      = 0;
    m_eof             = false;
    m_seekTargetFrame = static_cast<int64_t>(frame);
    return true;
}
