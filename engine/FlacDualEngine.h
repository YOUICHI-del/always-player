#pragma once
#include "RingBuffer.h"
#include "IAudioOutputBackend.h"
#include <string>
#include <thread>
#include <atomic>
#include <memory>

// ★ 実装マクロ(DR_FLAC_IMPLEMENTATION)はここでは定義しない。
//   ヘッダは複数の.cppから読み込まれる（直接のFlacDualEngine.cppと、
//   QtのmocsがPlayer.h経由で間接的に読み込むmocs_compilation.cpp）ため、
//   ここで定義すると実装が2回コンパイルされ「複数回定義」リンクエラーになる。
//   実装は FlacDualEngine.cpp の先頭で一度だけ定義する。
#include "dr_flac.h"

constexpr size_t kDecodeFrameSize = 4096;

class FlacDualEngine {
public:
    FlacDualEngine();
    ~FlacDualEngine();

    bool Open(const std::wstring& filePath);
    void Close();

    void AttachOutputBackend(IAudioOutputBackend* backend);

    void StartDecoding();
    void StopDecoding();

    // ★ シーク：一旦デコードスレッドを止め、リングバッファをクリアしてから
    //   目的のPCMフレーム位置へジャンプする。呼び出し前にデコード中だった
    //   場合は、シーク後に自動的にデコードスレッドを再開する。
    bool Seek(double seconds);

    AudioRingBuffer* GetRingBuffer() { return m_ring.get(); }

    uint32_t GetSampleRate() const { return m_sampleRate; }
    uint32_t GetTotalChannels() const { return m_channels; }
    uint32_t GetBitsPerSample() const { return m_bitsPerSample; }
    uint64_t GetTotalFrames() const { return m_totalFrames; }
    bool IsEndOfStream() const { return m_endOfStream.load(); }

private:
    void DecodeThreadProc();

    drflac* m_flac = nullptr;
    uint32_t m_sampleRate = 0;
    uint32_t m_channels = 0;
    uint32_t m_bitsPerSample = 0;
    uint64_t m_totalFrames = 0;

    std::unique_ptr<AudioRingBuffer> m_ring; // interleaved s32

    IAudioOutputBackend* m_outputBackend = nullptr; // 非所有

    std::thread m_decodeThread;
    std::atomic<bool> m_decoding{false};
    std::atomic<bool> m_endOfStream{false};
};
