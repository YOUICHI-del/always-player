#include "WavPackPcmDecoder.h"
#include "wavpack/wavpack.h"
#include <windows.h>
#include <vector>
#include <algorithm>
#include <cstring>

namespace {
    // std::wstring(UTF-16) → UTF-8 std::string。
    // libwavpackはOPEN_FILE_UTF8フラグ指定時、内部でUTF-8→UTF-16(_wfopen)へ
    // 変換して開くため、日本語パスでもこの経路で安全に扱える。
    std::string WideToUtf8(const std::wstring &w) {
        if (w.empty()) return std::string();
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return std::string();
        std::string out(static_cast<size_t>(len) - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
        return out;
    }
}

bool WavPackPcmDecoder::Open(const std::wstring& filePath) {
    Close();

    std::string utf8Path = WideToUtf8(filePath);
    char errorBuf[128] = {0};

    m_ctx = WavpackOpenFileInput(utf8Path.c_str(), errorBuf, OPEN_NORMALIZE | OPEN_FILE_UTF8, 0);
    if (!m_ctx) return false;

    m_sampleRate    = WavpackGetSampleRate(m_ctx);
    m_channels      = static_cast<uint32_t>(WavpackGetNumChannels(m_ctx));
    m_bitsPerSample = static_cast<uint32_t>(WavpackGetBitsPerSample(m_ctx));
    int64_t total   = WavpackGetNumSamples64(m_ctx);
    m_totalFrames   = (total >= 0) ? static_cast<uint64_t>(total) : 0;

    int mode = WavpackGetMode(m_ctx);
    m_isFloat = (mode & MODE_FLOAT) != 0;
    m_shiftAmount = static_cast<int>(32 - m_bitsPerSample);
    if (m_shiftAmount < 0) m_shiftAmount = 0;

    if (m_sampleRate == 0 || m_channels == 0) {
        Close();
        return false;
    }
    return true;
}

void WavPackPcmDecoder::Close() {
    if (m_ctx) {
        WavpackCloseFile(m_ctx);
        m_ctx = nullptr;
    }
    m_sampleRate = m_channels = m_bitsPerSample = 0;
    m_totalFrames = 0;
    m_isFloat = false;
    m_shiftAmount = 0;
}

uint64_t WavPackPcmDecoder::ReadFrames(int32_t* out, uint64_t frameCount) {
    if (!m_ctx || frameCount == 0) return 0;

    // WavpackUnpackSamples()は「完全なサンプル数（フレーム数）」単位で
    // 読み取り、バッファには channels * frames 個のint32_tが必要。
    uint32_t gotFrames = WavpackUnpackSamples(m_ctx, out, static_cast<uint32_t>(frameCount));
    if (gotFrames == 0) return 0;

    const uint64_t totalSamples = static_cast<uint64_t>(gotFrames) * m_channels;

    if (m_isFloat) {
        // OPEN_NORMALIZEにより±1.0に正規化されたfloatが、各int32スロットに
        // ビットパターンとして格納されている。left-justified int32へ変換。
        for (uint64_t i = 0; i < totalSamples; ++i) {
            float fv;
            std::memcpy(&fv, &out[i], 4);
            if (fv > 1.0f) fv = 1.0f;
            if (fv < -1.0f) fv = -1.0f;
            out[i] = static_cast<int32_t>(fv * 2147483647.0f);
        }
    } else if (m_shiftAmount > 0) {
        // libwavpackはright-justifiedで返す（例：16bitなら±32k程度）。
        // 他のデコーダー(dr_flac等)と同じleft-justified規約に揃えるため
        // 左シフトする。
        for (uint64_t i = 0; i < totalSamples; ++i) {
            out[i] = static_cast<int32_t>(static_cast<uint32_t>(out[i]) << m_shiftAmount);
        }
    }

    return gotFrames;
}

bool WavPackPcmDecoder::SeekToFrame(uint64_t frame) {
    if (!m_ctx) return false;
    if (m_totalFrames > 0 && frame > m_totalFrames) frame = m_totalFrames;
    return WavpackSeekSample64(m_ctx, static_cast<int64_t>(frame)) != 0;
}
