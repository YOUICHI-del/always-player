// ★ dr_flacの実装本体はこのファイルでのみコンパイルする（1回だけ）。
//   このdefineは、下のFlacPcmDecoder.h内の#include "dr_flac.h"より
//   必ず先に来ている必要がある。
#define DR_FLAC_IMPLEMENTATION
#include "FlacPcmDecoder.h"

FlacPcmDecoder::FlacPcmDecoder() = default;
FlacPcmDecoder::~FlacPcmDecoder() { Close(); }

bool FlacPcmDecoder::Open(const std::wstring& filePath) {
    Close();
    m_flac = drflac_open_file_w(filePath.c_str(), nullptr);
    if (!m_flac) return false;

    m_sampleRate    = m_flac->sampleRate;
    m_channels      = m_flac->channels;
    m_bitsPerSample = m_flac->bitsPerSample;
    m_totalFrames   = m_flac->totalPCMFrameCount;

    if (m_channels != 2) {
        drflac_close(m_flac);
        m_flac = nullptr;
        return false;
    }
    return true;
}

void FlacPcmDecoder::Close() {
    if (m_flac) { drflac_close(m_flac); m_flac = nullptr; }
}

uint64_t FlacPcmDecoder::ReadFrames(int32_t* out, uint64_t frameCount) {
    if (!m_flac) return 0;
    return drflac_read_pcm_frames_s32(m_flac, frameCount, out);
}

bool FlacPcmDecoder::SeekToFrame(uint64_t frame) {
    if (!m_flac) return false;
    return drflac_seek_to_pcm_frame(m_flac, frame) != 0;
}
