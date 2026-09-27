#pragma once
#include <cstdint>
#include <memory>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QMutex>
#include <QElapsedTimer>
#include <QVector>
#include <QSet>
#include <mpv/client.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/flacfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/wavfile.h>
#include "WasapiLevelMeter.h"
#include "PcmDualEngine.h"
#include "WasapiExclusiveOutput.h"
#include "AudioProcessThread.h"
#include "DspChain.h"
#include "AudioDeviceWatcher.h"

class QTimer;

class Player : public QObject
{
    Q_OBJECT

public:
    explicit Player(QObject *parent = nullptr);
    ~Player();

    bool init();
    void preWarm();
    void loadFolder(const QString &path);
    void loadFile(const QString &filePath);  // 一曲再生
    void loadPlaylist(const QStringList &paths);  // ★ CD再生用に追加
    void loadCdStream(const QString &filePath);
    void setMediaTitle(const QString &title);       // ★ Named Pipe経由CDストリーミング
    void loadCdDirect(const QString &driveLetter);     // ★ First Mode：mpv直接CD再生
    void appendPlaylist(const QStringList &paths); // 再生中断なしでプレイリスト更新
    void clearPlaylist();                              // ★ プレイリストを完全クリア
    void play(int index = -1);
    void pause();
    void resume();
    void stop();
    void next();
    void prev();
    void setVolume(int vol);
    void setMode(const QString &mode, bool hp1 = false, bool hp2 = false, const QString &soundField = QString());
    void setModeQuiet(const QString &mode) { m_mode = mode; }
    void setDspOff(bool off);   // DSP完全バイパス（SAEC・高調波・アップサンプリングすべて無効）
    bool dspOff() const { return m_dspOff; }
    void setChainOn(bool on); // 中密度チェーン・高調波のみON/OFF（アップサンプリングは維持）
    bool chainOn() const { return m_chainOn; }
    void setBitPerfectAuto(bool on);  // ビットパーフェクト自動化（CD/MP3→dsd8、ハイレゾ→pure）
    bool bitPerfectAuto() const { return m_bpAutoOn; }

    // ★ MainWindowの「16種類の手動ビットパーフェクト」メニューで特定の
    //   レート/ビット数を固定選択している間、trueにする。
    //   true の間、applyAudioChain()はaudio-samplerateを一切上書きしない
    //   （m_modeに基づく自動レート変更で、MainWindow側の手動固定レートが
    //    次の曲で踏み潰されてしまうバグの修正）。
    void setManualRateOverride(bool active) { m_manualRateOverride = active; }
    bool manualRateOverride() const { return m_manualRateOverride; }

    // ── 新エンジン(PcmDualEngine)統合
    // ★ 対応フォーマット(FLAC/WAV/AIFF/WavPack)は自作エンジン(WASAPI排他・
    //   ビットパーフェクト)で再生を試み、成功した間だけtrueを返す。
    //   それ以外（未対応フォーマット、mpv経路）はfalse。
    bool isUsingNewEngineNow() const { return m_useNewEngine; }
    // ★ 実際にWASAPIへ出力しているレート（dsd8/hires4でアップサンプリング
    //   済みならその値）。BitPerfectボタンの表示専用。m_cachedSr（getInfo()
    //   が使う「原音」レート）とは意図的に別管理にしている
    //  （両者を混同するとgetInfo()側のhiRes判定が循環してしまうため）。
    uint32_t newEngineActualSampleRate() const { return static_cast<uint32_t>(m_newEngineOutputSr); }
    uint32_t newEngineActualBits() const { return static_cast<uint32_t>(m_cachedBits); }

    // ★ シーク：新エンジン再生中はPcmDualEngine::Seek()、それ以外はmpvの
    //   time-posプロパティへ委譲する。
    //   ※音量調整は新エンジン側に意図的に未実装（Windows/DAC側で調整する設計）。
    void seekTo(double seconds);
    void setAudioDevice(const QString &deviceId);
    void setBitPerfectMeter(bool on);  // VUメーター疑似モード切替
    void disableBitPerfect();  // 排他モード解除（stop→設定→再生は呼び出し側で行う）

    // ── v3.0追加
    enum class RepeatMode  { None, One, All };
    enum class ShuffleMode { None, Folder, Favorites };

    void setRepeat(RepeatMode mode)  { m_repeatMode = mode; }
    void setShuffle(ShuffleMode mode);
    void setFavoritePaths(const QStringList &paths) { m_favPaths = paths; }

