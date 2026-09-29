#include "DsdPcmDecoder.h"
#include <cmath>
#include <climits>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <string>

#ifdef _WIN32
#include <windows.h>
#define DSD_FSEEK _fseeki64
#else
#define DSD_FSEEK fseeko
#endif

namespace {
    // ── 段A：1bit列を1/8に間引く96タップFIR（等リップル設計、係数の総和=1）。
    //    通過域0〜35kHz（0〜20kHzの偏差0.000dB）、折り返し帯域 -157dB。
    //    DSD64(2.8224MHz)基準で設計。DSD128以上では相対的にさらに余裕が出る。
    constexpr int kTapsA = 96;
    constexpr int kBytesA = kTapsA / 8;   // 12
    const double kCoefA[kTapsA] = {
    1.8516402353583773e-08, 4.3113600687944e-09, -1.5138230013884596e-07, -7.3526355336039347e-07,
    -2.3061522101549445e-06, -5.8057718851473159e-06, -1.2625753817245362e-05, -2.456889325273793e-05,
    -4.3628639875760203e-05, -7.1516851663688314e-05, -0.00010890344578920096, -0.00015439645205651297,
    -0.00020338685362755087, -0.00024699438288973755, -0.00027145734916303379, -0.00025837681396029629,
    -0.00018621837516566932, -3.3361910742625129e-05, 0.00021724595031286922, 0.00057213083821436078,
    0.0010207388371101479, 0.0015294379673183793, 0.0020375502272657066, 0.0024570255682422452,
    0.0026772113167698825, 0.0025756637472284837, 0.0020350962887308978, 0.00096543894065023901,
    -0.00067125169686071007, -0.0028364287096049818, -0.0053954293909427783, -0.0081070056971724105,
    -0.010625921496404419, -0.012521233274589616, -0.013310874499078336, -0.012510695950405545,
    -0.0096935299793452535, -0.0045515756265849675, 0.0030461228400373019, 0.013009752339555857,
    0.025006123680343342, 0.038462947661515534, 0.052602287135647754, 0.066501555051651798,
    0.079176435023556202, 0.08967666670560305, 0.097183339038216285, 0.10109558862720915,
    0.10109558862720915, 0.097183339038216285, 0.08967666670560305, 0.079176435023556202,
    0.066501555051651798, 0.052602287135647754, 0.038462947661515534, 0.025006123680343342,
    0.013009752339555857, 0.0030461228400373019, -0.0045515756265849675, -0.0096935299793452535,
    -0.012510695950405545, -0.013310874499078336, -0.012521233274589616, -0.010625921496404419,
    -0.0081070056971724105, -0.0053954293909427783, -0.0028364287096049818, -0.00067125169686071007,
    0.00096543894065023901, 0.0020350962887308978, 0.0025756637472284837, 0.0026772113167698825,
    0.0024570255682422452, 0.0020375502272657066, 0.0015294379673183793, 0.0010207388371101479,
    0.00057213083821436078, 0.00021724595031286922, -3.3361910742625129e-05, -0.00018621837516566932,
    -0.00025837681396029629, -0.00027145734916303379, -0.00024699438288973755, -0.00020338685362755087,
    -0.00015439645205651297, -0.00010890344578920096, -7.1516851663688314e-05, -4.3628639875760203e-05,
    -2.456889325273793e-05, -1.2625753817245362e-05, -5.8057718851473159e-06, -2.3061522101549445e-06,
    -7.3526355336039347e-07, -1.5138230013884596e-07, 4.3113600687944e-09, 1.8516402353583773e-08
    };

    double g_tableA[kBytesA][256];   // g_tableA[j][byte]：j=0が最新のバイト
    uint8_t g_bitRev[256];
    std::once_flag g_tableOnce;

    void BuildTables() {
        for (int j = 0; j < kBytesA; ++j) {
            for (int v = 0; v < 256; ++v) {
                // バイト内はMSBが古いビット、LSBが新しいビット。
                // 新しいビットほど係数の添字が小さい（h[0]が最新のサンプル）。
                double s = 0.0;
                for (int b = 0; b < 8; ++b)
                    s += kCoefA[8 * j + b] * (((v >> b) & 1) ? 1.0 : -1.0);
                g_tableA[j][v] = s;
            }
        }
        for (int v = 0; v < 256; ++v) {
            uint8_t r = 0;
            for (int b = 0; b < 8; ++b) if (v & (1 << b)) r |= static_cast<uint8_t>(0x80 >> b);
            g_bitRev[v] = r;
        }
    }

