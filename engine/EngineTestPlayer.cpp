// ─────────────────────────────────────────────
// EngineTestPlayer — v9自作エンジン(dr_flac + WASAPI排他)の
// 独立動作確認用コンソールアプリ。
//
// Always Player本体（Qt/mpv）には一切依存しない。
// FlacDualEngine / WasapiExclusiveOutput / AudioProcessThread
// の3点だけを使い、「フォルダを開く→再生→一時停止→停止→
// スキップ→バック」という制御フローが正しく組めるかを確認する。
//
// ここで固めたロジックは、そのまま Player.cpp への移植の
// 設計図になる（フェーズB）。
// ─────────────────────────────────────────────

#include "FlacDualEngine.h"
#include "WasapiExclusiveOutput.h"
#include "AudioProcessThread.h"

#include <filesystem>
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>
#include <memory>
#include <cwctype>

namespace fs = std::filesystem;

namespace {

const wchar_t* ResultToString(AudioBackendResult r) {
    switch (r) {
        case AudioBackendResult::Ok:                     return L"Ok";
        case AudioBackendResult::DeviceNotFound:          return L"DeviceNotFound";
        case AudioBackendResult::FormatNotSupported:      return L"FormatNotSupported";
        case AudioBackendResult::ExclusiveModeUnavailable:return L"ExclusiveModeUnavailable";
        case AudioBackendResult::DeviceLost:              return L"DeviceLost";
        default:                                          return L"UnknownError";
    }
}

// ── フォルダ直下のFLACファイルを収集（非再帰・単純比較で十分）
std::vector<std::wstring> CollectFlacFiles(const std::wstring& folder) {
    std::vector<std::wstring> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file()) continue;
        std::wstring ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".flac") files.push_back(entry.path().wstring());
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

// ─────────────────────────────────────────────
// EngineTestPlayer本体：再生状態とプレイリストを管理する
// （このクラスの構造が、そのままPlayer.cppに移植される）
// ─────────────────────────────────────────────
class EngineTestPlayer {
public:
    enum class State { Stopped, Playing, Paused };

    bool LoadFolder(const std::wstring& folder) {
        m_playlist = CollectFlacFiles(folder);
        m_currentIndex = -1;
        if (m_playlist.empty()) {
            std::wcout << L"[EngineTestPlayer] FLACファイルが見つかりません: " << folder << L"\n";
            return false;
        }
        std::wcout << L"[EngineTestPlayer] " << m_playlist.size() << L"曲を検出しました。\n";
        for (size_t i = 0; i < m_playlist.size(); ++i) {
            std::wcout << L"  [" << i << L"] " << fs::path(m_playlist[i]).filename().wstring() << L"\n";
        }
        return true;
    }

    // ── 再生（index省略時：一時停止からの再開 or 先頭から）
    void Play(int index = -1) {
        if (m_playlist.empty()) return;

        // 一時停止中に index 未指定で呼ばれた → 再開のみ
        if (index < 0 && m_state == State::Paused) {
            Resume();
            return;
        }

        if (index < 0) index = (m_currentIndex < 0) ? 0 : m_currentIndex;
        if (index < 0 || index >= static_cast<int>(m_playlist.size())) return;

        StopInternal(/*keepEngineClosed=*/false);

        m_currentIndex = index;
        const std::wstring& path = m_playlist[m_currentIndex];

        if (!m_engine.Open(path)) {
            std::wcout << L"[EngineTestPlayer] Open失敗: " << path << L"\n";
            return;
        }

        AudioFormat fmt;
        fmt.sampleRate    = m_engine.GetSampleRate();
        fmt.channels       = static_cast<uint16_t>(m_engine.GetTotalChannels());
        // ★ AudioProcessThreadはs32(4byte/sample)をそのままWriteFramesへ渡すため、
        //   バイト数を一致させるためにInt32コンテナを明示指定する。
        //   24bit専用コンテナへのパッキングは今回のテストの範囲外（次の課題）。
        fmt.sampleFormat  = AudioSampleFormat::Int32;

        auto initResult = m_output.Initialize(fmt);
        if (initResult != AudioBackendResult::Ok) {
            std::wcout << L"[EngineTestPlayer] WASAPI排他初期化失敗: "
                       << ResultToString(initResult) << L"\n";
            return;
        }
        m_engine.AttachOutputBackend(&m_output);

        m_processThread = std::make_unique<AudioProcessThread>(m_engine.GetRingBuffer(), &m_output);
        m_processThread->SetBitPerfect(true); // DSP無し、s32そのまま出力

        m_engine.StartDecoding();
        m_output.Start();
        m_processThread->Start();

        m_state = State::Playing;
        std::wcout << L"[Playing] [" << m_currentIndex << L"] "
                   << fs::path(path).filename().wstring()
                   << L"  (" << fmt.sampleRate << L"Hz / "
                   << m_engine.GetBitsPerSample() << L"bit)\n";
    }