    RepeatMode  repeatMode()  const { return m_repeatMode; }
    ShuffleMode shuffleMode() const { return m_shuffleMode; }

    bool    isPlaying()     const { return m_playing; }
    bool    isPaused()      const { return m_paused; }
    int     currentIndex()  const { return m_currentIndex; }
    int     total()         const { return m_playlist.size(); }
    QString currentFile()     const;
    QString currentFilePath() const;
    QString fileAt(int i)   const;
    QString filePathAt(int i) const;  // フルパスを返す
    QString getInfo(const QString &mode = QString()) const;
    QString getTagTitle()   const;
    QString getTagArtist()  const;
    void    getAudioLevels(float &left, float &right) const;
    QString getCoverArt()   const;
    QString lastFolder()    const { return m_lastFolder; }
    QString loadLastFolder();
    double  getPosition()   const;
    double  getDuration()   const;
    QString mode()          const { return m_mode; }
    mpv_handle *mpvHandle()  const { return m_mpv; }
    int     volume()        const { return m_volume; }
    int     cachedSr()      const { return m_cachedSr; }
    int     cachedBr()      const { return m_cachedBr; }
    void    setCachedInfo(int br, int sr, int bits) { m_cachedBr = br; m_cachedSr = sr; m_cachedBits = bits; }

signals:
    void trackChanged(int index, const QString &filename,
                       const QString &title, const QString &artist);
    void playbackStarted();
    void playbackStopped();
    void playbackPaused();
    void errorOccurred(const QString &msg);

private:
    void applyAudioChain();
    void applyAudioChainAndReload();  // 再生中の音質切り替え：チェーン適用＋シークリロード
    QStringList collectFiles(const QString &folder, int depth = 0);
    void mpvEventLoop();

    // ── 新エンジン(PcmDualEngine)統合
    // 対応フォーマット(FLAC/WAV/AIFF/WavPack)を自作エンジン(WASAPI排他)で再生開始する。
    // 排他モード確保失敗・未対応フォーマットなど何らかの理由で開始できなければ
    // falseを返し、呼び出し側は従来通りmpv経路にフォールバックする。
    bool tryPlayViaNewEngine(const QString &filePath);
    void stopNewEngine();
    void checkNewEngineEof();  // 新エンジン再生中のEOFポーリング（タイマー）

    // ★ アップサンプリング(dsd8/hires4)：ネイティブレートから目標レートを
    //   決定する（m_dspOff/m_modeを見る）。レート変更にはWASAPI排他ストリーム
    //   の再オープンが必要なため、再生中のモード/dspOff切り替え時は
    //   reloadNewEngineForRateChange()で位置を保つ再オープンを行う。
    uint32_t computeNewEngineTargetRate(uint32_t nativeRate) const;
    void reloadNewEngineForRateChange();

    // ★ ギャップレス再生：次曲の出力フォーマット（アップサンプリング後の
    //   レート）が現在再生中の曲と完全一致する場合のみ、PcmDualEngine側に
    //   次曲デコーダーを事前オープンして渡す。一致しなければ何もしない
    //   （＝従来通りEOF時の再オープンにフォールバックする）。
    void tryPrepareGaplessNext(const QString &nextPath);
    // デコードスレッドが内部でシームレスに次曲へ切り替わったかをポーリングで
    // 検知し、Player側のインデックス・長さ・タグ等の状態を追いつかせる。
    void checkGaplessTransition();
    // ★ PcmDualEngineの内部スワップ検知1回分の処理（次曲インデックス確定・
    //   次の次曲の再アーム・表示チェーンへの追記）。checkGaplessTransition()
    //   から遷移検知時にのみ呼ばれる。
    void onGaplessTransitionDetected();
    // ★ 経過壁時計とm_gaplessChainを突き合わせて求めた表示状態へ、
    //   m_currentIndex等のUI状態を追いつかせる。checkGaplessTransition()の
    //   毎回のポーリングから呼ばれる（変化がなければ何もしない）。
    void applyGaplessDisplayCatchup();
    // play()／checkGaplessTransition()共通のタグ非同期読み取り処理。
    void fetchTagsAsync(const QString &filePath, int index, int serial);
    // ★ peekNextIndex()の本体。基準インデックスを外から渡せるようにし、
    //   UI表示用のm_currentIndex（遅延更新）と、デコードスレッドが実際に
    //   今再生している曲を指すm_gaplessEngineIndex（即時更新）のどちらを
    //   基準にしても次曲を計算できるようにする（詳細はm_gaplessEngineIndex参照）。
    int peekNextIndexFrom(int baseIndex) const;
    uint64_t m_gaplessTransitionSeen = 0; // PcmDualEngine::GetGaplessTransitionCount()の既読カウント
    // ★ デコードスレッドが「実際に今デコード・再生している」曲のプレイリスト上の
    //   インデックス。play()開始時とcheckGaplessTransition()での遷移検知時に
    //   "即座に" 更新される（UI表示用のm_currentIndexとは非同期に進む）。
    //   次曲の事前準備（checkGaplessPreload()/tryPrepareGaplessNext()）は必ず
    //   こちらを基準にpeekNextIndexFrom()で次曲を計算する。
    int m_gaplessEngineIndex = -1;

