#include "AudioProcessThread.h"
#include <windows.h>
#include <chrono>
#include <algorithm>

AudioProcessThread::AudioProcessThread(AudioRingBuffer* sourceRing, IAudioOutputBackend* backend)
    : m_sourceRing(sourceRing), m_backend(backend) {
    m_chunkFrames = backend->GetBufferSize(); // 真実の源
}

AudioProcessThread::~AudioProcessThread() { Stop(); }

void AudioProcessThread::SetDspCallback(DspCallback callback) { m_dspCallback = std::move(callback); }
void AudioProcessThread::SetBitPerfect(bool enabled) { m_bitPerfect.store(enabled, std::memory_order_relaxed); }

void AudioProcessThread::Start() {
    if (m_running) return;
    m_running = true;
    m_thread = std::thread(&AudioProcessThread::ThreadProc, this);
}

void AudioProcessThread::Stop() {
    if (!m_running) return;
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void AudioProcessThread::ThreadProc() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    const size_t samplesPerChunk = m_chunkFrames * 2; // interleaved L,R
    std::vector<int32_t> s32Chunk(samplesPerChunk);
    std::vector<float> floatChunk;

    // ★ s32Chunkに満タン分(samplesPerChunk)のデータが入っている前提で、
    //   ビットパーフェクト/DSP経路を通してWriteFrames()まで行う共通処理。
    //   最終端の端数フレームを出力する際も、末尾をゼロ埋めした上で
    //   このまま同じ経路（常にm_chunkFrames分）を通す。
    auto outputChunk = [&]{
        // ★ ここではコンテナへの詰め直し（16/24/32bitパッキング）を一切行わない。
        //   WasapiExclusiveOutput::WriteFrames()は「生のint32(4byte/sample、
        //   左詰め正規化済み)」を受け取る契約になっており、デバイスと実際に
        //   合意したコンテナ幅へのパッキングはその内部で完結する。
        //   （かつてここでPackInt32To24によって先に24bit(3byte/sample)へ
        //   詰めてWriteFramesに渡していたが、WriteFrames側は常にint32として
        //   読み直す実装のため、サンプル境界がずれる上にバッファの範囲外を
        //   読みに行く実害のあるバグだった。二重にパッキングしないこと。）
        bool bitPerfect = m_bitPerfect.load(std::memory_order_relaxed);
        if (bitPerfect || !m_dspCallback) {
            // ★ ビットパーフェクト経路：値そのものは一切変換しない。
            m_backend->WriteFrames(reinterpret_cast<const uint8_t*>(s32Chunk.data()),
                                    static_cast<uint32_t>(m_chunkFrames));
        } else {
            floatChunk.resize(samplesPerChunk);
            // ★ 修正: 2147483648.0f(2^31)ではなく2147483647.0f(2^31-1、
            //   INT32_MAX)を正規化係数に使う。2^31を使うと理論上+1.0の
            //   入力がint32範囲を超えてしまい、丸め次第で符号反転や
            //   意図しないクリップが起きうる。正負対称な2^31-1を使うことで
            //   往復変換（float化→int32化）が正確に可逆になる（TEST_Playerで
            //   検証済みの修正をこちらにも反映）。
            constexpr float kInt32FullScale = 2147483647.0f; // INT32_MAX
            constexpr float kS32ToFloatScale = 1.0f / kInt32FullScale;
            for (size_t i = 0; i < samplesPerChunk; ++i) {
                floatChunk[i] = static_cast<float>(s32Chunk[i]) * kS32ToFloatScale;
            }

            m_dspCallback(floatChunk.data(), m_chunkFrames);

            // ★ v10修正（重大）：以前はfloatのまま「v > 2147483647.0f」でクリップして
            //   int32へ変換していたが、floatでは2147483647を表せず2147483648.0に
            //   丸められる。そのため0dBを少しでも超えたサンプル（DSPのEQや高調波で
            //   音量の大きい曲のピークが超える）が「+最大」ではなく「-最大」に
            //   化けていた（x86の変換は範囲外で0x80000000を返す）。これが音量の
            //   大きい曲での「バリバリ」の正体。テスト用の小さな正弦波では0dBに
            //   届かないため再現しなかった。doubleで計算・クリップしてから変換する。
            constexpr double kFullScaleD = 2147483647.0;
            for (size_t i = 0; i < samplesPerChunk; ++i) {
                double v = static_cast<double>(floatChunk[i]) * kFullScaleD;
                if (v >  kFullScaleD) v =  kFullScaleD;
                if (v < -kFullScaleD) v = -kFullScaleD;
                s32Chunk[i] = static_cast<int32_t>(v);
            }
            m_backend->WriteFrames(reinterpret_cast<const uint8_t*>(s32Chunk.data()),
                                    static_cast<uint32_t>(m_chunkFrames));
        }
    };

    while (m_running) {
        // ★ 重要：AudioRingBuffer::Read()は「要求数に満たなくても、取れた分だけ
        //   読み出してリングバッファからは削除してしまう」実装。そのため、
        //   先にRead()を呼んでから「満タンでなければ捨てる」という順序だと、
        //   実際に取り出せた分の音声データが一切出力されずに消失してしまう
        //   （曲終端付近・ギャップレス切り替え付近など、リングバッファが
        //   一瞬でも1チャンク未満になる状況で必ず発生する実害のあるバグだった）。
        //   単一生産者・単一消費者のリングバッファであり、AvailableToRead()は
        //   消費側(このスレッド)だけが減らせる値なので、確認後にRead()するまでの
        //   間に減ることはない。1チャンク分揃うまではRead()自体を呼ばずに待つ。
        const size_t available = m_sourceRing->AvailableToRead();
        if (available < samplesPerChunk) {
            // ★ 「もう書き込みは来ない」（真の終端）かつ端数が残っている場合のみ、
            //   ゼロ埋めして最後の1回だけ出力する。まだ書き込みが来る可能性が
            //   ある間（ギャップレス遷移中を含む）は、ここでは何もせず
            //   1チャンク分揃うまで待ち続ける。これをしないと、端数が
            //   AvailableToRead()に残り続け、呼び出し側のEOF検知
            //  （AvailableToRead()==0待ち）が永久に成立しなくなる。
            if (!m_finalPartialFlushed && available > 0 && m_eofQuery && m_eofQuery()) {
                size_t got = m_sourceRing->Read(s32Chunk.data(), available);
                if (got < samplesPerChunk) {
                    std::fill(s32Chunk.begin() + static_cast<ptrdiff_t>(got), s32Chunk.end(), 0);
                }
                m_finalPartialFlushed = true;
                outputChunk();
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        size_t got = m_sourceRing->Read(s32Chunk.data(), samplesPerChunk);
        if (got < samplesPerChunk) {
            // 通常到達しない想定（単一消費者のためAvailableToRead()確認後に
            // 減ることはない）。保険として、取れた分はゼロ埋めして出力する
            // （Read()は既に取り出した分をリングバッファから削除済みのため、
            //  ここで捨てると音が消失する）。
            std::fill(s32Chunk.begin() + static_cast<ptrdiff_t>(got), s32Chunk.end(), 0);
        }
        m_finalPartialFlushed = false; // 通常のフルチャンクが出せたので次の終端に備えリセット

        outputChunk();
    }
}
