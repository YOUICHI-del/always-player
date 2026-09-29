#pragma once
#include "IPcmDecoder.h"
#include <cstdio>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────
// DsdPcmDecoder
//
// DSD（1bit）ファイル DSF / DFF(DSDIFF) を、PCMへ変換して出力する
// デコーダー。IPcmDecoderの実装としてPcmDualEngineから利用される。
//
// ★ 変換方式（2段の間引きフィルタ）
//   [A] 1bit → 1/8 間引き：96タップFIR（等リップル設計）。8bit=1byteを
//       まとめて処理するルックアップ表方式（12表×256）で高速。
//       折り返し帯域の減衰 -157dB。
//   [B] さらに 1/M 間引き：Kaiser窓sinc（β=9、-6dB点50kHz、遷移幅30kHz）。
//       出力は 44.1k系DSD → 176.4kHz、48k系DSD → 192kHz。
//       88.2kHz以上を -100dB 以下に落とすので、DSD特有の超音波ノイズは
//       DACへ渡らない。
//   シミュレーション（5次ΔΣで作った実際的なDSD64信号）で、帯域内
//   SN比は元のDSD信号そのもの(96.5dB)と同じ＝変換による劣化なし。
//
// ★ レベル：DSDの50%変調（SACDの0dB）を -3dBFS に割り当てる。
//   SACD規格で許される最大(70%変調)でもクリップしない値。
//
// ★ 出力はハイレゾ（176.4k/192k）扱いになるため、プレーヤー側では
//   ピュア（アップサンプリングなし）で再生される。
//
// ★ 非対応：DST圧縮のDFF、3ch以上（これらはOpen()で失敗させる）。
// ─────────────────────────────────────────────────────────
class DsdPcmDecoder : public IPcmDecoder {
public:
    // v10: dop=true なら PCM へ変換せず、DoP（DSD over PCM）の形で出力する。
    //   DSD 16bit分を1フレームに詰め、上位8bitにマーカー(0x05/0xFA交互)を置いた
    //   24bitワードを左詰めint32で返す。出力レート＝DSDレート/16
    //   （DSD64→176.4kHz、DSD128→352.8kHz、DSD256→705.6kHz）。
    //   この値はビット単位でそのままDACへ届ける必要がある（DSP・音量・
    //   リサンプル・16bit化は一切不可）。
    explicit DsdPcmDecoder(bool dop = false) : m_dop(dop) {}
    // ★ v10: ネイティブDSD（ASIOのDSDモード用）。DSDのバイトを加工せずに出す。
    //   1フレーム＝各チャンネル1バイト（DSD 8サンプル、MSBが古いサンプル）。
    //   左詰めint32の上位8bitにバイトを置き、「有効データ」の印としてbit16を立てる
    //   （値0は上位層のゼロ埋め＝無音の意味になり、出力側でDSDの無音0x69に置き換える）。
    //   出力レート（フレーム/秒）＝DSDレート/8（DSD64→352800）。
    static DsdPcmDecoder* CreateNative() { auto* d = new DsdPcmDecoder(false); d->m_native = true; return d; }
    bool IsNative() const { return m_native; }
    ~DsdPcmDecoder() override { Close(); }

    bool Open(const std::wstring& filePath) override;
    void Close() override;

    uint32_t GetSampleRate()    const override { return m_outRate; }
    uint32_t GetChannels()      const override { return 2; }
    uint32_t GetBitsPerSample() const override { return 24; } // 変換後PCMの表示用
    uint64_t GetTotalFrames()   const override { return m_totalFrames; }

    uint64_t ReadFrames(int32_t* out, uint64_t frameCount) override;
    bool SeekToFrame(uint64_t frame) override;

    // 表示・診断用：元のDSDのサンプリング周波数（2822400など）
    uint32_t GetDsdRate() const { return m_dsdRate; }
    bool IsDop() const { return m_dop; }

private:
    enum class Container { None, Dsf, Dff };

    bool ParseDsf();
    bool ParseDff();
    void PrepareFilters();
    void ResetFilterState();
    // 各チャンネルの次のDSDバイト（時間順・MSBが古いビット）を最大nバイト取り出す
    size_t ReadDsdBytes(size_t n);
    bool SeekToByte(uint64_t byteIndex);

    uint64_t  ReadFramesDop(int32_t* out, uint64_t frameCount);
    uint64_t  ReadFramesNative(int32_t* out, uint64_t frameCount);
    bool      m_native = false;      // v10: ネイティブDSD出力モード

    bool      m_dop = false;         // v10: DoP出力モード
    uint8_t   m_dopMarker = 0x05;    // v10: DoPマーカー（フレームごとに0x05/0xFAを交互）
    FILE*     m_fp = nullptr;
    Container m_container = Container::None;
    uint32_t  m_channels = 0;        // ファイルのチャンネル数(1 or 2)
    uint32_t  m_dsdRate = 0;
    bool      m_lsbFirst = false;    // DSF(bits=1)はLSBが古いビット
    uint64_t  m_dataStart = 0;       // 音声データ先頭のファイル位置
    uint64_t  m_bytesPerCh = 0;      // 有効なDSDバイト数（チャンネルあたり）
    uint32_t  m_blockSize = 0;       // DSF：チャンネルごとのブロック長(4096)

    uint64_t  m_bytePos = 0;         // 次に読むバイト位置（チャンネルあたり）
    std::vector<uint8_t> m_fileBuf;  // ファイル読み込み用
    std::vector<uint8_t> m_chBytes[2];

    // 変換フィルタ
    uint32_t  m_outRate = 0;
    uint32_t  m_M = 0;               // 段B の間引き率
    uint64_t  m_totalFrames = 0;
    std::vector<double> m_hB;        // 段B 係数
    uint8_t   m_histA[2][12] = {};   // 段A 直近12バイト（[0]が最新）
    std::vector<double> m_histB[2];  // 段B 直近の段A出力（リング）
    size_t    m_histBPos = 0;
    uint32_t  m_phase = 0;           // 段A出力の数 mod M
    uint64_t  m_skipFrames = 0;      // シーク直後に読み捨てる出力フレーム数
};