    // ★★ ギャップレス表示モデル（壁時計ベース）★★
    //   以前は「内部スワップを検知した時点でbacklog分の時間だけ
    //   QTimer::singleShot()でUI更新を遅延させる」という、遷移1回ごとに
    //   個別のタイマーを積む方式だった。しかし短い曲が連続してデコード
    //   スレッドが一気に数曲先まで進んでしまうケース（テスト用の短い曲、
    //   あるいは高速なストレージでの実曲）では、複数の遅延タイマーが
    //   同時に積み上がり、「どれを適用しどれを捨てるか」の判定が破綻して
    //   曲の表示が丸ごと1曲飛ばされる不具合が実機デバッグログで
    //   繰り返し観測された。
    //   そこで、個別のタイマーで「後から追いつかせる」のをやめ、
    //   チェーン開始からの経過時間（壁時計）を唯一の真実の源とし、
    //   「今どの曲の何秒目か」を都度その場で計算する方式に変更した。
    //   m_gaplessChainは、現在の連続ギャップレス再生チェーンにおける
    //   各曲の長さを、実際に鳴る順番に追記していくだけの配列。
    //   内部スワップを検知したら「即座に」ここへ1件追記するだけでよく、
    //   タイマーもスケジューリングも不要。表示側（getPosition/getDuration/
    //   m_currentIndexの追従）は、経過時間がこの配列のどの区間に
    //   属するかを毎回計算するだけなので、複数の遷移が積み重なっても
    //   自然に正しい曲へたどり着く（取りこぼしが原理的に起きない）。
    struct GaplessChainEntry {
        int index;                 // プレイリスト上のインデックス
        double durationSec;        // この曲のネイティブ長（秒）
        uint32_t nativeSampleRate; // getInfo()/m_cachedSr用
        uint32_t bitsPerSample;    // getInfo()/m_cachedBits用
    };
    QVector<GaplessChainEntry> m_gaplessChain;
    struct GaplessDisplayState {
        int index = -1;
        double posInTrack = 0.0;
        double durationSec = 0.0;
        uint32_t nativeSampleRate = 0;
        uint32_t bitsPerSample = 0;
    };
    // ★ m_newEnginePositionBase（チェーン開始または直近のpause時点までの
    //   累積経過秒数）とm_newEngineElapsedTimer（再生中の経過時間）から、
    //   「今どの曲の何秒目を表示すべきか」をm_gaplessChainと突き合わせて
    //   計算する。getPosition()/getDuration()/checkGaplessTransition()の
    //   表示追従ロジックが共通で使う。
    GaplessDisplayState computeGaplessDisplayState() const;

    PcmDualEngine                       m_pcmEngine;
    WasapiExclusiveOutput                m_newEngineOutput;
    std::unique_ptr<AudioProcessThread>  m_newEngineProcessThread;
    QElapsedTimer                        m_newEngineElapsedTimer;
    double                               m_newEnginePositionBase = 0.0;
    double                               m_newEngineDuration     = 0.0;
    QTimer                              *m_newEngineEofTimer     = nullptr;
    bool                                 m_newEngineEofFired     = false;  // 二重next()防止

    // ★ 新エンジン用DSPチェーン（同レート処理のみ対応。詳細はDspChain.h参照）
    DspChain                             m_newEngineDsp;
    static SoundFieldMode soundFieldModeFromString(const QString &s);