    double BesselI0(double x) {
        double sum = 1.0, term = 1.0;
        const double q = x * x / 4.0;
        for (int k = 1; k < 64; ++k) {
            term *= q / (static_cast<double>(k) * k);
            sum += term;
            if (term < sum * 1e-17) break;
        }
        return sum;
    }

    uint32_t RdLE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
    uint64_t RdLE64(const uint8_t* p) { return RdLE32(p) | (static_cast<uint64_t>(RdLE32(p + 4)) << 32); }
    uint32_t RdBE32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
    uint64_t RdBE64(const uint8_t* p) { return (static_cast<uint64_t>(RdBE32(p)) << 32) | RdBE32(p + 4); }
    uint16_t RdBE16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

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

    constexpr double kOutputGain = 1.4142135623730951; // +3dB：50%変調(SACD 0dB)=-3dBFS
    constexpr uint8_t kDsdIdle = 0x69;                  // DSDの無音パターン(01101001)
}

bool DsdPcmDecoder::Open(const std::wstring& filePath) {
    Close();
    std::call_once(g_tableOnce, BuildTables);

    m_fp = OpenWide(filePath);
    if (!m_fp) return false;

    uint8_t magic[4] = {};
    if (std::fread(magic, 1, 4, m_fp) != 4) { Close(); return false; }
    DSD_FSEEK(m_fp, 0, SEEK_SET);

    bool ok = false;
    if (std::memcmp(magic, "DSD ", 4) == 0)      ok = ParseDsf();
    else if (std::memcmp(magic, "FRM8", 4) == 0) ok = ParseDff();
    if (!ok || (m_channels != 1 && m_channels != 2) || m_dsdRate == 0 || m_bytesPerCh == 0) {
        Close();
        return false;
    }

    // ★ v10: ネイティブDSD。1チャンネルあたり1バイトで1フレーム（加工なし）。
    if (m_native) {
        if (m_dsdRate % 8 != 0) { Close(); return false; }
        m_outRate     = m_dsdRate / 8;       // DSD64→352800、DSD128→705600、DSD256→1411200 …
        m_totalFrames = m_bytesPerCh;
        SeekToByte(0);
        m_skipFrames = 0;
        return true;
    }

    // ★ v10: DoPモード。DSD 16bit(1チャンネルあたり2バイト)で1フレーム。
    //   フィルタは使わない（DSDのビット列をそのままDACへ届ける）。
    if (m_dop) {
        if (m_dsdRate % 16 != 0) { Close(); return false; }
        m_outRate     = m_dsdRate / 16;      // DSD64→176400、DSD128→352800、DSD256→705600
        m_totalFrames = m_bytesPerCh / 2;
        m_dopMarker   = 0x05;
        SeekToByte(0);
        m_skipFrames = 0;
        return true;
    }

    // 出力レート：44.1k系→176.4kHz、48k系→192kHz
    if (m_dsdRate % 44100 == 0)      m_outRate = 176400;
    else if (m_dsdRate % 48000 == 0) m_outRate = 192000;
    else { Close(); return false; }
    const uint32_t fsB = m_dsdRate / 8;
    if (fsB % m_outRate != 0) { Close(); return false; }
    m_M = fsB / m_outRate;
    if (m_M < 1) { Close(); return false; }

    PrepareFilters();
    m_totalFrames = m_bytesPerCh / m_M;
    ResetFilterState();
    SeekToByte(0);
    m_skipFrames = 0;
    return true;
}

bool DsdPcmDecoder::ParseDsf() {
    uint8_t h[28];
    if (std::fread(h, 1, 28, m_fp) != 28) return false;
    uint8_t f[52];
    if (std::fread(f, 1, 52, m_fp) != 52 || std::memcmp(f, "fmt ", 4) != 0) return false;
    const uint64_t fmtSize   = RdLE64(f + 4);
    const uint32_t formatId  = RdLE32(f + 16);
    m_channels               = RdLE32(f + 24);
    m_dsdRate                = RdLE32(f + 28);
    const uint32_t bits      = RdLE32(f + 32);
    const uint64_t sampleCnt = RdLE64(f + 36);
    m_blockSize              = RdLE32(f + 44);
    if (formatId != 0 || m_blockSize == 0) return false;
    m_lsbFirst = (bits == 1);

    DSD_FSEEK(m_fp, static_cast<long long>(28 + fmtSize), SEEK_SET);
    uint8_t d[12];
    if (std::fread(d, 1, 12, m_fp) != 12 || std::memcmp(d, "data", 4) != 0) return false;
    const uint64_t dataSize = RdLE64(d + 4) - 12;
    m_dataStart = 28 + fmtSize + 12;

    const uint64_t groups = (m_channels > 0) ? dataSize / (static_cast<uint64_t>(m_blockSize) * m_channels) : 0;
    const uint64_t maxBytes = groups * m_blockSize;
    m_bytesPerCh = (sampleCnt + 7) / 8;
    if (m_bytesPerCh > maxBytes) m_bytesPerCh = maxBytes;
    m_container = Container::Dsf;
    return true;
}

