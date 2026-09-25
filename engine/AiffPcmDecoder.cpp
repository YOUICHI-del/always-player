#include "AiffPcmDecoder.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
    uint32_t ReadU32BE(const uint8_t *p) {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
    }
    uint16_t ReadU16BE(const uint8_t *p) {
        return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
    }
    // 80bit IEEE754拡張精度(SANE形式、ビッグエンディアン) → double
    // AIFFのCOMMチャンクのsampleRateフィールドはこの形式で格納される。
    double ReadExtendedBE(const uint8_t *buf) {
        int sign = (buf[0] & 0x80) ? -1 : 1;
        int exponent = ((buf[0] & 0x7F) << 8) | buf[1];
        uint64_t mantissa = 0;
        for (int i = 0; i < 8; ++i) mantissa = (mantissa << 8) | buf[2 + i];
        if (exponent == 0 && mantissa == 0) return 0.0;
        double val = static_cast<double>(mantissa) * std::pow(2.0, exponent - 16383 - 63);
        return sign * val;
    }
}

bool AiffPcmDecoder::Open(const std::wstring& filePath) {
    Close();
    m_file.open(filePath, std::ios::binary);
    if (!m_file.is_open()) return false;

    uint8_t formHeader[12];
    m_file.read(reinterpret_cast<char*>(formHeader), 12);
    if (!m_file || std::memcmp(formHeader, "FORM", 4) != 0 ||
        (std::memcmp(formHeader + 8, "AIFF", 4) != 0 && std::memcmp(formHeader + 8, "AIFC", 4) != 0)) {
        Close();
        return false;
    }

    bool haveComm = false;
    bool haveSsnd = false;
    uint32_t ssndOffset = 0;
    uint64_t ssndDataPos = 0, ssndDataSize = 0;

    while (m_file) {
        uint8_t chunkHeader[8];
        m_file.read(reinterpret_cast<char*>(chunkHeader), 8);
        if (!m_file) break;
        char chunkId[5] = {0};
        std::memcpy(chunkId, chunkHeader, 4);
        uint32_t chunkSize = ReadU32BE(chunkHeader + 4);
        std::streampos chunkDataPos = m_file.tellg();

        if (std::memcmp(chunkId, "COMM", 4) == 0 && chunkSize >= 18) {
            uint8_t commBuf[18];
            m_file.read(reinterpret_cast<char*>(commBuf), 18);
            if (!m_file) { Close(); return false; }

            m_channels      = ReadU16BE(commBuf + 0);
            uint32_t numFrames = ReadU32BE(commBuf + 2);
            m_bitsPerSample = ReadU16BE(commBuf + 6);
            m_sampleRate    = static_cast<uint32_t>(ReadExtendedBE(commBuf + 8) + 0.5);
            m_totalFrames   = numFrames;
            haveComm = true;

            // 残り（AIFCの圧縮タイプ等）は読み飛ばす
            m_file.seekg(chunkDataPos + static_cast<std::streamoff>(chunkSize));
        } else if (std::memcmp(chunkId, "SSND", 4) == 0 && chunkSize >= 8) {
            uint8_t ssndHeader[8];
            m_file.read(reinterpret_cast<char*>(ssndHeader), 8);
            if (!m_file) { Close(); return false; }
            ssndOffset   = ReadU32BE(ssndHeader);
            ssndDataPos  = static_cast<uint64_t>(chunkDataPos) + 8 + ssndOffset;
            ssndDataSize = chunkSize - 8 - ssndOffset;
            haveSsnd = true;
            if (haveComm) break;
            m_file.seekg(chunkDataPos + static_cast<std::streamoff>(chunkSize));
        } else {
            uint32_t seekSize = chunkSize + (chunkSize & 1);
            m_file.seekg(chunkDataPos + static_cast<std::streamoff>(seekSize));
        }
    }

    if (!haveComm || !haveSsnd || m_channels != 2 ||
        (m_bitsPerSample != 8 && m_bitsPerSample != 16 &&
         m_bitsPerSample != 24 && m_bitsPerSample != 32)) {
        Close();
        return false;
    }

    m_bytesPerSample = m_bitsPerSample / 8;
    m_bytesPerFrame  = m_channels * m_bytesPerSample;
    m_dataStartPos   = ssndDataPos;
    // COMMのnumSampleFramesを信頼しつつ、SSNDの実データ量で上限を掛ける
    uint64_t framesFromData = m_bytesPerFrame > 0 ? (ssndDataSize / m_bytesPerFrame) : 0;
    if (m_totalFrames == 0 || m_totalFrames > framesFromData) m_totalFrames = framesFromData;
    m_framesReadSoFar = 0;

    m_file.clear();
    m_file.seekg(static_cast<std::streamoff>(m_dataStartPos));
    return true;
}

void AiffPcmDecoder::Close() {
    if (m_file.is_open()) m_file.close();
    m_file.clear();
    m_sampleRate = m_channels = m_bitsPerSample = m_bytesPerSample = m_bytesPerFrame = 0;
    m_totalFrames = m_dataStartPos = m_framesReadSoFar = 0;
}

uint64_t AiffPcmDecoder::ReadFrames(int32_t* out, uint64_t frameCount) {
    if (!m_file.is_open()) return 0;
    if (m_framesReadSoFar >= m_totalFrames) return 0;

    uint64_t framesToRead = std::min(frameCount, m_totalFrames - m_framesReadSoFar);
    if (framesToRead == 0) return 0;

    std::vector<uint8_t> raw(static_cast<size_t>(framesToRead) * m_bytesPerFrame);
    m_file.read(reinterpret_cast<char*>(raw.data()), raw.size());
    std::streamsize gotBytes = m_file.gcount();
    uint64_t gotFrames = static_cast<uint64_t>(gotBytes) / m_bytesPerFrame;
    if (gotFrames == 0) return 0;

    for (uint64_t f = 0; f < gotFrames; ++f) {
        for (uint32_t ch = 0; ch < m_channels; ++ch) {
            const uint8_t *p = &raw[(f * m_bytesPerFrame) + static_cast<uint64_t>(ch) * m_bytesPerSample];
            int32_t sample = 0;

            if (m_bitsPerSample == 8) {
                // AIFFの8bitは符号付き
                int8_t v = static_cast<int8_t>(p[0]);
                sample = static_cast<int32_t>(v) << 24;
            } else if (m_bitsPerSample == 16) {
                int16_t v = static_cast<int16_t>(ReadU16BE(p));
                sample = static_cast<int32_t>(v) << 16;
            } else if (m_bitsPerSample == 24) {
                int32_t v = (static_cast<int32_t>(p[0]) << 16) |
                            (static_cast<int32_t>(p[1]) << 8)  |
                             static_cast<int32_t>(p[2]);
                if (v & 0x00800000) v |= static_cast<int32_t>(0xFF000000); // 符号拡張
                sample = v << 8;
            } else if (m_bitsPerSample == 32) {
                sample = static_cast<int32_t>(ReadU32BE(p));
            }

            out[f * m_channels + ch] = sample;
        }
    }

    m_framesReadSoFar += gotFrames;
    return gotFrames;
}

bool AiffPcmDecoder::SeekToFrame(uint64_t frame) {
    if (!m_file.is_open()) return false;
    if (frame > m_totalFrames) frame = m_totalFrames;
    m_file.clear();
    m_file.seekg(static_cast<std::streamoff>(m_dataStartPos + frame * m_bytesPerFrame));
    m_framesReadSoFar = frame;
    return true;
}