    // ── 出力デバイス切替時の爆音防止 ─────────────────────────
    // 既定の再生デバイスが切り替わったら即座に停止し、
    // 「Always Playerを停止させました。再生をしてください」と知らせる。
    // 自動再開はしない（排他モードの爆音・内蔵スピーカーでの誤再生を防ぐ）。
    void onDefaultDeviceChangedRaw(const char *source);   // 切替検知（即時停止）。sourceは"notify"/"poll"
    void deviceLog(const QString &line);                    // 診断ログ（exeと同じフォルダのdevice_debug.log）
    int  m_deviceQueryFailLogged = 0;                       // 取得失敗ログの出しすぎ防止
    void onDefaultDeviceSettled();      // デバウンス後（メッセージ表示）
    AudioDeviceWatcher m_deviceWatcher;
    QTimer   *m_deviceSettleTimer      = nullptr;
    QTimer   *m_devicePollTimer        = nullptr;  // 通知が届かない環境向けの保険（1秒ごとに既定デバイスIDを比較）
    QString   m_currentDeviceId;                  // 最後に確認した既定デバイスID
    bool      m_stoppedByDeviceChange  = false;   // 切替で停止した（メッセージ待ち）
    bool      m_deviceDialogOpen       = false;   // メッセージ表示中（再入防止）

    // ── 簡易ギャップレス（v8簡易版。本格対応はv9自作エンジンで実施）
    // 次曲切り替え時のWASAPI再初期化遅延を、事前準備で短縮する。
    // サンプル単位の継ぎ目なし再生ではなく、体感の途切れを減らすための延命措置。
    void checkGaplessPreload();           // 再生位置ポーリング（タイマー）
    int  peekNextIndex() const;           // next()と同じロジックで「次に鳴る曲」を非破壊で取得
    void preloadNextFile(const QString &path); // OSファイルキャッシュ温め＋タグ事前読み込み
    QTimer     *m_preloadTimer      = nullptr;
    QString     m_preloadedPath;          // 事前読み込み済みファイルパス
    bool        m_preloadDone       = false; // 現在の曲について事前読み込み済みか

    mpv_handle  *m_mpv          = nullptr;
    QStringList  m_playlist;
    int          m_currentIndex = 0;
    bool         m_playing      = false;
    bool         m_paused       = false;
    int          m_volume       = 100;
    QString      m_mode         = "dsd8";
    QString      m_lastFolder;
    bool         m_hp1          = false;
    bool         m_hp2          = false;
    bool         m_dspOff       = false;  // DSP完全バイパスフラグ
    bool         m_chainOn      = true;   // 中密度チェーン（デフォルトON）
    bool         m_bpAutoOn     = false;  // ビットパーフェクト自動化フラグ
    bool         m_useNewEngine = false;  // 新エンジン(PcmDualEngine)使用中フラグ
    bool         m_manualRateOverride = false;  // MainWindow側で手動レート固定中フラグ
    QString      m_soundField;
    QMutex       m_mutex;
    RepeatMode   m_repeatMode  = RepeatMode::None;
    ShuffleMode  m_shuffleMode = ShuffleMode::None;
    QList<int>   m_shuffleList;
    int          m_shufflePos  = 0;
    QStringList  m_favPaths;

    static const QStringList SUPPORTED_EXT;
    static const QStringList NEW_ENGINE_EXT;  // PcmDualEngine(WASAPI排他)対応フォーマット

    // リアルタイムVUメーター（WASAPI ループバック経由）
    WasapiLevelMeter *m_levelMeter = nullptr;

    // infoラベル用キャッシュ（play()時に更新）
    // ★ m_cachedSrは常に「原音（ネイティブ）」レート。getInfo()のdispMode
    //   判定（hiRes等）がこれを前提にしているため、新エンジンのアップ
    //   サンプリング先レートで上書きしてはいけない。
    int m_cachedBr   = 0;
    int m_realtimeBr = 0;
    int m_cachedSr   = 0;
    int m_cachedBits = 0;
    // ★ 新エンジンが実際にWASAPIへ出力しているレート（BitPerfect表示専用）。
    //   dsd8/hires4でアップサンプリングしていればm_cachedSrと異なる値になる。
    int m_newEngineOutputSr = 0;
    // v10: 今の出力デバイスが排他モードで受け付けなかった出力レート
    QSet<uint32_t> m_unsupportedOutRates;

    // タグ読み取りスレッドの世代管理（古いスレッド結果を破棄するためのシリアル番号）
    int m_playSerial = 0;

    // applyAudioChainAndReload中のstopによる誤EOF→next()防止フラグ
    bool m_reloading = false;
};
