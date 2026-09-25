// ★ dr_flacの実装本体はこのファイルでのみコンパイルする（1回だけ）。
//   このdefineは、下のFlacDualEngine.h内の#include "dr_flac.h"より
//   必ず先に来ている必要がある。
#define DR_FLAC_IMPLEMENTATION
#include "FlacDualEngine.h"
#include <vector>
#include <chrono>

FlacDualEngine::FlacDualEngine() = default;
FlacDualEngine::~FlacDualEngine() { Close(); }

bool FlacDualEngine::Open(const std::wstring& filePath) {
    Close();

    m_flac = drflac_open_file_w(filePath.c_str(), nullptr);
    if (!m_flac) return false;

    m_sampleRate = m_flac->sampleRate;
    m_channels = m_flac->channels;
    m_bitsPerSample = m_flac->bitsPerSample;
    m_totalFrames = m_flac->totalPCMFrameCount;

    if (m_channels != 2) {
        drflac_close(m_flac);
        m_flac = nullptr;
        return false;
    }

    size_t capacityFrames = kDecodeFrameSize * 64;
    if (m_outputBackend) {
        capacityFrames = static_cast<size_t>(m_outputBackend->GetBufferSize()) * 64;
    }
    m_ring = std::make_unique<AudioRingBuffer>(capacityFrames * m_channels);

    m_endOfStream = false;
    return true;
}

void FlacDualEngine::AttachOutputBackend(IAudioOutputBackend* backend) {
    m_outputBackend = backend;
}

void FlacDualEngine::Close() {
    StopDecoding();
    if (m_flac) { drflac_close(m_flac); m_flac = nullptr; }
    if (m_ring) m_ring->Clear();
}

void FlacDualEngine::StartDecoding() {
    if (m_decoding || !m_flac) return;
    m_decoding = true;
    m_endOfStream = false;
    m_decodeThread = std::thread(&FlacDualEngine::DecodeThreadProc, this);
}

void FlacDualEngine::StopDecoding() {
    if (!m_decoding) return;
    m_decoding = false;
    if (m_decodeThread.joinable()) m_decodeThread.join();
}

bool FlacDualEngine::Seek(double seconds) {
    if (!m_flac) return false;
    if (seconds < 0.0) seconds = 0.0;

    bool wasDecoding = m_decoding.load();
    StopDecoding();
    if (m_ring) m_ring->Clear();

    drflac_uint64 targetFrame = static_cast<drflac_uint64>(seconds * m_sampleRate);
    if (targetFrame > m_totalFrames) targetFrame = m_totalFrames;

    drflac_bool32 ok = drflac_seek_to_pcm_frame(m_flac, targetFrame);
    m_endOfStream = false;

    if (wasDecoding) StartDecoding();
    return ok != 0;
}

void FlacDualEngine::DecodeThreadProc() {
    std::vector<int32_t> interleaved(kDecodeFrameSize * m_channels);

    while (m_decoding) {
        drflac_uint64 framesRead = drflac_read_pcm_frames_s32(
            m_flac, kDecodeFrameSize, interleaved.data());

        if (framesRead == 0) { m_endOfStream = true; break; }

        size_t samplesToWrite = static_cast<size_t>(framesRead) * m_channels;
        size_t written = 0;
        while (m_decoding && written < samplesToWrite) {
            written += m_ring->Write(interleaved.data() + written, samplesToWrite - written);
            if (written < samplesToWrite) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
}
