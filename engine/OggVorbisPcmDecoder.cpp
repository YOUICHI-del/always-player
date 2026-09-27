#include "OggVorbisPcmDecoder.h"

// stb_vorbis.c 本体は別の翻訳単位(Cファイル)としてビルドする。
// ここでは宣言だけを取り込む。
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <cstdio>
#include <cmath>
#include <climits>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
    inline int32_t FloatToS32(float f) {
        // ±1.0 を 2^31 スケールへ。範囲外は正しい向きにクリップする
        // （floatのままクリップするとINT32_MAXが表せず符号が反転するため、
        //   必ずdoubleで計算する）。
        const double v = static_cast<double>(f) * 2147483648.0;
        if (v >=  2147483647.0) return INT32_MAX;
        if (v <= -2147483648.0) return INT32_MIN;
        return static_cast<int32_t>(std::lrint(v));
    }

    FILE* OpenWide(const std::wstring& path) {
#ifdef _WIN32
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"rb") != 0) return nullptr;
        return fp;
#else
        std::string s(path.begin(), path.end()); // テスト用（非Windows）
        return std::fopen(s.c_str(), "rb");
#endif
    }
}

bool OggVorbisPcmDecoder::Open(const std::wstring& filePath) {
    Close();

    FILE* fp = OpenWide(filePath); // 日本語パス対応のため自前で開いて渡す
    if (!fp) return false;

    int err = 0;
    m_vorbis = stb_vorbis_open_file(fp, /*close_handle_on_close=*/1, &err, nullptr);
    if (!m_vorbis) {
        std::fclose(fp);
        return false;
    }

    const stb_vorbis_info info = stb_vorbis_get_info(m_vorbis);
    m_sampleRate  = info.sample_rate;
    m_srcChannels = static_cast<uint32_t>(info.channels);
    m_totalFrames = stb_vorbis_stream_length_in_samples(m_vorbis);

    if (m_sampleRate == 0 || (m_srcChannels != 1 && m_srcChannels != 2)) {
        Close();
        return false;
    }
    return true;
}

void OggVorbisPcmDecoder::Close() {
    if (m_vorbis) {
        stb_vorbis_close(m_vorbis); // close_handle_on_close=1 なのでFILEも閉じられる
        m_vorbis = nullptr;
    }
    m_sampleRate = m_srcChannels = 0;
    m_totalFrames = 0;
}

uint64_t OggVorbisPcmDecoder::ReadFrames(int32_t* out, uint64_t frameCount) {
    if (!m_vorbis || frameCount == 0) return 0;

    const size_t need = static_cast<size_t>(frameCount) * m_srcChannels;
    if (m_floatBuf.size() < need) m_floatBuf.resize(need);

    uint64_t got = 0;
    while (got < frameCount) {
        const int want = static_cast<int>((frameCount - got) * m_srcChannels);
        const int n = stb_vorbis_get_samples_float_interleaved(
            m_vorbis, static_cast<int>(m_srcChannels),
            m_floatBuf.data() + got * m_srcChannels, want);
        if (n <= 0) break; // EOF
        got += static_cast<uint64_t>(n);
    }

    for (uint64_t f = 0; f < got; ++f) {
        const float l = m_floatBuf[f * m_srcChannels];
        const float r = (m_srcChannels == 2) ? m_floatBuf[f * m_srcChannels + 1] : l;
        out[f * 2]     = FloatToS32(l);
        out[f * 2 + 1] = FloatToS32(r);
    }
    return got;
}

bool OggVorbisPcmDecoder::SeekToFrame(uint64_t frame) {
    if (!m_vorbis) return false;
    if (m_totalFrames > 0 && frame > m_totalFrames) frame = m_totalFrames;
    return stb_vorbis_seek(m_vorbis, static_cast<unsigned int>(frame)) != 0;
}