bool DsdPcmDecoder::ParseDff() {
    uint8_t h[16];
    if (std::fread(h, 1, 16, m_fp) != 16 || std::memcmp(h + 12, "DSD ", 4) != 0) return false;
    const uint64_t formEnd = 12 + RdBE64(h + 4);
    uint64_t pos = 16;
    bool gotData = false, compressed = false;
    uint64_t dataSize = 0;

    while (pos + 12 <= formEnd) {
        uint8_t ch[12];
        DSD_FSEEK(m_fp, static_cast<long long>(pos), SEEK_SET);
        if (std::fread(ch, 1, 12, m_fp) != 12) break;
        const uint64_t size = RdBE64(ch + 4);
        const uint64_t body = pos + 12;

        if (std::memcmp(ch, "PROP", 4) == 0) {
            uint64_t p = body + 4; // "SND "
            while (p + 12 <= body + size) {
                uint8_t sc[12];
                DSD_FSEEK(m_fp, static_cast<long long>(p), SEEK_SET);
                if (std::fread(sc, 1, 12, m_fp) != 12) break;
                const uint64_t ssize = RdBE64(sc + 4);
                uint8_t v[4] = {};
                if (std::memcmp(sc, "FS  ", 4) == 0 && std::fread(v, 1, 4, m_fp) == 4) {
                    m_dsdRate = RdBE32(v);
                } else if (std::memcmp(sc, "CHNL", 4) == 0 && std::fread(v, 1, 2, m_fp) == 2) {
                    m_channels = RdBE16(v);
                } else if (std::memcmp(sc, "CMPR", 4) == 0 && std::fread(v, 1, 4, m_fp) == 4) {
                    compressed = (std::memcmp(v, "DSD ", 4) != 0); // DST等は非対応
                }
                p += 12 + ssize + (ssize & 1);
            }
        } else if (std::memcmp(ch, "DSD ", 4) == 0) {
            m_dataStart = body;
            dataSize = size;
            gotData = true;
        }
        pos = body + size + (size & 1);
    }
    if (!gotData || compressed || m_channels == 0) return false;
    m_bytesPerCh = dataSize / m_channels;
    m_lsbFirst = false;
    m_container = Container::Dff;
    return true;
}

void DsdPcmDecoder::PrepareFilters() {
    // ── 段B：Kaiser窓sinc（β=9 ≒ 阻止域-90dB級、-6dB点50kHz、遷移幅30kHz）
    constexpr double kPi = 3.14159265358979323846;
    const double fsB   = static_cast<double>(m_dsdRate / 8);
    const double fc    = 50000.0;
    const double trans = 30000.0;
    const double beta  = 9.0;
    const double atten = beta / 0.1102 + 8.7;
    int n = static_cast<int>(std::ceil((atten - 8.0) / (2.285 * 2.0 * kPi * trans / fsB)));
    n = (n / static_cast<int>(m_M) + 1) * static_cast<int>(m_M);

    m_hB.assign(static_cast<size_t>(n), 0.0);
    const double i0b = BesselI0(beta);
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = i - (n - 1) / 2.0;
        const double x = 2.0 * fc / fsB * t;
        const double sinc = (std::fabs(x) < 1e-12) ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double r = 2.0 * i / (n - 1) - 1.0;
        const double w = BesselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        m_hB[static_cast<size_t>(i)] = 2.0 * fc / fsB * sinc * w;
        sum += m_hB[static_cast<size_t>(i)];
    }
    for (double& v : m_hB) v /= sum;
    m_histB[0].assign(m_hB.size(), 0.0);
    m_histB[1].assign(m_hB.size(), 0.0);
}

void DsdPcmDecoder::ResetFilterState() {
    std::memset(m_histA, kDsdIdle, sizeof(m_histA));
    for (auto& h : m_histB) std::fill(h.begin(), h.end(), 0.0);
    m_histBPos = 0;
    m_phase = 0;
}

