#include "PcmDualEngine.h"
#include "FlacPcmDecoder.h"
#include "WavPcmDecoder.h"
#include "AiffPcmDecoder.h"
#include "WavPackPcmDecoder.h"
#include "MfPcmDecoder.h"
#include "OggVorbisPcmDecoder.h"
#include "OpusPcmDecoder.h"
#include "DsdPcmDecoder.h"
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <windows.h>
#include <avrt.h>

namespace {
    // ★ 調査用デバッグ出力（OutputDebugStringA経由。Visual Studioの
    //   「出力」ウィンドウ／DebugViewで確認できる）。engine層はQtに
    //   依存しない設計のため、qDebug()ではなくこちらを使う。
    void DbgLog(const char* fmt, ...) {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
    }
}

// ★ v10: 拡張子に応じたデコーダーを作って開く（通常再生とギャップレス先読みで共用）。
//   開けなければnullptr（呼び出し側でmpv経路／通常の再オープンにフォールバック）。
//   ".ogg"は中身がVorbisとOpusの両方ありうるので、Vorbis→Opusの順に試す。
static std::unique_ptr<IPcmDecoder> OpenDecoderFor(const std::wstring& filePath, const std::string& extLower)
{
    auto tryOpen = [&](std::unique_ptr<IPcmDecoder> d) -> std::unique_ptr<IPcmDecoder> {
        if (d && d->Open(filePath)) return d;
        return nullptr;
    };
    if (extLower == "flac")                          return tryOpen(std::make_unique<FlacPcmDecoder>());
    if (extLower == "wav")                           return tryOpen(std::make_unique<WavPcmDecoder>());
    if (extLower == "aiff" || extLower == "aif")     return tryOpen(std::make_unique<AiffPcmDecoder>());
    if (extLower == "wv")                            return tryOpen(std::make_unique<WavPackPcmDecoder>());
    // MP3/AAC/M4A(ALAC含む)はWindows Media Foundationでデコード
    if (extLower == "mp3" || extLower == "m4a" || extLower == "aac")
                                                     return tryOpen(std::make_unique<MfPcmDecoder>());
    if (extLower == "opus")                          return tryOpen(std::make_unique<OpusPcmDecoder>());
    // DSD(DSF/DFF)はPCM(176.4k/192k)へ変換して出力
    if (extLower == "dsf" || extLower == "dff")      return tryOpen(std::make_unique<DsdPcmDecoder>());
    if (extLower == "ogg") {
        if (auto d = tryOpen(std::make_unique<OggVorbisPcmDecoder>())) return d;
        return tryOpen(std::make_unique<OpusPcmDecoder>());
    }
    return nullptr; // 未対応フォーマット
}

PcmDualEngine::PcmDualEngine() = default;
PcmDualEngine::~PcmDualEngine() { Close(); }

bool PcmDualEngine::Open(const std::wstring& filePath, const std::string& extLower) {
    Close();

    m_decoder = OpenDecoderFor(filePath, extLower);
    if (!m_decoder) return false; // 未対応・開けない場合は呼び出し側でmpv経路にフォールバック

    m_nativeSampleRate = m_decoder->GetSampleRate();
    m_outputSampleRate = m_nativeSampleRate; // SetTargetSampleRate()未呼び出しならネイティブのまま
    m_channels         = m_decoder->GetChannels();
    m_bitsPerSample    = m_decoder->GetBitsPerSample();
    m_totalFrames      = m_decoder->GetTotalFrames();
    m_resamplingActive = false;

    if (m_channels != 2) {
        m_decoder->Close();
        m_decoder.reset();
        return false;
    }

    // ★ リサンプル(最大8倍程度)を見越し、リングバッファは出力レート換算で
    //   十分な容量を確保する（kPcmDecodeFrameSize*64フレーム相当を基準に、
    //   最大8倍のマージンを持たせる。通常のネイティブレート再生時は
    //   従来通りの容量のまま）。
    size_t capacityFrames = kPcmDecodeFrameSize * 64 * 8;
    if (m_outputBackend) {
        capacityFrames = static_cast<size_t>(m_outputBackend->GetBufferSize()) * 64 * 8;
    }
    m_ring = std::make_unique<AudioRingBuffer>(capacityFrames * m_channels);

    m_endOfStream = false;
    return true;
}

void PcmDualEngine::AttachOutputBackend(IAudioOutputBackend* backend) {
    m_outputBackend = backend;
}

void PcmDualEngine::SetTargetSampleRate(uint32_t targetRate) {
    if (targetRate == 0 || targetRate == m_nativeSampleRate) {
        m_resamplingActive = false;
        m_outputSampleRate = m_nativeSampleRate;
        return;
    }
    m_outputSampleRate = targetRate;
    m_resampler.Prepare(static_cast<double>(m_nativeSampleRate), static_cast<double>(targetRate));
    m_resamplingActive = true;
}

