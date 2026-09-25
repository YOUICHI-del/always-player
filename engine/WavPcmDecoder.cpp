#include "WavPcmDecoder.h"
#include <cstring>
#include <algorithm>
#include <vector>

namespace {
    uint32_t ReadU32LE(const uint8_t *p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
    uint16_t ReadU16LE(const uint8_t *p) {
        return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    }
    // WAVE_FORMAT_EXTENSIBLEのSubFormat GUID先頭4バイトで実フォーマットを判定
    // （KSDATAFORMAT_SUBTYPE_PCM = 00000001-..., IEEE_FLOAT = 00000003-...）
    constexpr uint16_t kFormatPcm       = 1;
    constexpr uint16_t kFormatFloat     = 3;
    constexpr uint16_t kFormatExtensible = 0xFFFE;
}

bool WavPcmDecoder::Open(const std::wstring& filePath) {
    Close();
    m_file.open(filePath, std::ios::binary);
    if (!m_file.is_open()) return false;

    uint8_t riffHeader[12];
    m_file.read(reinterpret_cast<char*>(riffHeader), 12);
    if (!m_file || std::memcmp(riffHeader, "RIFF", 4) != 0 ||
        std::memcmp(riffHeader + 8, "WAVE", 4) != 0) {
        Close();
        return false;
    }

    bool haveFmt = false;
    uint16_t formatTag = 0;

    while (m_file) {
        uint8_t chunkHeader[8];
        m_file.read(reinterpret_cast<char*>(chunkHeader), 8);
        if (!m_file) break;
        char chunkId[5] = {0};
        std::memcpy(chunkId, chunkHeader, 4);
        uint32_t chunkSize = ReadU32LE(chunkHeader + 4);
        std::streampos chunkDataPos = m_file.tellg();

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            std::vector<uint8_t> fmtBuf(std::max<uint32_t>(chunkSize, 16));
            m_file.read(reinterpret_cast<char*>(fmtBuf.data()), chunkSize);
            if (!m_file) { Close(); return false; }

            formatTag       = ReadU16LE(&fmtBuf[0]);
            m_channels      = ReadU16LE(&fmtBuf[2]);
            m_sampleRate    = ReadU32LE(&fmtBuf[4]);
            m_bitsPerSample = ReadU16LE(&fmtBuf[14]);

            if (formatTag == kFormatExtensible && chunkSize >= 40) {
                // cbSize(2) + validBitsPerSample(2) + channelMask(4) + SubFormatGUID(16)
                // fmtBuf[16..17]=cbSize, [18..19]=validBits, [20..23]=mask, [24..39]=GUID
                uint16_t subFormat = ReadU16LE(&fmtBuf[24]);
                formatTag = subFormat; // GUID先頭2バイトがWAVE_FORMAT_*と同じ値
            }
            m_isFloat = (formatTag == kFormatFloat);
            haveFmt = true;
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            m_dataStartPos = static_cast<uint64_t>(chunkDataPos);
            m_dataSize     = chunkSize;
            // dataチャンクは通常ファイル末尾に近いが、まだ後続チャンクが
            // ある場合に備え読み飛ばしを続ける（fmtが先に見つかっていれば
            // ここで確定してよい）
            if (haveFmt) break;
        }

        // 偶数バイト境界にパディング
        uint32_t seekSize = chunkSize + (chunkSize & 1);
        m_file.seekg(chunkDataPos + static_cast<std::streamoff>(seekSize));
    }

    if (!haveFmt || m_dataSize == 0 || m_channels != 2 ||
        (formatTag != kFormatPcm && formatTag != kFormatFloat)) {
        Close();
        return false;
    }
    if (m_bitsPerSample != 8 && m_bitsPerSample != 16 &&
        m_bitsPerSample != 24 && m_bitsPerSample != 32) {
        Close();
        return false;
    }

    m_bytesPerFrame  = m_channels * (m_bitsPerSample / 8);
    m_totalFrames    = m_dataSize / m_bytesPerFrame;
    m_framesReadSoFar = 0;

    m_file.clear();
    m_file.seekg(static_cast<std::streamoff>(m_dataStartPos));
    return true;
}

void WavPcmDecoder::Close() {
    if (m_file.is_open()) m_file.close();
    m_file.clear();
    m_sampleRate = m_channels = m_bitsPerSample = m_bytesPerFrame = 0;
    m_totalFrames = m_dataStartPos = m_dataSize = m_framesReadSoFar = 0;
    m_isFloat = false;
}

uint64_t WavPcmDecoder::ReadFrames(int32_t* out, uint64_t frameCount) {
    if (!m_file.is_open()) return 0;
    if (m_framesReadSoFar >= m_totalFrames) return 0;

    uint64_t framesToRead = std::min(frameCount, m_totalFrames - m_framesReadSoFar);
    if (framesToRead == 0) return 0;

    std::vector<uint8_t> raw(static_cast<size_t>(framesToRead) * m_bytesPerFrame);
    m_file.read(reinterpret_cast<char*>(raw.data()), raw.size());
    std::streamsize gotBytes = m_file.gcount();
    uint64_t gotFrames = static_cast<uint64_t>(gotBytes) / m_bytesPerFrame;
    if (gotFrames == 0) return 0;

    const int bytesPerSample = m_bitsPerSample / 8;
    for (uint64_t f = 0; f < gotFrames; ++f) {
        for (uint32_t ch = 0; ch < m_channels; ++ch) {
            const uint8_t *p = &raw[(f * m_bytesPerFrame) + static_cast<uint64_t>(ch) * bytesPerSample];
            int32_t sample = 0;

            if (m_isFloat && m_bitsPerSample == 32) {
                float fv;
                std::memcpy(&fv, p, 4);
                if (fv > 1.0f) fv = 1.0f;
                if (fv < -1.0f) fv = -1.0f;
                sample = static_cast<int32_t>(fv * 2147483647.0f);
            } else if (m_bitsPerSample == 8) {
                // WAVの8bit PCMはunsigned(0..255、中心128)
                int v = static_cast<int>(p[0]) - 128;
                sample = v << 24;
            } else if (m_bitsPerSample == 16) {
                int16_t v = static_cast<int16_t>(ReadU16LE(p));
                sample = static_cast<int32_t>(v) << 16;
            } else if (m_bitsPerSample == 24) {
                int32_t v = (static_cast<int32_t>(p[0])) |
                            (static_cast<int32_t>(p[1]) << 8) |
                            (static_cast<int32_t>(p[2]) << 16);
                if (v & 0x00800000) v |= static_cast<int32_t>(0xFF000000); // 符号拡張
                sample = v << 8;
            } else if (m_bitsPerSample == 32) {
                sample = static_cast<int32_t>(ReadU32LE(p));
            }

            out[f * m_channels + ch] = sample;
        }
    }

    m_framesReadSoFar += gotFrames;
    return gotFrames;
}

bool WavPcmDecoder::SeekToFrame(uint64_t frame) {
    if (!m_file.is_open()) return false;
    if (frame > m_totalFrames) frame = m_totalFrames;
    m_file.clear();
    m_file.seekg(static_cast<std::streamoff>(m_dataStartPos + frame * m_bytesPerFrame));
    m_framesReadSoFar = frame;
    return true;
}
