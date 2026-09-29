#pragma once
// ★ v10: ASIO用のサンプル形式変換（ASIOコールバック内で使う。ロック・確保なし）。
//   入力：左詰めint32のinterleaved（L,R,L,R…）の ch 番目のチャンネル。
//   出力：ドライバが要求する形式（ASIOSampleTypeの値）で dst へ frames 分。
//   テストしやすいよう、ASIO SDKのヘッダに依存しない形で書いてある
//  （形式の番号はasio.hのASIOSampleTypeと同じ値）。
#include <cstdint>
#include <cstring>

namespace asioconv {

enum : long {
    Int16LSB = 16, Int24LSB = 17, Int32LSB = 18, Float32LSB = 19, Float64LSB = 20,
    Int32LSB16 = 24, Int32LSB18 = 25, Int32LSB20 = 26, Int32LSB24 = 27,
    DSDInt8LSB1 = 32, DSDInt8MSB1 = 33,   // v10: ネイティブDSD（1バイト＝DSD 8サンプル）
};

// ★ v10: ネイティブDSDの形式か
inline bool IsDsd(long type) { return type == DSDInt8LSB1 || type == DSDInt8MSB1; }

// ネイティブDSD：上位層のint32（上位8bit＝DSDバイト、bit16＝有効データの印）→ドライバのバイト。
// 値0（上位層のゼロ埋め＝無音）はDSDの無音パターン0x69にする（0x00はDSDでは最大振幅の直流）。
inline uint8_t DsdByte(int32_t v, bool lsbFirst) {
    uint8_t b = (v == 0) ? static_cast<uint8_t>(0x69) : static_cast<uint8_t>(static_cast<uint32_t>(v) >> 24);
    if (lsbFirst) {   // MSBが古いサンプル → LSBが古いサンプルへ並べ替え
        b = static_cast<uint8_t>(((b * 0x0802u & 0x22110u) | (b * 0x8020u & 0x88440u)) * 0x10101u >> 16);
    }
    return b;
}

// この形式に対応しているか
inline bool Supported(long type) {
    switch (type) {
    case Int16LSB: case Int24LSB: case Int32LSB: case Float32LSB: case Float64LSB:
    case Int32LSB16: case Int32LSB18: case Int32LSB20: case Int32LSB24:
        return true;
    case DSDInt8LSB1: case DSDInt8MSB1:
        return true;
    default:
        return false;
    }
}

// 有効ビット数（表示用）
inline uint16_t ValidBits(long type) {
    switch (type) {
    case Int16LSB: case Int32LSB16: return 16;
    case Int32LSB18: return 18;
    case Int32LSB20: return 20;
    case Int24LSB: case Int32LSB24: return 24;
    case DSDInt8LSB1: case DSDInt8MSB1: return 1;
    default: return 32;
    }
}

// DoP（値をビット単位で保つ必要がある）に使える形式か：24bit以上の整数のみ
inline bool DopSafe(long type) {
    return type == Int24LSB || type == Int32LSB || type == Int32LSB24;
}

inline void Convert(const int32_t* in, int ch, long frames, long type, void* dst) {
    switch (type) {
    case Int32LSB: {
        int32_t* o = static_cast<int32_t*>(dst);
        for (long i = 0; i < frames; ++i) o[i] = in[i * 2 + ch];
        break;
    }
    case Int24LSB: {
        uint8_t* o = static_cast<uint8_t*>(dst);
        for (long i = 0; i < frames; ++i) {
            const uint32_t v = static_cast<uint32_t>(in[i * 2 + ch]);
            o[i * 3 + 0] = static_cast<uint8_t>(v >> 8);
            o[i * 3 + 1] = static_cast<uint8_t>(v >> 16);
            o[i * 3 + 2] = static_cast<uint8_t>(v >> 24);
        }
        break;
    }
    case Int16LSB: {
        int16_t* o = static_cast<int16_t*>(dst);
        for (long i = 0; i < frames; ++i) o[i] = static_cast<int16_t>(in[i * 2 + ch] >> 16);
        break;
    }
    // 32bitコンテナの右詰め（下位ビットに有効データ）。算術シフトで符号を保つ。
    case Int32LSB16: case Int32LSB18: case Int32LSB20: case Int32LSB24: {
        const int shift = (type == Int32LSB16) ? 16 : (type == Int32LSB18) ? 14
                        : (type == Int32LSB20) ? 12 : 8;
        int32_t* o = static_cast<int32_t*>(dst);
        for (long i = 0; i < frames; ++i) o[i] = in[i * 2 + ch] >> shift;
        break;
    }
    case Float32LSB: {
        float* o = static_cast<float*>(dst);
        constexpr double k = 1.0 / 2147483648.0;
        for (long i = 0; i < frames; ++i) o[i] = static_cast<float>(in[i * 2 + ch] * k);
        break;
    }
    case Float64LSB: {
        double* o = static_cast<double*>(dst);
        constexpr double k = 1.0 / 2147483648.0;
        for (long i = 0; i < frames; ++i) o[i] = in[i * 2 + ch] * k;
        break;
    }
    case DSDInt8MSB1: case DSDInt8LSB1: {   // framesはバイト数（DSD 8サンプル単位）
        uint8_t* o = static_cast<uint8_t*>(dst);
        const bool lsb = (type == DSDInt8LSB1);
        for (long i = 0; i < frames; ++i) o[i] = DsdByte(in[i * 2 + ch], lsb);
        break;
    }
    default:
        break;
    }
}

// 無音（上の形式のバイト幅でゼロ埋め）
inline void Silence(long frames, long type, void* dst) {
    size_t bytes = 4;
    if (type == Int16LSB) bytes = 2;
    else if (type == Int24LSB) bytes = 3;
    else if (type == Float64LSB) bytes = 8;
    std::memset(dst, 0, static_cast<size_t>(frames) * bytes);
}

} // namespace asioconv