void PcmDualEngine::Close() {
    StopDecoding();
    CancelGapless();
    if (m_decoder) { m_decoder->Close(); m_decoder.reset(); }
    if (m_ring) m_ring->Clear();
    m_nativeSampleRate = m_outputSampleRate = m_channels = m_bitsPerSample = 0;
    m_totalFrames = 0;
    m_resamplingActive = false;
}

void PcmDualEngine::StartDecoding() {
    if (m_decoding || !m_decoder) return;
    m_decoding = true;
    m_endOfStream = false;
    m_decodeThread = std::thread(&PcmDualEngine::DecodeThreadProc, this);
}

void PcmDualEngine::StopDecoding() {
    if (!m_decoding) return;
    m_decoding = false;
    if (m_decodeThread.joinable()) m_decodeThread.join();
}

bool PcmDualEngine::Seek(double seconds) {
    if (!m_decoder) return false;
    if (seconds < 0.0) seconds = 0.0;

    bool wasDecoding = m_decoding.load();
    StopDecoding();
    if (m_ring) m_ring->Clear();

    // ★ シークはネイティブ（原音）レート基準のフレーム位置で行う。
    //   デコーダー自体は常にネイティブレートでデコードするため。
    uint64_t targetFrame = static_cast<uint64_t>(seconds * m_nativeSampleRate);
    if (targetFrame > m_totalFrames) targetFrame = m_totalFrames;

    bool ok = m_decoder->SeekToFrame(targetFrame);
    m_endOfStream = false;

    // ★ シークで連続性が失われるため、リサンプラーの内部状態（履歴・
    //   読み出し位置）もリセットする。しないと直前の音声との補間が
    //   混ざってノイズになる。
    if (m_resamplingActive) m_resampler.Reset();

    if (wasDecoding) StartDecoding();
    return ok;
}

// ── ギャップレス再生 ────────────────────────────────────────
PcmDualEngine::GaplessInfo PcmDualEngine::PrepareGaplessNext(const std::wstring& filePath, const std::string& extLower) {
    GaplessInfo info;

    std::unique_ptr<IPcmDecoder> dec = OpenDecoderFor(filePath, extLower);
    if (!dec) return info; // 未対応・開けない場合は通常の再オープンにフォールバック
    if (dec->GetChannels() != 2) { dec->Close(); return info; }

    info.ok               = true;
    info.nativeSampleRate = dec->GetSampleRate();
    info.bitsPerSample    = dec->GetBitsPerSample();
    info.totalFrames      = dec->GetTotalFrames();

    std::lock_guard<std::mutex> lock(m_gaplessMutex);
    // ★ 曲の開始直後にも呼ぶようになったため、曲終端間際の再呼び出し
    //   （checkGaplessPreload経由）と二重に準備される場合がある。
    //   前回分が未消費のまま残っていればファイルハンドルを確実に閉じてから
    //   差し替える（開けっぱなしのリーク防止）。
    if (m_nextDecoder) { m_nextDecoder->Close(); m_nextDecoder.reset(); }
    m_nextDecoder           = std::move(dec);
    m_nextNativeSampleRate  = info.nativeSampleRate;
    m_nextBitsPerSample     = info.bitsPerSample;
    m_nextTotalFrames       = info.totalFrames;
    m_nextResamplingActive  = false; // ConfirmGaplessTarget()で確定するまでは無効扱い
    return info;
}

void PcmDualEngine::ConfirmGaplessTarget(uint32_t targetRate) {
    std::lock_guard<std::mutex> lock(m_gaplessMutex);
    if (!m_nextDecoder) return;

    if (targetRate == 0 || targetRate == m_nextNativeSampleRate) {
        m_nextResamplingActive = false;
    } else {
        m_nextResampler.Prepare(static_cast<double>(m_nextNativeSampleRate), static_cast<double>(targetRate));
        m_nextResamplingActive = true;
    }
}

void PcmDualEngine::CancelGapless() {
    std::lock_guard<std::mutex> lock(m_gaplessMutex);
    if (m_nextDecoder) { m_nextDecoder->Close(); m_nextDecoder.reset(); }
    m_nextNativeSampleRate = 0;
    m_nextBitsPerSample    = 0;
    m_nextTotalFrames      = 0;
    m_nextResamplingActive = false;
}

PcmDualEngine::GaplessInfo PcmDualEngine::LastGaplessTransitionInfo() const {
    std::lock_guard<std::mutex> lock(m_gaplessMutex);
    GaplessInfo info;
    info.ok                  = true;
    info.nativeSampleRate    = m_lastTransitionNativeSampleRate;
    info.bitsPerSample       = m_lastTransitionBitsPerSample;
    info.totalFrames         = m_lastTransitionTotalFrames;
    info.backlogOutputFrames = m_lastTransitionBacklogFrames;
    return info;
}
// ────────────────────────────────────────────────────────────