void DsdPcmDecoder::Close() {
    if (m_fp) { std::fclose(m_fp); m_fp = nullptr; }
    m_container = Container::None;
    m_channels = m_dsdRate = m_blockSize = m_outRate = m_M = 0;
    m_dataStart = m_bytesPerCh = m_bytePos = m_totalFrames = m_skipFrames = 0;
}

bool DsdPcmDecoder::SeekToByte(uint64_t byteIndex) {
    if (byteIndex > m_bytesPerCh) byteIndex = m_bytesPerCh;
    m_bytePos = byteIndex;
    return true;
}

size_t DsdPcmDecoder::ReadDsdBytes(size_t n) {
    if (!m_fp || m_bytePos >= m_bytesPerCh) return 0;
    if (n > m_bytesPerCh - m_bytePos) n = static_cast<size_t>(m_bytesPerCh - m_bytePos);
    for (uint32_t c = 0; c < m_channels; ++c)
        if (m_chBytes[c].size() < n) m_chBytes[c].resize(n);

    size_t got = 0;
    if (m_container == Container::Dsf) {
        // DSF：チャンネルごとにblockSizeバイトずつ交互に並ぶ
        while (got < n) {
            const uint64_t group  = m_bytePos / m_blockSize;
            const uint64_t within = m_bytePos % m_blockSize;
            const size_t take = static_cast<size_t>(std::min<uint64_t>(m_blockSize - within, n - got));
            for (uint32_t c = 0; c < m_channels; ++c) {
                const uint64_t off = m_dataStart + group * m_blockSize * m_channels
                                   + static_cast<uint64_t>(c) * m_blockSize + within;
                DSD_FSEEK(m_fp, static_cast<long long>(off), SEEK_SET);
                if (std::fread(m_chBytes[c].data() + got, 1, take, m_fp) != take) { n = got; break; }
            }
            if (got == n) break;
            got += take;
            m_bytePos += take;
        }
    } else {
        // DFF：1バイトずつチャンネルが交互に並ぶ
        const size_t total = n * m_channels;
        if (m_fileBuf.size() < total) m_fileBuf.resize(total);
        DSD_FSEEK(m_fp, static_cast<long long>(m_dataStart + m_bytePos * m_channels), SEEK_SET);
        const size_t rd = std::fread(m_fileBuf.data(), 1, total, m_fp) / m_channels;
        for (size_t i = 0; i < rd; ++i)
            for (uint32_t c = 0; c < m_channels; ++c)
                m_chBytes[c][i] = m_fileBuf[i * m_channels + c];
        got = rd;
        m_bytePos += rd;
    }

    if (m_lsbFirst) {
        for (uint32_t c = 0; c < m_channels; ++c)
            for (size_t i = 0; i < got; ++i) m_chBytes[c][i] = g_bitRev[m_chBytes[c][i]];
    }
    return got;
}

// ★ v10: DoP（DSD over PCM）。各フレーム＝[マーカー8bit][古いDSDバイト][新しいDSDバイト]の
//   24bitを、左詰めint32（下位8bitは0）で返す。WASAPI出力側の24bit詰め／24in32／32bit
//   いずれでも、上位24bitがそのままDACへ届く。マーカーは両チャンネル同じ値で、
//   フレームごとに0x05と0xFAを交互に切り替える（DACはこれを見てDSDと判定する）。
uint64_t DsdPcmDecoder::ReadFramesDop(int32_t* out, uint64_t frameCount) {
    uint64_t produced = 0;
    while (produced < frameCount) {
        uint64_t needBytes = (frameCount - produced) * 2;
        if (needBytes > 65536) needBytes = 65536;
        const uint64_t posBefore = m_bytePos;
        const size_t got = ReadDsdBytes(static_cast<size_t>(needBytes));
        const size_t frames = got / 2;
        if (got % 2) m_bytePos = posBefore + frames * 2;   // 端数の1バイトは次回に回す
        if (frames == 0) break;                             // EOF
        for (size_t f = 0; f < frames; ++f) {
            const uint32_t marker = m_dopMarker;
            m_dopMarker = static_cast<uint8_t>(m_dopMarker ^ 0xFF);   // 0x05 <-> 0xFA
            for (int c = 0; c < 2; ++c) {
                const uint32_t src = (m_channels == 1) ? 0u : static_cast<uint32_t>(c);
                const uint32_t w = (marker << 24)
                                 | (static_cast<uint32_t>(m_chBytes[src][f * 2])     << 16)
                                 | (static_cast<uint32_t>(m_chBytes[src][f * 2 + 1]) << 8);
                out[(produced + f) * 2 + c] = static_cast<int32_t>(w);
            }
        }
        produced += frames;
    }
    return produced;
}