    void TogglePause() {
        if (m_state == State::Playing) {
            // ★ 出力とデコードの両方を止める。drflacの読み取り位置は
            //   m_flac内部に保持されるので、再開時も続きから鳴る。
            m_processThread->Stop();
            m_output.Stop();
            m_engine.StopDecoding();
            m_state = State::Paused;
            std::wcout << L"[Paused]\n";
        } else if (m_state == State::Paused) {
            Resume();
        } else {
            Play();
        }
    }

    void Stop() {
        StopInternal(/*keepEngineClosed=*/true);
        m_state = State::Stopped;
        std::wcout << L"[Stopped]\n";
    }

    void Next() {
        if (m_playlist.empty()) return;
        if (m_currentIndex + 1 < static_cast<int>(m_playlist.size())) {
            Play(m_currentIndex + 1);
        } else {
            std::wcout << L"[EngineTestPlayer] 最後の曲です。\n";
        }
    }

    void Prev() {
        if (m_playlist.empty()) return;
        if (m_currentIndex > 0) {
            Play(m_currentIndex - 1);
        } else {
            std::wcout << L"[EngineTestPlayer] 先頭の曲です。\n";
        }
    }

    void Shutdown() {
        StopInternal(/*keepEngineClosed=*/true);
    }

private:
    void Resume() {
        if (m_state != State::Paused) return;
        m_engine.StartDecoding();
        m_output.Start();
        m_processThread->Start();
        m_state = State::Playing;
        std::wcout << L"[Resumed]\n";
    }

    // keepEngineClosed=false: Play()内部から呼ばれる場合、直後にOpen()するので
    //                          後片付けのみでよい。
    // keepEngineClosed=true : 明示的なStop()操作。エンジンを完全に閉じる。
    void StopInternal(bool keepEngineClosed) {
        if (m_processThread) {
            m_processThread->Stop();
            m_processThread.reset();
        }
        m_output.Stop();
        m_output.Shutdown();
        m_engine.StopDecoding();
        if (keepEngineClosed) {
            m_engine.Close();
        }
    }

    FlacDualEngine                    m_engine;
    WasapiExclusiveOutput             m_output;
    std::unique_ptr<AudioProcessThread> m_processThread;

    std::vector<std::wstring> m_playlist;
    int   m_currentIndex = -1;
    State m_state = State::Stopped;
};

// ─────────────────────────────────────────────
int wmain(int argc, wchar_t* argv[]) {
    std::wstring folder;
    if (argc >= 2) {
        folder = argv[1];
    } else {
        std::wcout << L"フォルダパスを入力してください（FLACファイルが入っているフォルダ）: ";
        std::getline(std::wcin, folder);
    }

    EngineTestPlayer player;
    if (!player.LoadFolder(folder)) {
        return 1;
    }

    std::wcout << L"\n操作: [p]再生/一時停止  [s]停止  [n]次の曲  [b]前の曲  [q]終了\n";

    while (true) {
        std::wcout << L"> ";
        std::wstring line;
        if (!std::getline(std::wcin, line)) break;
        if (line.empty()) continue;

        wchar_t c = static_cast<wchar_t>(std::towlower(line[0]));
        switch (c) {
            case L'p': player.TogglePause(); break;
            case L's': player.Stop();        break;
            case L'n': player.Next();        break;
            case L'b': player.Prev();        break;
            case L'q': player.Shutdown(); return 0;
            default:
                std::wcout << L"[p]再生/一時停止  [s]停止  [n]次の曲  [b]前の曲  [q]終了\n";
                break;
        }
    }

    player.Shutdown();
    return 0;
}