void PcmDualEngine::DecodeThreadProc() {
    // ★ 改善: デコードスレッドもMMCSS「Pro Audio」特性を与えて優先度を
    //   上げる。RTスレッド(WasapiExclusiveOutput::RenderThreadProc)だけを
    //   優先度アップしていると、このデコードスレッドがOSにプリエンプトされて
    //   リングバッファへの供給が一瞬遅れ、それが結果的にRT側の周期的な
    //   遅延(catch-up)として観測されることがあるため（TEST_Playerで
    //   検証済みの修正をこちらにも反映）。
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    std::vector<int32_t> interleaved(kPcmDecodeFrameSize * m_channels);

    // リサンプル有効時の出力バッファ：最悪ケース（比較的大きいアップサンプル比）
    // でも1回のデコード分をまかなえるよう、余裕を持ったサイズを確保する。
    std::vector<int32_t> resampledBuf;
    if (m_resamplingActive) {
        const double ratio = (m_resampler.GetStep() > 0.0) ? (1.0 / m_resampler.GetStep()) : 1.0;
        size_t maxOutFrames = static_cast<size_t>(kPcmDecodeFrameSize * ratio) + 64;
        resampledBuf.resize(maxOutFrames * m_channels);
    }

    while (m_decoding) {
        uint64_t framesRead = m_decoder->ReadFrames(interleaved.data(), kPcmDecodeFrameSize);
        if (framesRead == 0) {
            // ★ ストリーム終端：リサンプラー内部に残っている端数フレームを
            //   flushして書き出してから終了する。
            if (m_resamplingActive) {
                size_t producedFrames = m_resampler.Flush(resampledBuf.data(), resampledBuf.size() / m_channels);
                if (producedFrames > 0) {
                    size_t samplesToWrite = producedFrames * m_channels;
                    size_t written = 0;
                    while (m_decoding && written < samplesToWrite) {
                        written += m_ring->Write(resampledBuf.data() + written, samplesToWrite - written);
                        if (written < samplesToWrite) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            }

            // ★ ギャップレス：次曲が事前オープン済みなら、WASAPIストリーム／
            //   リングバッファを一切止めずにデコード元を差し替えて継続する
            //   （出力レートが変わらない前提はConfirmGaplessTarget()を呼んだ
            //    呼び出し側が保証済み）。
            {
                std::lock_guard<std::mutex> glock(m_gaplessMutex);
                if (m_nextDecoder) {
                    // ★ この時点でリングバッファに残っている分は、すべて前の曲の
                    //   flush済みテール（まだ次曲は1サンプルも書き込んでいない）。
                    //   これが実際にWASAPIから鳴り終わるまでは、まだ「前の曲を
                    //   再生中」なので、呼び出し側にはこの分の秒数だけUI更新を
                    //   遅らせてもらう。
                    const uint64_t backlogFrames = m_ring ? (m_ring->AvailableToRead() / m_channels) : 0;

                    m_decoder           = std::move(m_nextDecoder);
                    m_nativeSampleRate  = m_nextNativeSampleRate;
                    m_bitsPerSample     = m_nextBitsPerSample;
                    m_totalFrames       = m_nextTotalFrames;
                    m_resamplingActive  = m_nextResamplingActive;
                    if (m_resamplingActive) {
                        m_resampler = std::move(m_nextResampler);
                    }
                    m_nextNativeSampleRate = 0;
                    m_nextBitsPerSample    = 0;
                    m_nextTotalFrames      = 0;
                    m_nextResamplingActive = false;

                    m_lastTransitionNativeSampleRate = m_nativeSampleRate;
                    m_lastTransitionBitsPerSample    = m_bitsPerSample;
                    m_lastTransitionTotalFrames      = m_totalFrames;
                    m_lastTransitionBacklogFrames    = backlogFrames;

                    // リサンプル比が変わった可能性があるので出力バッファを再計算
                    if (m_resamplingActive) {
                        const double ratio = (m_resampler.GetStep() > 0.0) ? (1.0 / m_resampler.GetStep()) : 1.0;
                        size_t maxOutFrames = static_cast<size_t>(kPcmDecodeFrameSize * ratio) + 64;
                        resampledBuf.resize(maxOutFrames * m_channels);
                    }

                    m_gaplessTransitionCount.fetch_add(1, std::memory_order_release);
                    DbgLog("[PcmDualEngine] GAPLESS SWAP: backlog=%llu frames, newNativeRate=%u",
                           static_cast<unsigned long long>(backlogFrames), m_nativeSampleRate);
                    continue; // 次曲のデコードへシームレスに継続
                }
            }

            DbgLog("[PcmDualEngine] TRUE EOF (no next decoder armed at this moment)");
            m_endOfStream = true;
            break;
        }

        const int32_t* outData = interleaved.data();
        size_t outFrames = static_cast<size_t>(framesRead);

        if (m_resamplingActive) {
            outFrames = m_resampler.Process(interleaved.data(), static_cast<size_t>(framesRead),
                                             resampledBuf.data(), resampledBuf.size() / m_channels);
            outData = resampledBuf.data();
            if (outFrames == 0) continue; // 先読みが溜まるまで出力0フレームのこともある（正常）
        }

        size_t samplesToWrite = outFrames * m_channels;
        size_t written = 0;
        while (m_decoding && written < samplesToWrite) {
            written += m_ring->Write(outData + written, samplesToWrite - written);
            if (written < samplesToWrite) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
}
