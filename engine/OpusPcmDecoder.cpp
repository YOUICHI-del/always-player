#include "OpusPcmDecoder.h"
#include <opusfile.h>
#include <cmath>
#include <climits>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
    inline int32_t FloatToS32(float f) {
        // floatのままクリップするとINT32_MAXが表せず符号が反転するため、
        // 必ずdoubleで計算・クリップする。
        const double v = static_cast<double>(f) * 2147483648.0;
        if (v >=  2147483647.0) return INT32_MAX;
        if (v <= -2147483648.0) return INT32_MIN;
        return static_cast<int32_t>(std::lrint(v));
    }

    // opusfileのop_open_file()はWindowsではUTF-8のパスを受け取り、
    // 内部でUTF-16に変換して開く（日本語パス対応）。
    std::string ToUtf8(const std::wstring& w) {
#ifdef _WIN32
        if (w.empty()) return std::string();
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return std::string();
        std::string s(static_cast<size_t>(len) - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
        return s;
#else
        return std::string(w.begin(), w.end()); // テスト用（非Windows）
#endif
    }
}

bool OpusPcmDecoder::Open(const std::wstring& filePath) {
    Close();
    int err = 0;
    m_of = op_open_file(ToUtf8(filePath).c_str(), &err);
    if (!m_of) return false;

    const ogg_int64_t total = op_pcm_total(m_of, -1);
    m_totalFrames = (total > 0) ? static_cast<uint64_t>(total) : 0;
    return true;
}

void OpusPcmDecoder::Close() {
    if (m_of) {
        op_free(m_of);
        m_of = nullptr;
    }
    m_totalFrames = 0;
}

uint64_t OpusPcmDecoder::ReadFrames(int32_t* out, uint64_t frameCount) {
    if (!m_of || frameCount == 0) return 0;

    const size_t need = static_cast<size_t>(frameCount) * 2;
    if (m_floatBuf.size() < need) m_floatBuf.resize(need);

    uint64_t got = 0;
    int holeRetries = 0;
    while (got < frameCount) {
        const int n = op_read_float_stereo(m_of, m_floatBuf.data() + got * 2,
                                           static_cast<int>((frameCount - got) * 2));
        if (n == OP_HOLE && ++holeRetries < 8) continue; // データ欠落は読み飛ばして続行
        if (n <= 0) break; // EOF またはエラー
        got += static_cast<uint64_t>(n);
    }

    for (uint64_t i = 0; i < got * 2; ++i) out[i] = FloatToS32(m_floatBuf[i]);
    return got;
}

bool OpusPcmDecoder::SeekToFrame(uint64_t frame) {
    if (!m_of) return false;
    if (m_totalFrames > 0 && frame > m_totalFrames) frame = m_totalFrames;
    return op_pcm_seek(m_of, static_cast<ogg_int64_t>(frame)) == 0;
}