// ★ v10: ネイティブDSD。各チャンネルのDSDバイトをそのまま上位8bitへ（bit16は有効データの印）。
uint64_t DsdPcmDecoder::ReadFramesNative(int32_t* out, uint64_t frameCount) {
    uint64_t produced = 0;
    while (produced < frameCount) {
        uint64_t need = frameCount - produced;
        if (need > 65536) need = 65536;
        const size_t got = ReadDsdBytes(static_cast<size_t>(need));
        if (got == 0) break;                                 // EOF
        for (size_t f = 0; f < got; ++f) {
            for (int c = 0; c < 2; ++c) {
                const uint32_t src = (m_channels == 1) ? 0u : static_cast<uint32_t>(c);
                const uint32_t w = (static_cast<uint32_t>(m_chBytes[src][f]) << 24) | 0x00010000u;
                out[(produced + f) * 2 + c] = static_cast<int32_t>(w);
            }
        }
        produced += got;
    }
    return produced;
}

uint64_t DsdPcmDecoder::ReadFrames(int32_t* out, uint64_t frameCount) {
    if (!m_fp || frameCount == 0) return 0;
    if (m_native) return ReadFramesNative(out, frameCount);
    if (m_dop) return ReadFramesDop(out, frameCount);

    const size_t nB = m_hB.size();
    uint64_t produced = 0;

    while (produced < frameCount) {
        // ちょうど必要な分だけ読む（読み過ぎて出力を取りこぼさないため）
        uint64_t needBytes = (frameCount - produced + m_skipFrames) * m_M - m_phase;
        if (needBytes > 65536) needBytes = 65536;
        const size_t got = ReadDsdBytes(static_cast<size_t>(needBytes));
        if (got == 0) break; // EOF

        for (size_t i = 0; i < got; ++i) {
            // 段A：バイトを履歴に入れ、12表の和で1サンプル得る
            m_histBPos = (m_histBPos + 1) % nB;
            for (uint32_t c = 0; c < m_channels; ++c) {
                uint8_t* hA = m_histA[c];
                std::memmove(hA + 1, hA, kBytesA - 1);
                hA[0] = m_chBytes[c][i];
                double a = 0.0;
                for (int j = 0; j < kBytesA; ++j) a += g_tableA[j][hA[j]];
                m_histB[c][m_histBPos] = a;
            }
            if (++m_phase < m_M) continue;
            m_phase = 0;

            // 段B：M個ごとに1出力
            double y[2] = {0.0, 0.0};
            for (uint32_t c = 0; c < m_channels; ++c) {
                const std::vector<double>& hist = m_histB[c];
                size_t idx = m_histBPos;
                double acc = 0.0;
                for (size_t k = 0; k < nB; ++k) {
                    acc += m_hB[k] * hist[idx];
                    idx = (idx == 0) ? nB - 1 : idx - 1;
                }
                y[c] = acc * kOutputGain;
            }
            if (m_channels == 1) y[1] = y[0];

            if (m_skipFrames > 0) { --m_skipFrames; continue; }
            for (int c = 0; c < 2; ++c) {
                const double v = y[c] * 2147483648.0;
                out[produced * 2 + c] = (v >= 2147483647.0) ? INT32_MAX
                                      : (v <= -2147483648.0) ? INT32_MIN
                                      : static_cast<int32_t>(std::lrint(v));
            }
            ++produced;
        }
    }
    return produced;
}

bool DsdPcmDecoder::SeekToFrame(uint64_t frame) {
    if (!m_fp) return false;
    if (frame > m_totalFrames) frame = m_totalFrames;
    if (m_native) {                   // v10: ネイティブDSDは1フレーム＝1バイト
        SeekToByte(frame);
        m_skipFrames = 0;
        return true;
    }
    if (m_dop) {                      // v10: DoPはバイト位置へ直接移動するだけ
        SeekToByte(frame * 2);
        m_skipFrames = 0;
        return true;
    }
    // 段A(12バイト)と段Bの履歴を満たすだけ手前から読み直し、目的の位置まで
    // 出力を読み捨てる（連続再生時と1サンプルも違わない値になる）。
    const uint64_t target  = frame * m_M;
    const uint64_t preroll = m_hB.size() + kBytesA;
    uint64_t start = (target > preroll) ? target - preroll : 0;
    start -= start % m_M;
    ResetFilterState();
    SeekToByte(start);
    m_skipFrames = (target - start) / m_M;
    return true;
}
