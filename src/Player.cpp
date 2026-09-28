#include "Player.h"
#include <shlwapi.h>  // StrCmpLogicalW（自然順ソート）
#include <intrin.h>   // __cpuid（CPUスペック判定）
#pragma comment(lib, "shlwapi.lib")
#include <cmath>
#include <algorithm>
#include <random>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QDebug>
#include <QtConcurrent>
#include <QMessageBox>
#include <QPushButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <windows.h>

// ★ ギャップレス関連デバッグログ用：実時間（起動からのミリ秒）を付与し、
//   VS出力ウィンドウのログから実際のタイミングを検証できるようにする。
//   （調査専用の一時的な計測用。恒久的な機能ではない）
static qint64 dbgMs() { return static_cast<qint64>(GetTickCount64()); }

// ★ v10: スライダー位置(0〜100)→リニア倍率。mpvと同じ3乗カーブで、
//   以前と同じ操作感にする。100なら厳密に1.0（ビットパーフェクトのまま）。
static double volumeToGain(int vol)
{
    if (vol >= 100) return 1.0;
    if (vol <= 0)   return 0.0;
    const double x = vol / 100.0;
    return x * x * x;
}



const QStringList Player::SUPPORTED_EXT = {
    "mp3","aac","ogg","wav","flac","opus","dsf","dff","m4a","aiff","wv"
};

// ★ PcmDualEngine(WASAPI排他・ビットパーフェクト)で再生を試みるフォーマット。
//   ここに無いフォーマットは常にmpv経路で再生される。
//   ("aif"はAIFFの別拡張子表記。"wv"はWavPack)
//   v10: "mp3"/"m4a"/"aac" はWindows Media Foundation(MfPcmDecoder)で追加
//        "ogg"(Vorbis/Opus) は stb_vorbis／libopusfile、"opus" は libopusfile で追加
//        "dsf"/"dff" は DsdPcmDecoder（DSD→PCM 176.4k/192k 変換）で追加
const QStringList Player::NEW_ENGINE_EXT = {
    "flac", "wav", "aiff", "aif", "wv", "mp3", "m4a", "aac", "ogg", "opus", "dsf", "dff"
};

Player::Player(QObject *parent) : QObject(parent) {}

Player::~Player()
{
    // ★ COMスレッドからの通知が破棄後のPlayerへ届かないよう、最初に登録解除する。
    m_deviceWatcher.Stop();
    stop();
    // ④ init()でtimeBeginPeriod(1)したものを返却
    timeEndPeriod(1);
}

bool Player::init()
{
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

    // ── 簡易ギャップレス：曲終端が近づいたら次曲のOSファイルキャッシュを温める
    m_preloadTimer = new QTimer(this);
    connect(m_preloadTimer, &QTimer::timeout, this, &Player::checkGaplessPreload);
    m_preloadTimer->start(1000);

    // ★ 新エンジン(PcmDualEngine)再生中のEOF検知用。
    //   mpvはMPV_EVENT_END_FILEイベントで自動的にEOFを検知するが、新エンジンには
    //   そのようなイベント通知が無いため、短い間隔でポーリングして曲終端を検出し、
    //   mpv経路と同様に自動的にnext()へ進める。
    m_newEngineEofTimer = new QTimer(this);
    // ★ ギャップレス再生：デコードスレッドが内部でシームレスに次曲へ
    //   切り替わったこと（PcmDualEngine::GetGaplessTransitionCount()の増分）を
    //   同じ間隔でポーリングし、Player側のインデックス・長さ・タグ表示を
    //   追いつかせる。checkNewEngineEof()より先に接続し、遷移直後の
    //   トラック状態でEOF判定が行われるようにする（QObject::connect()は
    //   同一シグナルに対して接続順にスロットを呼び出す）。
    connect(m_newEngineEofTimer, &QTimer::timeout, this, &Player::checkGaplessTransition);
    connect(m_newEngineEofTimer, &QTimer::timeout, this, &Player::checkNewEngineEof);
    m_newEngineEofTimer->start(100);

    // ★ 出力デバイス切替時の爆音防止（詳細はPlayer.h参照）。
    //   Windowsは切替1回で複数の通知を短時間に連発することがあるため、
    //   再生停止は「最初の通知で即座に」、ダイアログは「通知が落ち着いてから」
    //   （400msのデバウンス後）に1回だけ出す。
    m_deviceSettleTimer = new QTimer(this);
    m_deviceSettleTimer->setSingleShot(true);
    m_deviceSettleTimer->setInterval(400);
    connect(m_deviceSettleTimer, &QTimer::timeout, this, &Player::onDefaultDeviceSettled);
    const bool watcherOk = m_deviceWatcher.Start([this](const std::wstring &){
        // COMスレッド上。UIスレッドへ転送するだけ。
        QMetaObject::invokeMethod(this, [this]{ onDefaultDeviceChangedRaw("notify"); },
                                  Qt::QueuedConnection);
    });
    // ★ 初期デバイスIDは、Start()でCOMを初期化した「後」に取得する
    //   （先に取るとCOM未初期化で空になり、起動直後に誤って切替扱いになる）。
    {
        AudioDeviceInfo info;
        if (AudioDeviceWatcher::QueryDefaultRenderDevice(info))
            m_currentDeviceId = QString::fromStdWString(info.id);
    }
    deviceLog(QStringLiteral("===== Always Player started. watcher=%1 initialDevice=%2")
              .arg(watcherOk ? QStringLiteral("OK") : QStringLiteral("FAILED"))
              .arg(m_currentDeviceId.isEmpty() ? QStringLiteral("(empty)") : m_currentDeviceId));

    // ★ 保険：ドライバやWindowsの状態によっては切替通知が届かないことがある。
    //   1秒ごとに既定デバイスIDを比較し、変わっていれば通知と同じ処理を行う
    //   （IDが同じなら onDefaultDeviceChangedRaw() は即returnするので軽い）。
    m_devicePollTimer = new QTimer(this);
    m_devicePollTimer->setInterval(1000);
    connect(m_devicePollTimer, &QTimer::timeout, this, [this]{ onDefaultDeviceChangedRaw("poll"); });
    m_devicePollTimer->start();

    return true;
}

QString Player::loadLastFolder()
{
    QString ini = QDir::homePath() + "/AlwaysPlayer.ini";
    QFile f(ini);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QTextStream s(&f);
    while (!s.atEnd()) {
        QString line = s.readLine().trimmed();
        if (line.startsWith("last_folder=")) {
            return line.mid(12);
        }
    }
    return {};
}

QStringList Player::collectFiles(const QString &folder, int depth)
{
    if (depth > 2) return {}; // 2層まで
    QStringList files;
    QDir dir(folder);
    QStringList filters;
    for (const auto &ext : SUPPORTED_EXT) {
        filters << QString("*.%1").arg(ext) << QString("*.%1").arg(ext.toUpper());
    }
    dir.setNameFilters(filters);
    dir.setFilter(QDir::Files | QDir::NoDotAndDotDot);
    dir.setSorting(QDir::Unsorted);
    auto fileList = dir.entryInfoList();
    std::sort(fileList.begin(), fileList.end(), [](const QFileInfo &a, const QFileInfo &b){
        return StrCmpLogicalW(a.fileName().toStdWString().c_str(),
                              b.fileName().toStdWString().c_str()) < 0;
    });
    for (const auto &fi : fileList)
        files << fi.absoluteFilePath();

    QDir sub(folder);
    sub.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    sub.setSorting(QDir::Unsorted);
    auto subList = sub.entryInfoList();
    std::sort(subList.begin(), subList.end(), [](const QFileInfo &a, const QFileInfo &b){
        return StrCmpLogicalW(a.fileName().toStdWString().c_str(),
                              b.fileName().toStdWString().c_str()) < 0;
    });
    for (const auto &s : subList)
        files << collectFiles(s.absoluteFilePath(), depth + 1);

    return files;
}

void Player::loadFolder(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    stop();
    m_playlist = collectFiles(path, 0);
    m_currentIndex = 0;
    m_lastFolder = path;

    // 最後のフォルダをiniに保存（アトミックセーブ方式）
    QString ini = QDir::homePath() + "/AlwaysPlayer.ini";
    QString tmp = ini + ".tmp";
    QString bak = ini + ".bk";
    // ① tmpに書く
    {
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream s(&f);
        s << "[settings]\n";
        s << "last_folder=" << path << "\n";
        s.flush(); f.flush();
    }
    // ② 既存iniをバックアップ
    if (QFile::exists(ini)) {
        QFile::remove(bak);
        QFile::rename(ini, bak);
    }
    // ③ tmp → ini
    QFile::rename(tmp, ini);

}

void Player::play(int index)
{
    QMutexLocker lock(&m_mutex);
    // ★ デバイス切替で停止した直後、メッセージが出る前にユーザーが自分で
    //   再生を押した場合は、もうメッセージは不要。
    m_stoppedByDeviceChange = false;
    if (m_playlist.isEmpty()) return;
    if (index >= 0) m_currentIndex = index;
    if (m_currentIndex >= m_playlist.size()) return;

    QString file = m_playlist[m_currentIndex];

    // ── 簡易ギャップレス：曲が変わったので次曲用の事前読み込み状態をリセット
    m_preloadDone = false;
    m_preloadedPath.clear();

    // ★ 前の曲が新エンジン(FLAC/WASAPI排他)経由だった場合、ここで確実に停止する。
    //   （非FLACへ切り替わる場合はtryPlayViaNewEngineが呼ばれないため、
    //    ここで止めておかないと新エンジンが鳴りっぱなしになってしまう）
    stopNewEngine();

    // ★ cdda:// URI は拡張子チェック・TagLib処理をスキップ
    bool isCd = file.startsWith("cdda://");

    QString ext;
    if (!isCd) {
        ext = QFileInfo(file).suffix().toLower();
        if (!NEW_ENGINE_EXT.contains(ext)) {
            emit errorOccurred(QString("非対応フォーマット: %1").arg(ext));
            return;
        }
    }

// ★ 対応フォーマット(FLAC/WAV/AIFF)は、まず自作エンジン(WASAPI排他・
//   ビットパーフェクト)での再生を試みる。排他モード確保失敗など、
//   何らかの理由で開始できなかった場合は、従来通りmpv経路にフォールバックする。
bool playedViaNewEngine = false;
if (isCd) return; // v10: CDはMainWindow側（MCI）で再生するのでここには来ない
{
    // ★ 直前の曲がmpv経路（例：MP3）だった場合、mpvはまだWASAPIデバイスを
    //   共有モードで掴んだままになっている。この状態のままWASAPI排他モードを
    //   要求すると、デバイスが使用中と判定され排他確保に失敗する
    //   （＝毎回mpv側へフォールバックしてしまい、実質FLACが新エンジンで
    //    再生されなくなる)。新エンジンの排他確保を試みる前に、必ずmpvを
    //   止めてデバイスを解放しておく。
    playedViaNewEngine = tryPlayViaNewEngine(file);

    // ★ v10: mpvへのフォールバックは廃止。開けなかった曲は理由を知らせて次の曲へ。
    //   全曲が開けない場合などに無限に送り続けないよう、連続失敗が
    //   プレイリストの曲数に達したら止める。1曲リピート中は送らない。
    if (!playedViaNewEngine) {
        m_useNewEngine = false;
        m_playing = false;
        m_paused  = false;
        const QString name = QFileInfo(file).fileName();
        const QString reason = m_lastEngineError.isEmpty()
            ? QString::fromUtf8("再生できませんでした") : m_lastEngineError;
        QMetaObject::invokeMethod(this, [this, name, reason]{
            emit errorOccurred(QString::fromUtf8("再生できません：%1（%2）").arg(name, reason));
        }, Qt::QueuedConnection);
        ++m_consecutiveEngineFailures;
        if (m_repeatMode != RepeatMode::One && m_consecutiveEngineFailures < m_playlist.size()) {
            QTimer::singleShot(300, this, [this]{ next(); });
        } else {
            m_consecutiveEngineFailures = 0;
            QMetaObject::invokeMethod(this, [this]{ emit playbackStopped(); }, Qt::QueuedConnection);
        }
        return;
    }
    m_consecutiveEngineFailures = 0;
}

if (playedViaNewEngine) {
    m_useNewEngine = true;
    m_playing = true;
    m_paused  = false;
    // ★ v10: mpv経路ではMPV_EVENT_PLAYBACK_RESTARTでplaybackStartedが出るが、
    //   新エンジン経路では誰も出していなかった（VUメーターが動き出さない原因の一つ）。
    //   play()はm_mutexを保持中なので、キュー経由で発行する。
    QMetaObject::invokeMethod(this, [this]{ emit playbackStarted(); }, Qt::QueuedConnection);
    // ★ このplay()で実際に鳴り始める曲を、デコードスレッド基準の「エンジン内
    //   カレントインデックス」としても記録する（m_currentIndexと同じ値で開始し、
    //   以後はcheckGaplessTransition()で内部スワップが起きるたびに即座に
    //   進む。詳細はPlayer.h側のコメント参照）。
    m_gaplessEngineIndex = m_currentIndex;
    qDebug() << "[Gapless] t=" << dbgMs() << "PLAY START: index=" << m_currentIndex
             << "file=" << file << "duration=" << m_newEngineDuration
             << "outputSr=" << m_newEngineOutputSr;

    // ★ ギャップレス：次曲の事前準備は「曲終端まで残り5秒」を待たず、
    //   曲の開始直後に試みる。短いトラック（テスト用ファイルなど）では、
    //   デコードスレッドがリングバッファの余裕分だけ先行して一瞬で
    //   全フレームを読み切ってしまい、5秒前トリガーが一度も発火する前に
    //   「本当のEOF」へ到達してしまうため、ギャップレスが一度も有効化
    //   されないまま常に旧来の再オープン経路にフォールバックしてしまう
    //   （実際に観測された不具合）。PrepareGaplessNext()はファイルを
    //   開くだけの軽い処理なので、早期に呼んでも副作用は無い。
    {
        int nextIdx = peekNextIndex();
        if (nextIdx >= 0 && nextIdx < m_playlist.size()) {
            QString nextPath = m_playlist[nextIdx];
            if (!nextPath.startsWith("cdda://")) {
                tryPrepareGaplessNext(nextPath);
                // ★ checkGaplessPreload()（1000ms周期タイマー）がこの直後に
                //   同じ次曲を重複してアームし直さないよう、ここでも
                //   m_preloadDone を立てておく（従来はcheckGaplessPreload()側
                //   でしか立てておらず、曲頭でのこの早期アームだけでは
                //   フラグが立たないままになっていた）。
                m_preloadDone = true;
            }
        }
    }
}

// タグ読み取りを別スレッドで行いUIスレッドをブロックしない
// ★ シリアル番号で古いスレッドの結果を破棄
int capturedIndex = m_currentIndex;
QString capturedFile = file;
int capturedSerial = ++m_playSerial;
fetchTagsAsync(capturedFile, capturedIndex, capturedSerial);
}

// ★ play()／checkGaplessTransition()（ギャップレス遷移後のUI追いつき）で
//   共用するタグ非同期読み取り処理。指定ファイルのタグ・音声プロパティを
//   別スレッドで読み取り、UIスレッドでキャッシュ更新とtrackChangedを行う。
void Player::fetchTagsAsync(const QString &filePath, int index, int serial)
{
    QThreadPool::globalInstance()->start([this, filePath, index, serial]{
        QString tagTitle, tagArtist;
        int br = 0, sr = 0, bits = 0;
        QString ext2 = QFileInfo(filePath).suffix().toLower();

        if (ext2 == "flac") {
            TagLib::FLAC::File tf(filePath.toStdWString().c_str());
            if (tf.isValid()) {
                if (tf.tag()) {
                    tagTitle  = QString::fromUtf8(tf.tag()->title().toCString(true));
                    tagArtist = QString::fromUtf8(tf.tag()->artist().toCString(true));
                }
                if (tf.audioProperties()) {
                    br   = tf.audioProperties()->bitrate();
                    sr   = tf.audioProperties()->sampleRate();
                    bits = tf.audioProperties()->bitsPerSample();
                }
            }
        } else if (ext2 == "mp3") {
            TagLib::MPEG::File tf(filePath.toStdWString().c_str());
            if (tf.isValid()) {
                if (tf.tag()) {
                    tagTitle  = QString::fromUtf8(tf.tag()->title().toCString(true));
                    tagArtist = QString::fromUtf8(tf.tag()->artist().toCString(true));
                }
                if (tf.audioProperties()) {
                    br = tf.audioProperties()->bitrate();
                    sr = tf.audioProperties()->sampleRate();
                }
            }
        } else if (ext2 == "m4a" || ext2 == "mp4" || ext2 == "aac") {
            TagLib::MP4::File tf(filePath.toStdWString().c_str());
            if (tf.isValid()) {
                if (tf.tag()) {
                    tagTitle  = QString::fromUtf8(tf.tag()->title().toCString(true));
                    tagArtist = QString::fromUtf8(tf.tag()->artist().toCString(true));
                }
                if (tf.audioProperties()) {
                    br   = tf.audioProperties()->bitrate();
                    sr   = tf.audioProperties()->sampleRate();
                    bits = tf.audioProperties()->bitsPerSample();
                }
            }
        } else if (ext2 == "wav" || ext2 == "aiff" || ext2 == "wv") {
            TagLib::FileRef tf(filePath.toStdWString().c_str());
            if (!tf.isNull()) {
                if (tf.tag()) {
                    tagTitle  = QString::fromUtf8(tf.tag()->title().toCString(true));
                    tagArtist = QString::fromUtf8(tf.tag()->artist().toCString(true));
                }
                if (tf.audioProperties()) {
                    br   = tf.audioProperties()->bitrate();
                    sr   = tf.audioProperties()->sampleRate();
                }
            }
        }
        if (tagTitle.isEmpty())
            tagTitle = QFileInfo(filePath).completeBaseName();

        // キャッシュ更新とシグナル発行はUIスレッドで
        // ★ シリアル番号が一致する場合のみ発行（古いスレッド結果を捨てる）
        QMetaObject::invokeMethod(this, [this, index, filePath,
                                          tagTitle, tagArtist, br, sr, bits, serial]{
            if (serial != m_playSerial) {
                qDebug() << "[Player] trackChanged discarded (stale):" << serial;
                return;
            }
            m_cachedBr   = br;
            emit trackChanged(index, QFileInfo(filePath).fileName(),
                              tagTitle, tagArtist);
        }, Qt::QueuedConnection);
    });
}

// ── 新エンジン(PcmDualEngine)統合 ──────────────────────────────
bool Player::tryPlayViaNewEngine(const QString &filePath)
{
    stopNewEngine();

    const std::string extLower = QFileInfo(filePath).suffix().toLower().toStdString();
    if (!m_pcmEngine.Open(filePath.toStdWString(), extLower)) {
        m_lastEngineError = QString::fromUtf8("この形式・内容のファイルには対応していません");
        return false;
    }
    // v10: 手動ビットパーフェクトのビット数を優先（なければ自動）
    m_newEngineOutput.SetPreferredBits(m_manualRateOverride ? m_pinBits : 0);

    // ★ アップサンプリング(dsd8/hires4)：ネイティブ（原音）レートを基に
    //   目標レートを決定し、PcmDualEngine側のSincResamplerへ反映する。
    //   m_dspOffがtrueの間は常にネイティブレート（リサンプルなし）になる
    //   （computeNewEngineTargetRate()内で判定）。
    const uint32_t nativeRate = m_pcmEngine.GetNativeSampleRate();
    // ★ v10: 共有モードで鳴らしている機器は、Windowsの「既定の形式」が途中で
    //   変更されていても追従できるよう、開くたびにレートを問い合わせ直す。
    if (m_deviceSharedMode) {
        const uint32_t mr = m_newEngineOutput.QueryMixRate();
        if (mr > 0) m_deviceFixedRate = mr;
    }
    const uint32_t targetRate = computeNewEngineTargetRate(nativeRate);

    // ★ v10: 出力デバイスが目標レート（例：疑似DSD×8の352.8kHz）に排他モードで
    //   対応していない場合、以前はそのままmpvへ逃げていた。今は半分ずつ
    //   (352.8→176.4→88.2kHz…)下げて、デバイスが受け付ける一番高いレートを
    //   自動で選ぶ。最後はネイティブ（原音）レート。
    // ★ v10: 前回までに「この機器は特定のレート／共有モードでしか鳴らない」と
    //   わかっていれば、最初からその形で開く（ギャップレス判定とも一致させる）。
    m_newEngineOutput.SetSharedMode(m_deviceSharedMode);

    QList<uint32_t> candidates;
    candidates << targetRate;
    if (m_deviceFixedRate == 0) {
        for (uint32_t r = targetRate / 2; r > nativeRate && r >= 44100; r /= 2)
            candidates << r;
        if (!candidates.contains(nativeRate)) candidates << nativeRate;
    }

    AudioFormat fmt;
    AudioBackendResult initResult = AudioBackendResult::UnknownError;
    for (uint32_t candidate : candidates) {
        if (tryInitOutput(candidate, initResult)) break;
        if (initResult == AudioBackendResult::FormatNotSupported) {
            m_unsupportedOutRates.insert(candidate);
            continue;
        }
        break; // 形式以外の理由（デバイス使用中など）はレートを下げても無駄
    }

    // ★ v10: どのレートでも開けなかった場合（Bluetoothなど）。
    //   ① 機器本来の形式（Windowsの「既定の形式」）のレートで、排他モードを試す。
    //   ② それも断られたら、共有モードで鳴らす（レートは同じく既定の形式に合わせ、
    //      周波数変換はAlways Engine自身のリサンプラーで行う）。
    if (initResult != AudioBackendResult::Ok) {
        // 他のアプリがDACを使っていて排他を取れなかっただけなら、共有モードで
        // 鳴らすのはこの曲だけにする（次の曲では改めて排他モードを試す）。
        const bool deviceBusy = (m_newEngineOutput.GetLastHr() == AUDCLNT_E_DEVICE_IN_USE);
        const uint32_t mixRate = m_newEngineOutput.QueryMixRate();
        if (mixRate > 0) {
            bool ok = false;
            if (!candidates.contains(mixRate) || m_deviceSharedMode) {
                m_newEngineOutput.SetSharedMode(false);
                ok = tryInitOutput(mixRate, initResult);
                if (ok) { m_deviceFixedRate = mixRate; m_deviceSharedMode = false; }
            }
            if (!ok) {
                m_newEngineOutput.SetSharedMode(true);
                ok = tryInitOutput(mixRate, initResult);
                if (ok && !deviceBusy) { m_deviceFixedRate = mixRate; m_deviceSharedMode = true; }
                if (!ok) m_newEngineOutput.SetSharedMode(false);
            }
        }
    }
    fmt = m_newEngineOutput.GetActualFormat();
    fmt.sampleRate = m_pcmEngine.GetSampleRate();
    if (initResult != AudioBackendResult::Ok) {
        m_lastEngineError = (initResult == AudioBackendResult::FormatNotSupported)
            ? QString::fromUtf8("出力デバイスがこの形式に対応していません")
            : QString::fromUtf8("出力デバイスを開けませんでした（他のアプリが使用中の可能性があります）");
        // ★ 排他モード確保失敗などの場合、呼び出し側でmpv経路にフォールバックする。
        //   Initialize()が途中まで成功していた場合（IAudioClient::Initializeで
        //   排他ロックを確保した後、SetEventHandle等で失敗した場合など）に
        //   デバイスのロックが残ってしまわないよう、必ずShutdown()で完全に解放する。
        m_newEngineOutput.Shutdown();
        m_pcmEngine.Close();
        return false;
    }

    m_pcmEngine.AttachOutputBackend(&m_newEngineOutput);

    // ★ DSPチェーン。現在の中密度チェーン/HP補正/音場/ラウドネス正規化設定を
    //   反映してからコールバックとして接続する。m_dspOffがtrueの間は
    //   SetBitPerfect(true)で完全バイパス（ビットパーフェクト優先）。
    //   （アップサンプリング自体はDspChainではなくPcmDualEngine側で
    //    デコード直後に行う。DspChainはリサンプル後のレートで動作する）
    m_newEngineDsp.Prepare(fmt.sampleRate);
    m_newEngineDsp.SetChainOn(m_chainOn);
    m_newEngineDsp.SetHp(m_hp1, m_hp2);
    m_newEngineDsp.SetSoundField(soundFieldModeFromString(m_soundField));
    m_newEngineDsp.SetLoudnessOn(m_mode == "loudness");

    m_newEngineProcessThread = std::make_unique<AudioProcessThread>(
        m_pcmEngine.GetRingBuffer(), &m_newEngineOutput);
    m_newEngineProcessThread->SetDspCallback([this](float *buf, size_t n){
        m_newEngineDsp.Process(buf, n);
    });
    m_newEngineProcessThread->SetBitPerfect(m_dspOff);
    m_newEngineProcessThread->SetGain(volumeToGain(m_volume)); // v10: 音量
    // ★ 真の終端（ギャップレスで次曲へ継続しない、本当のストリーム終端）でのみ、
    //   リングバッファに残った端数フレームをゼロ埋めして出力するための問い合わせ。
    //   ギャップレス遷移中はIsEndOfStream()がfalseのままなので、この端数フラッシュは
    //   発火せず、正しく次曲のデータを待ち続ける（AudioProcessThread.h参照）。
    m_newEngineProcessThread->SetEofQuery([this]{ return m_pcmEngine.IsEndOfStream(); });

    m_pcmEngine.StartDecoding();
    m_newEngineOutput.Start();
    m_newEngineProcessThread->Start();

    // ★ 長さはネイティブ（原音）のフレーム数／ネイティブレートで計算する
    //   （fmt.sampleRateはリサンプル後の目標レートのため、これで割ると
    //    アップサンプリング時に長さがズレてしまう）。
    uint64_t totalFrames = m_pcmEngine.GetTotalFrames();
    m_newEngineDuration = (nativeRate > 0)
        ? static_cast<double>(totalFrames) / static_cast<double>(nativeRate) : 0.0;
    m_newEnginePositionBase = 0.0;
    m_newEngineElapsedTimer.restart();
    m_newEngineEofFired = false;

    // infoラベル用キャッシュも実際の再生値で更新
    // ★ m_cachedSrはgetInfo()のdispMode判定が前提とする「原音」レートを
    //   維持する（nativeRateを使う。fmt.sampleRateはリサンプル後の目標
    //   レートなので、ここで使うとdsd8/hires4選択時にhiRes判定が循環して
    //   おかしくなる）。実際の出力レートはm_newEngineOutputSrに別途保持する。
    m_cachedSr          = static_cast<int>(nativeRate);
    m_cachedBits        = static_cast<int>(m_pcmEngine.GetBitsPerSample());
    m_newEngineOutputSr = static_cast<int>(fmt.sampleRate);

    // ★ ギャップレス表示チェーンを「今開始したこの1曲だけ」でリセットする。
    //   これ以降、内部スワップが検知されるたびにcheckGaplessTransition()が
    //   ここへ1件ずつ追記していく（詳細はPlayer.h側のコメント参照）。
    m_gaplessChain.clear();
    m_gaplessChain.append({ m_currentIndex, m_newEngineDuration,
                             static_cast<uint32_t>(nativeRate),
                             static_cast<uint32_t>(m_cachedBits) });

    return true;
}

// ★ v10: 指定レートで出力デバイスを開く（排他／共有はSetSharedMode()の指定どおり）。
//   デバイス解放の遅れで「使用中」になることがあるので、短い待機を挟んで数回試す。
//   形式非対応は待っても変わらないので、すぐに諦める。
bool Player::tryInitOutput(uint32_t rate, AudioBackendResult &result)
{
    m_pcmEngine.SetTargetSampleRate(rate);
    AudioFormat fmt;
    fmt.sampleRate   = m_pcmEngine.GetSampleRate(); // リサンプル後（目標）レート
    fmt.channels     = static_cast<uint16_t>(m_pcmEngine.GetTotalChannels());
    fmt.sampleFormat = AudioSampleFormat::Int32;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0) {
            m_newEngineOutput.Shutdown();
            QThread::msleep(50);
        }
        result = m_newEngineOutput.Initialize(fmt);
        if (result == AudioBackendResult::Ok) return true;
        if (result == AudioBackendResult::FormatNotSupported) break;
    }
    m_newEngineOutput.Shutdown();
    return false;
}

// ★ 新エンジン用のアップサンプリング目標レートを決定する。
//   mpv経路のapplyAudioChain()と同じルールを踏襲：
//   ・m_dspOff中は常にネイティブレート（リサンプルなし、ビットパーフェクト優先）
//   ・dsd8：常に352800Hzへ（ネイティブが既にそれ以上でも一律アップ/ダウン変換）
//   ・hires4：ハイレゾ(48kHz超)ならネイティブのまま、それ以外は176400Hzへ
//   ・pure/loudness：ネイティブのまま（loudnessは同レート処理のみ対応の
//     既存方針を維持。upsampling併用は将来検討）
uint32_t Player::computeNewEngineTargetRate(uint32_t nativeRate) const
{
    if (nativeRate == 0) return nativeRate;
    // ★ v10: 機器が特定のレートしか受け付けない（Bluetooth等）とわかっていれば、
    //   常にそのレートへ変換して出す。
    if (m_deviceFixedRate > 0) return m_deviceFixedRate;
    // ★ v10: 手動ビットパーフェクトで出力レートが指定されていれば、それが最優先
    //   （デバイスが受け付けなければ半分ずつ下げる）。
    if (m_manualRateOverride && m_pinRate > 0) {
        uint32_t target = static_cast<uint32_t>(m_pinRate);
        while (target > 44100 && m_unsupportedOutRates.contains(target)) target /= 2;
        return target;
    }
    if (m_dspOff) return nativeRate;
    const bool isHiRes = (nativeRate > 48000);
    uint32_t target = nativeRate; // pure / loudness
    if (m_mode == "dsd8") {
        target = 352800;
    } else if (m_mode == "hires4") {
        target = isHiRes ? nativeRate : 176400;
    }
    // ★ v10: 今の出力デバイスが受け付けなかったレートは避け、半分ずつ下げる
    //   （ギャップレス判定で次曲の目標レートと一致させるためにも必要）。
    while (target > nativeRate && m_unsupportedOutRates.contains(target))
        target /= 2;
    if (target < nativeRate) target = nativeRate;
    return target;
}

// ★ dsd8/hires4によるレート変更やdspOff切り替えは、WASAPI排他ストリームを
//   動作中に変更できないため、mpv経路のapplyAudioChainAndReload()と同様、
//   位置保存→再オープン(tryPlayViaNewEngine)→位置復元で反映する。
//   切り替え時に短い途切れが生じるのは許容する（正常動作）。
void Player::reloadNewEngineForRateChange()
{
    if (!m_useNewEngine) return;
    if (m_currentIndex < 0 || m_currentIndex >= m_playlist.size()) return;
    if (!m_playing && !m_paused) return; // 停止中は次回再生時に反映されるので何もしない

    const double pos = getPosition();
    const bool wasPaused = m_paused;
    const QString path = m_playlist[m_currentIndex];

    if (!tryPlayViaNewEngine(path)) {
        // 再オープン失敗：直前まで再生できていたファイルの再オープンのため
        // 通常は起きない想定。停止扱いにする。
        stopNewEngine();
        m_useNewEngine = false;
        m_playing = false;
        m_paused  = false;
        emit playbackStopped();
        return;
    }

    m_useNewEngine = true;
    m_playing = true;
    m_paused  = false;
    // ★ play()を経由しないtryPlayViaNewEngine()の直接呼び出しなので、
    //   ここでもエンジン内カレントインデックスを再同期しておく。
    m_gaplessEngineIndex = m_currentIndex;

    // ★ 出力レートが変わりうる再オープンのため、以前のレート向けに
    //   確定していたギャップレス次曲プリロード状態は無効。再アーム
    //   できるようリセットする（stopNewEngine()→Close()側で実際の
    //   PcmDualEngine::CancelGapless()は既に呼ばれている）。
    m_preloadDone = false;
    m_preloadedPath.clear();

    if (pos > 0.1) seekTo(pos);
    if (wasPaused) pause();
}

void Player::stopNewEngine()
{
    if (m_newEngineProcessThread) {
        m_newEngineProcessThread->Stop();
        m_newEngineProcessThread.reset();
    }
    m_newEngineOutput.Stop();
    m_newEngineOutput.Shutdown();
    m_pcmEngine.StopDecoding();
    m_pcmEngine.Close();
}

void Player::seekTo(double seconds)
{
    if (m_useNewEngine) {
        if (seconds < 0.0) seconds = 0.0;
        if (seconds > m_newEngineDuration) seconds = m_newEngineDuration;
        // ★ v10修正：Seek()はリングバッファをClear()する。処理スレッドが読んでいる
        //   最中にClear()すると読み出し位置が狂い、ゴミデータ（大音量ノイズ）が
        //   出る恐れがあるため、シークの間だけ処理スレッドを止める。
        const bool procWasRunning = m_newEngineProcessThread && m_newEngineProcessThread->IsRunning();
        if (procWasRunning) m_newEngineProcessThread->Stop();
        m_pcmEngine.Seek(seconds);
        if (procWasRunning) m_newEngineProcessThread->Start();
        m_newEnginePositionBase = seconds;
        m_newEngineElapsedTimer.restart();
        // ★ シークは「今表示している曲の中の位置」を直接指定する操作なので、
        //   それより後ろに積まれていたギャップレスチェーンの先読み分は
        //   もう意味を持たない。現在表示中の曲1件だけの新しいチェーンに
        //   作り直す（以後はcheckGaplessTransition()が内部スワップの
        //   たびに追記していく）。
        m_gaplessChain.clear();
        if (m_currentIndex >= 0 && m_currentIndex < m_playlist.size()) {
            m_gaplessChain.append({ m_currentIndex, m_newEngineDuration,
                                     static_cast<uint32_t>(m_cachedSr),
                                     static_cast<uint32_t>(m_cachedBits) });
        }
    }
}
// ──────────────────────────────────────────────────────────────

void Player::pause()
{
    if (m_useNewEngine) {
        if (!m_playing) return;
        m_newEnginePositionBase += m_newEngineElapsedTimer.elapsed() / 1000.0;
        // ★ v10修正（フリーズ対策）：以前は処理スレッドを動かしたまま出力だけ止めて
        //   いたため、処理スレッドがWriteFrames()の中で出力リングの空きを永久に待ち、
        //   一時停止後に次曲／停止／終了／出力切替をするとjoin()でGUIごと固まっていた。
        //   出力がまだ動いている（リングが捌ける）うちに、処理スレッドを先に止める。
        if (m_newEngineProcessThread) m_newEngineProcessThread->Stop();
        m_pcmEngine.StopDecoding();
        m_newEngineOutput.Stop();
        m_playing = false;
        m_paused  = true;
        emit playbackPaused();
        return;
    }
}

void Player::resume()
{
    if (m_useNewEngine) {
        if (m_paused) {
            m_newEngineOutput.Start();
            m_pcmEngine.StartDecoding();
            // ★ v10修正：pause()で止めた処理スレッドを再開する
            if (m_newEngineProcessThread) m_newEngineProcessThread->Start();
            m_newEngineElapsedTimer.restart();
            m_playing = true;
            m_paused  = false;
            emit playbackStarted();
        } else {
            play();
        }
        return;
    }
    play(); // 停止中（新エンジン未使用）なら通常の再生開始
}

void Player::stop()
{
    // ★ 新エンジン再生中/待機中の後始末（新エンジン非使用時は内部で早期returnする無害な呼び出し）
    stopNewEngine();
    m_useNewEngine = false;

    m_playing    = false;
    m_paused     = false;
    emit playbackStopped();
}

// ── 出力デバイス切替時の爆音防止 ─────────────────────────────────
// 方針：出力先が切り替わったら必ず停止し、「停止させました。再生をしてください」
// と知らせるだけにする。自動再開はしない（再生はユーザーが音量を確認してから
// 自分で押す）。内蔵スピーカーで勝手に鳴る事故も、排他モードの爆音も防げる。
//
// ① 既定デバイスの切替検知（COM通知のUIスレッド転送、または1秒ごとの監視）。
//   ここで即座に再生を止めてデバイスを解放する。mpv経路（共有モード）は
//   既定デバイスに自動追従して新デバイスで鳴り出すため、止めるのが遅れると
//   その間に音が出てしまう。
// ★ 診断ログ：VSが無くても原因を追えるよう、exeと同じフォルダの
//   device_debug.log に追記する（切替検知まわりの調査用）。
void Player::deviceLog(const QString &line)
{
    qDebug().noquote() << "[Device]" << line;
    // ★ リリース版ではファイルに書かない（Program Files配下は書き込み不可で、
    //   毎回失敗するだけのため）。調査時は ALWAYS_DEVICE_LOG を定義してビルドする。
#ifndef ALWAYS_DEVICE_LOG
    return;
#endif
    QFile f(QCoreApplication::applicationDirPath() + "/device_debug.log");
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        ts << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << "  " << line << "\n";
    }
}

void Player::onDefaultDeviceChangedRaw(const char *source)
{
    // 通知経由の呼び出しは、変化の有無に関わらず記録する（届いているかの確認用）
    if (qstrcmp(source, "notify") == 0)
        deviceLog(QStringLiteral("notify received from Windows"));

    // メッセージ表示中は何もしない（閉じた後に改めて確認する）。
    if (m_deviceDialogOpen) return;

    AudioDeviceInfo info;
    long hr = 0;
    // 取得失敗（一時的なエラー、デバイスが1台も無い等）は「切替」とみなさない。
    if (!AudioDeviceWatcher::QueryDefaultRenderDevice(info, &hr) || info.id.empty()) {
        if (m_deviceQueryFailLogged < 20) {
            ++m_deviceQueryFailLogged;
            deviceLog(QStringLiteral("[%1] query default device FAILED hr=0x%2")
                      .arg(QLatin1String(source)).arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')));
        }
        return;
    }
    const QString newId = QString::fromStdWString(info.id);

    // 1秒監視が動いているかの確認用：起動直後の3回だけ、見えているデバイスを記録
    static int s_pollSeen = 0;
    if (qstrcmp(source, "poll") == 0 && s_pollSeen < 3) {
        ++s_pollSeen;
        deviceLog(QStringLiteral("[poll #%1] default=\"%2\" kind=%3 formFactor=%4 same=%5")
                  .arg(s_pollSeen)
                  .arg(QString::fromStdWString(info.friendlyName))
                  .arg(static_cast<int>(info.kind))
                  .arg(info.formFactor)
                  .arg(int(newId == m_currentDeviceId)));
    }

    if (newId == m_currentDeviceId) return;   // 変化なし
    m_currentDeviceId = newId;
    // ★ v10: 出力先が変わったので、対応レート・共有モードの判定を調べ直す
    //   （以前は再生中に切り替えたときだけリセットしていた）。
    m_unsupportedOutRates.clear();
    m_deviceFixedRate  = 0;
    m_deviceSharedMode = false;

    deviceLog(QStringLiteral("[%1] CHANGED -> \"%2\" enumerator=%3 formFactor=%4 kind=%5 | playing=%6 paused=%7 newEngine=%8")
              .arg(QLatin1String(source))
              .arg(QString::fromStdWString(info.friendlyName))
              .arg(QString::fromStdWString(info.enumeratorName))
              .arg(info.formFactor)
              .arg(static_cast<int>(info.kind))
              .arg(int(m_playing)).arg(int(m_paused)).arg(int(m_useNewEngine)));

    if (m_playing || m_paused) {
        stopNewEngine();          // WASAPI排他デバイスを解放
        m_useNewEngine = false;
        m_playing = false;
        m_paused  = false;
        emit playbackStopped();
        m_stoppedByDeviceChange = true;
        deviceLog(QStringLiteral("  -> playback stopped"));
    }
    // 切替通知は短時間に連発するので、落ち着いてからメッセージを1回だけ出す
    if (m_stoppedByDeviceChange) m_deviceSettleTimer->start();
}

// ② 通知が落ち着いたら、止めたことをメッセージで知らせる（OKのみ）。
void Player::onDefaultDeviceSettled()
{
    if (m_deviceDialogOpen) return;
    if (!m_stoppedByDeviceChange) return;   // 途中でユーザーが再生を押した等
    m_stoppedByDeviceChange = false;

    QMessageBox box(QApplication::activeWindow());
    box.setIcon(QMessageBox::Warning);   // ⚠ 黄色アイコンで音量注意を目立たせる
    box.setWindowTitle(QStringLiteral("Always Player"));
    box.setText(QStringLiteral("出力先が切り替わったため、Always Playerを停止しました。"));
    box.setInformativeText(QStringLiteral("出力先によって音量が大きく変わることがあります。\n"
                                          "再生する前に、DACやアンプの音量を下げてから再生してください。"));
    box.setStandardButtons(QMessageBox::Ok);
    box.setWindowFlag(Qt::WindowStaysOnTopHint, true);

    deviceLog(QStringLiteral("  -> showing stop message"));
    m_deviceDialogOpen = true;
    box.exec();
    m_deviceDialogOpen = false;
}

void Player::setDspOff(bool off)
{
    m_dspOff = off;
    if (m_useNewEngine) {
        // ★ dspOffはアップサンプリング有無も左右するため（true中は常にネイティブ
        //   レート＝ビットパーフェクト優先）、WASAPI排他ストリームのレートを
        //   再計算するために再オープンが必要。mpv経路と同様、再生中のみ反映する。
        if (m_playing || m_paused) {
            reloadNewEngineForRateChange();
        }
    }
}

void Player::setChainOn(bool on)
{
    m_chainOn = on;
    m_newEngineDsp.SetChainOn(on);
}

void Player::setShuffle(ShuffleMode mode)
{
    m_shuffleMode = mode;
    m_shuffleList.clear();
    m_shufflePos = 0;
    if (mode == ShuffleMode::Folder) {
        // フォルダ内（下層含む）シャッフルリスト生成
        for (int i = 0; i < m_playlist.size(); i++) m_shuffleList << i;
        std::shuffle(m_shuffleList.begin(), m_shuffleList.end(),
                     std::default_random_engine{std::random_device{}()});
    } else if (mode == ShuffleMode::Favorites) {
        // お気に入りフォルダからファイルを収集してシャッフル
        QStringList files;
        for (const QString &favPath : m_favPaths) {
            QDir dir(favPath);
            if (!dir.exists()) continue;
            QStringList exts;
            for (const QString &e : SUPPORTED_EXT) exts << ("*" + e);
            QFileInfoList fi = dir.entryInfoList(exts,
                QDir::Files | QDir::NoDotAndDotDot);
            for (const auto &f : fi) files << f.absoluteFilePath();
            // 下層も含める（2階層）
            QStringList subdirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &sub : subdirs) {
                QDir subDir(favPath + "/" + sub);
                QFileInfoList sfi = subDir.entryInfoList(exts,
                    QDir::Files | QDir::NoDotAndDotDot);
                for (const auto &f : sfi) files << f.absoluteFilePath();
            }
        }
        std::shuffle(files.begin(), files.end(),
                     std::default_random_engine{std::random_device{}()});
        m_playlist = files;
        m_currentIndex = 0;
        for (int i = 0; i < m_playlist.size(); i++) m_shuffleList << i;
    }
}

void Player::next()
{
    QMutexLocker lock(&m_mutex);

    // リピート1曲
    if (m_repeatMode == RepeatMode::One) {
        lock.unlock();
        play();
        return;
    }

    // シャッフルモード
    if (m_shuffleMode != ShuffleMode::None && !m_shuffleList.isEmpty()) {
        m_shufflePos++;
        if (m_shufflePos >= m_shuffleList.size()) {
            if (m_repeatMode == RepeatMode::All) m_shufflePos = 0;
            else return;
        }
        m_currentIndex = m_shuffleList[m_shufflePos];
        lock.unlock();
        play();
        return;
    }

    // 通常次曲
    if (m_currentIndex + 1 < m_playlist.size()) {
        m_currentIndex++;
        lock.unlock();
        play();
    } else if (m_repeatMode == RepeatMode::All) {
        m_currentIndex = 0;
        lock.unlock();
        play();
    }
}

void Player::prev()
{
    QMutexLocker lock(&m_mutex);

    // シャッフルモード
    if (m_shuffleMode != ShuffleMode::None && !m_shuffleList.isEmpty()) {
        if (m_shufflePos > 0) {
            m_shufflePos--;
            m_currentIndex = m_shuffleList[m_shufflePos];
            lock.unlock();
            play();
        }
        return;
    }

    if (m_currentIndex > 0) {
        m_currentIndex--;
        lock.unlock();
        play();
    }
}

// ─────────────────────────────────────────────
// 簡易ギャップレス（v8簡易版）
//
// 本格的な継ぎ目なし再生（サンプル単位での連結）はv9の自作エンジンで
// 対応する。ここではWASAPI排他モードの再初期化にかかる遅延を、次曲の
// OSファイルキャッシュを事前に温めておくことで短縮する延命措置に留める。
// カタログスペックには記載しない内部最適化。
// ─────────────────────────────────────────────

// next()と同じ選曲ロジックを非破壊（状態を書き換えない）で辿り、
// 「次に鳴るはずの曲」のインデックスだけを返す。該当なしは-1。
int Player::peekNextIndex() const
{
    return peekNextIndexFrom(m_currentIndex);
}

// ★ baseIndexを基準に「次に来る曲」のインデックスを返す。シャッフル時は
//   m_shufflePos（シャッフル順での現在位置）を基準にするため、baseIndex自体は
//   シャッフル時には使われない（m_shufflePosが常に正しく保守されている前提）。
int Player::peekNextIndexFrom(int baseIndex) const
{
    if (m_playlist.isEmpty()) return -1;

    // リピート1曲：次も同じ曲
    if (m_repeatMode == RepeatMode::One) {
        return baseIndex;
    }

    // シャッフルモード
    if (m_shuffleMode != ShuffleMode::None && !m_shuffleList.isEmpty()) {
        int nextPos = m_shufflePos + 1;
        if (nextPos >= m_shuffleList.size()) {
            if (m_repeatMode == RepeatMode::All) nextPos = 0;
            else return -1;
        }
        return m_shuffleList[nextPos];
    }

    // 通常次曲
    if (baseIndex + 1 < m_playlist.size()) {
        return baseIndex + 1;
    } else if (m_repeatMode == RepeatMode::All) {
        return 0;
    }
    return -1;
}

// 再生位置ポーリング：曲終端が近づいたら一度だけ次曲の事前読み込みを起動
// ★ 新エンジン(PcmDualEngine)再生中のEOF検知：
//   ①デコードスレッドがファイル終端に達し(IsEndOfStream)、
//   ②デコード済みPCMがリングバッファに一切残っていない(AvailableToRead==0)
//   の両方が揃った時点で「鳴らし切った」とみなし、mpv経路のEOFと同様に
//   next()を呼ぶ。②を見ずに①だけで判定すると、まだ再生されていない
//   末尾数秒分を切り捨てて次曲へ進んでしまうため、必ず両方確認する。
void Player::checkNewEngineEof()
{
    if (!m_useNewEngine || !m_playing) return;
    if (m_newEngineEofFired) return;

    if (!m_pcmEngine.IsEndOfStream()) return;
    AudioRingBuffer *ring = m_pcmEngine.GetRingBuffer();
    if (ring && ring->AvailableToRead() > 0) return;
    // ★ v10: エンジン側リングが空でも、WASAPI出力側の内部リングにはまだ
    //   数十ms分の音が残っている。これが鳴り終わるまで待たないと、曲尾が
    //   切れ、シークバーも最後まで伸びきらないまま次曲へ進んでしまう。
    if (m_newEngineOutput.GetQueuedFrames() > 0) return;

    m_newEngineEofFired = true;

    // ★ シークバー表示を曲の長さちょうどで止める（経過時間の基準を十分先へ
    //   進めておけば、getPosition()は曲の長さでクリップされる）。
    m_newEnginePositionBase += 1.0e6;

    // ★ デバイスバッファに残った最後の1回分が鳴り終わるまでの時間＋
    //   UIのシークバー更新周期(100ms)分だけ待ってから次曲へ進む。
    //   （ギャップレスが効かなかった場合のみ通る経路なので、元々ここには
    //    再オープンによる短い途切れがあり、この待ちは聴感上問題にならない）
    int delayMs = 120;
    const uint32_t outSr = static_cast<uint32_t>(m_newEngineOutputSr);
    if (outSr > 0)
        delayMs += static_cast<int>(1000ULL * m_newEngineOutput.GetBufferSize() / outSr);

    qDebug() << "[Gapless] FALLBACK REOPEN PATH: checkNewEngineEof fired next() "
                "(gapless was NOT consumed for this boundary) delayMs=" << delayMs;
    QTimer::singleShot(delayMs, this, [this]{
        qDebug() << "[Player] 新エンジンEOF → next()";
        next();
    });
}

void Player::checkGaplessPreload()
{
    if (!m_playing || m_paused) return;
    if (m_preloadDone) return;

    double dur = getDuration();
    double pos = getPosition();
    if (dur <= 0.0) return;

    // 曲終端まで残り5秒を切ったら発火（CD再生・不明長は対象外）
    constexpr double kPreloadLeadSec = 5.0;
    if (dur - pos > kPreloadLeadSec) return;

    QMutexLocker lock(&m_mutex);
    // ★ 必ず「デコードスレッドが実際に今再生している曲」
    //   (m_gaplessEngineIndex) を基準に次曲を計算する。UI表示用の
    //   m_currentIndexを基準にすると、ギャップレス内部スワップ直後～UI追いつき
    //   更新が完了するまでの間（短い曲では曲の長さの大半に及ぶこともある）、
    //   既にデコード中の曲を「次の曲」として再計算し続けてしまい、本当の
    //   次曲が一度も準備されないまま本当のEOFに達してギャップレスが不成立に
    //   なってしまう（実機デバッグログで実際に観測・特定した不具合）。
    const int baseIndex = (m_gaplessEngineIndex >= 0) ? m_gaplessEngineIndex : m_currentIndex;
    int nextIdx = peekNextIndexFrom(baseIndex);
    if (nextIdx < 0 || nextIdx >= m_playlist.size()) return;
    QString nextPath = m_playlist[nextIdx];
    lock.unlock();

    // CD直再生(cdda://)はファイルではないため対象外
    if (nextPath.startsWith("cdda://")) return;

    m_preloadDone = true; // 二重発火防止（完了を待たずに立てる）

    // ★ 新エンジン再生中：出力フォーマットが一致すればサンプル単位で
    //   継ぎ目なし接続する（tryPrepareGaplessNext内で判定。不一致なら
    //   何もしない＝従来通りEOF時の再オープンで途切れる）。
    if (m_useNewEngine) {
        tryPrepareGaplessNext(nextPath);
    }

    preloadNextFile(nextPath);
}

// ★ ギャップレス再生：次曲の出力フォーマット（アップサンプリング後のレート）が
//   現在再生中の曲と完全一致する場合のみ、PcmDualEngine側に次曲デコーダーを
//   事前オープンして渡す。一致しなければCancelGapless()で破棄し、従来通り
//   EOF時の（途切れを伴う）再オープンにフォールバックさせる。
//   （WASAPI排他ストリームの実フォーマットはInt32/2ch固定でビット深度に
//    依存しないため、判定は出力サンプルレートの一致だけで十分）
void Player::tryPrepareGaplessNext(const QString &nextPath)
{
    QString ext = QFileInfo(nextPath).suffix().toLower();
    if (!NEW_ENGINE_EXT.contains(ext)) return; // mpv経路の曲へは継ぎ目なし非対応

    const std::string extLower = ext.toStdString();
    PcmDualEngine::GaplessInfo info = m_pcmEngine.PrepareGaplessNext(nextPath.toStdWString(), extLower);
    if (!info.ok) return;

    const uint32_t nextTargetRate = computeNewEngineTargetRate(info.nativeSampleRate);
    if (nextTargetRate != static_cast<uint32_t>(m_newEngineOutputSr)) {
        // 出力レートが変わる＝WASAPI再オープンが必要なので継ぎ目なし不可
        qDebug() << "[Gapless] t=" << dbgMs() << "CANCEL: nextTargetRate=" << nextTargetRate
                 << "currentOutputSr=" << m_newEngineOutputSr
                 << "nextNativeRate=" << info.nativeSampleRate
                 << "path=" << nextPath;
        m_pcmEngine.CancelGapless();
        return;
    }

    qDebug() << "[Gapless] t=" << dbgMs() << "ARMED: nextTargetRate=" << nextTargetRate
             << "nextNativeRate=" << info.nativeSampleRate
             << "totalFrames=" << info.totalFrames
             << "path=" << nextPath;
    m_pcmEngine.ConfirmGaplessTarget(nextTargetRate);
}

// ★ ギャップレス再生：デコードスレッドが内部でシームレスに次曲へ切り替わった
//   こと（PcmDualEngine::GetGaplessTransitionCount()の増分）をポーリングで
//   検知し、Player側のインデックス・長さ・infoキャッシュ・タグ表示を後追いで
//   更新する。切り替え自体はWASAPIストリームを止めずにデコードスレッド内で
//   完結しているため、ここではUI状態を追いつかせるだけでよい。
void Player::checkGaplessTransition()
{
    if (!m_useNewEngine || !m_playing) return;

    // ★ 調査用：シークバーに実際に反映される値（getPosition/getDuration）を
    //   約500ms間隔（このタイマーは100ms周期なので5回に1回）でログし、
    //   UIが実際にどのタイミングで途切れて見えるかを後から突き合わせられる
    //   ようにする。
    static int s_posLogTick = 0;
    if ((++s_posLogTick % 5) == 0) {
        qDebug() << "[Gapless] t=" << dbgMs() << "POS TICK: pos=" << getPosition()
                 << "dur=" << getDuration() << "index=" << m_currentIndex
                 << "engineIndex=" << m_gaplessEngineIndex;
    }

    const uint64_t count = m_pcmEngine.GetGaplessTransitionCount();
    if (count != m_gaplessTransitionSeen) {
        m_gaplessTransitionSeen = count;
        onGaplessTransitionDetected();
    }

    // ★ 遷移を検知したかどうかに関わらず、毎回のポーリングで必ず表示状態を
    //   突き合わせる。これにより、デコードスレッドが遷移を検知した時点では
    //   まだ壁時計がその曲の開始点に届いていなくても（バックログ再生中）、
    //   後続のポーリングで自然に正しいタイミングで表示が切り替わる。
    applyGaplessDisplayCatchup();
}

// ★ PcmDualEngineが内部で次曲へシームレスに切り替わった（デコード側の
//   スワップ）ことを検知した際に一度だけ行う処理：切り替わった先の
//   インデックス確定、次の次曲の先読み再アーム、表示チェーンへの追記。
//   実際にUI（m_currentIndex等）をいつ切り替えるかはここでは決めない
//   （checkGaplessTransition()内のapplyGaplessDisplayCatchup()が
//   ポーリングで都度その場で決める）。
void Player::onGaplessTransitionDetected()
{
    const PcmDualEngine::GaplessInfo tinfo = m_pcmEngine.LastGaplessTransitionInfo();
    qDebug() << "[Gapless] t=" << dbgMs() << "TRANSITION DETECTED: count=" << m_gaplessTransitionSeen
             << "backlogFrames=" << tinfo.backlogOutputFrames
             << "nextNativeRate=" << tinfo.nativeSampleRate
             << "curPos=" << getPosition() << "curDur=" << getDuration();

    // ★ next()と同じ選曲ロジックで、実際に切り替わった先のインデックスを
    //   "この時点で即座に" 確定する（UI表示への反映は後述の通り遅らせるが、
    //   インデックス自体の選定・m_shufflePosの前進はここで一度だけ行う）。
    QMutexLocker idxLock(&m_mutex);
    if (m_repeatMode == RepeatMode::One) {
        // 同一曲扱い：インデックス変更なし
    } else if (m_shuffleMode != ShuffleMode::None && !m_shuffleList.isEmpty()) {
        m_shufflePos++;
        if (m_shufflePos >= m_shuffleList.size()) m_shufflePos = 0;
        m_gaplessEngineIndex = m_shuffleList[m_shufflePos];
    } else if (m_gaplessEngineIndex + 1 < m_playlist.size()) {
        m_gaplessEngineIndex++;
    } else {
        m_gaplessEngineIndex = 0; // RepeatMode::All（一致判定できた時点で次曲がある想定）
    }
    const int newIndex = m_gaplessEngineIndex;
    const QString newPath = (newIndex >= 0 && newIndex < m_playlist.size())
        ? m_playlist[newIndex] : QString();
    idxLock.unlock();

    // ★ 次の次曲に向けたプリロード状態を "この時点で即座に" リセットする
    //   （UI追いつき更新を待たない）。デコードスレッドは既にnewIndexの曲を
    //   再生し始めているので、その次の曲の準備はすぐに再開して構わない。
    //   これを遅らせると（旧実装のバグ）、バックログの再生時間分（短い曲では
    //   曲の長さの大半に達することもある）ここが止まったままになり、
    //   デコードスレッドが本当のEOFに達するまでに次の次曲が一度も準備
    //   されず、ギャップレスが不成立になってしまう（実機デバッグログで
    //   実際に観測・特定した不具合）。
    m_preloadDone = false;
    m_preloadedPath.clear();
    if (!newPath.isEmpty() && m_useNewEngine) {
        int nextNextIdx = peekNextIndexFrom(newIndex);
        if (nextNextIdx >= 0 && nextNextIdx < m_playlist.size()) {
            QString nextNextPath = m_playlist[nextNextIdx];
            if (!nextNextPath.startsWith("cdda://")) {
                tryPrepareGaplessNext(nextNextPath);
                m_preloadDone = true;
            }
        }
    }

    // ★ 新方式：ここでUI（m_currentIndex等）をタイマーで遅延させて追いつか
    //   せるのはやめ、今切り替わった曲を「表示チェーン」に1件追記するだけに
    //   する。実際にいつこの曲の表示へ切り替えるべきかは、下のポーリング
    //   （computeGaplessDisplayState()、経過壁時計とチェーンの累積長を
    //   突き合わせるだけ）が都度その場で計算するので、個別タイマーの
    //   発火順序や「どれを捨てるか」の判定が要らなくなる。短い曲が連続して
    //   デコードスレッドが一気に数曲先まで進んでも、チェーンには全曲分が
    //   漏れなく追記されており、ポーリングはその時点の壁時計に対応する
    //   区間へ自然にたどり着くため、途中の曲の表示が丸ごと飛ばされる
    //   （以前の不具合）ことが原理的に起きない。
    const double newDurationSec = (tinfo.nativeSampleRate > 0)
        ? static_cast<double>(tinfo.totalFrames) / static_cast<double>(tinfo.nativeSampleRate) : 0.0;
    m_gaplessChain.append({ newIndex, newDurationSec,
                             tinfo.nativeSampleRate, tinfo.bitsPerSample });
    qDebug() << "[Gapless] t=" << dbgMs() << "CHAIN APPEND: newIndex=" << newIndex
             << "durationSec=" << newDurationSec << "chainLen=" << m_gaplessChain.size();

    // ★ チェーン追記後、即座に表示状態を再計算して反映する（次のポーリング
    //   まで待たない）。この関数はチェーンが更新されたかどうかに関わらず
    //   常に呼んでも安全（経過時間から都度正しい区間を導くだけの純粋な
    //   計算のため）。
    applyGaplessDisplayCatchup();
}

// ★ m_gaplessChain（チェーン開始からの経過壁時計に対応する曲の並び）と
//   実際の経過時間（m_newEnginePositionBase + m_newEngineElapsedTimer）を
//   突き合わせ、「今表示すべき曲・位置」を求めて、必要ならm_currentIndex等の
//   UI状態をそこへ追いつかせる。checkGaplessTransition()の周期ポーリングと
//   遷移検知直後の両方から呼ばれる。
void Player::applyGaplessDisplayCatchup()
{
    const GaplessDisplayState st = computeGaplessDisplayState();
    if (st.index < 0 || st.index == m_currentIndex) return;

    qDebug() << "[Gapless] t=" << dbgMs() << "UI CATCHUP (poll-driven): oldIndex=" << m_currentIndex
             << "newIndex=" << st.index << "posInTrack=" << st.posInTrack
             << "durationSec=" << st.durationSec;

    {
        QMutexLocker lock(&m_mutex);
        m_currentIndex = st.index;
    }

    // ★ 長さ・infoキャッシュを新トラックに合わせて更新。
    //   m_newEnginePositionBase／m_newEngineElapsedTimerは「チェーン開始
    //   からの累積経過時間」を表す唯一の真実の源であり、曲が切り替わっても
    //   リセットしない（ここが旧実装との最大の違い）。
    m_newEngineDuration = st.durationSec;
    m_newEngineEofFired = false;
    m_cachedSr   = static_cast<int>(st.nativeSampleRate);
    m_cachedBits = static_cast<int>(st.bitsPerSample);

    const QString newPath = (st.index >= 0 && st.index < m_playlist.size())
        ? m_playlist[st.index] : QString();
    if (!newPath.isEmpty()) {
        int capturedSerial = ++m_playSerial;
        fetchTagsAsync(newPath, st.index, capturedSerial);
    }
}

// ★ 経過壁時計（m_newEnginePositionBase + 再生中ならm_newEngineElapsedTimer
//   の経過分）をm_gaplessChainの累積長と突き合わせ、「今どの曲の何秒目を
//   表示すべきか」を求める。チェーンの最後の曲を過ぎた分は、最後の曲の
//   末尾（durationSecでクランプ）にとどめる。
Player::GaplessDisplayState Player::computeGaplessDisplayState() const
{
    GaplessDisplayState st;
    if (m_gaplessChain.isEmpty()) return st;

    double elapsed = m_newEnginePositionBase;
    if (m_playing) elapsed += m_newEngineElapsedTimer.elapsed() / 1000.0;
    if (elapsed < 0.0) elapsed = 0.0;

    double cum = 0.0;
    for (int i = 0; i < m_gaplessChain.size(); ++i) {
        const GaplessChainEntry &e = m_gaplessChain[i];
        const bool isLast = (i == m_gaplessChain.size() - 1);
        if (isLast || elapsed < cum + e.durationSec) {
            st.index           = e.index;
            st.durationSec     = e.durationSec;
            st.nativeSampleRate = e.nativeSampleRate;
            st.bitsPerSample    = e.bitsPerSample;
            double pos = elapsed - cum;
            if (pos < 0.0) pos = 0.0;
            if (pos > e.durationSec) pos = e.durationSec;
            st.posInTrack = pos;
            return st;
        }
        cum += e.durationSec;
    }
    return st; // 理論上到達しない
}

// 次曲ファイルをバックグラウンドで一度読み切り、OSのファイルキャッシュに
// 乗せておく。これにより実際のloadfile時のディスクI/O待ちを短縮する。
// デコード結果は保持しない（メモリ節約・実装の単純化のため）。
void Player::preloadNextFile(const QString &path)
{
    m_preloadedPath = path;
    QThreadPool::globalInstance()->start([path]{
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return;
        char buf[64 * 1024];
        while (f.read(buf, sizeof(buf)) > 0) {
            // 読み捨てるだけ：目的はOSページキャッシュへの先読み
        }
    });
}


void Player::setManualRateOverride(bool active)
{
    const bool wasPinned = m_manualRateOverride && m_pinRate > 0;
    m_manualRateOverride = active;
    if (!active) {
        m_pinRate = m_pinBits = 0;
        // 手動指定を解除したので、自動の出力形式で開き直す
        if (wasPinned && m_useNewEngine) reloadNewEngineForRateChange();
    }
}

void Player::setPinnedOutput(int rate, int bits)
{
    const bool changed = !(m_manualRateOverride && m_pinRate == rate && m_pinBits == bits);
    m_manualRateOverride = true;
    m_pinRate = rate;
    m_pinBits = bits;
    if (changed && m_useNewEngine) reloadNewEngineForRateChange();
}

void Player::setVolume(int vol)
{
    m_volume = qBound(0, vol, 100);
    if (m_newEngineProcessThread)
        m_newEngineProcessThread->SetGain(volumeToGain(m_volume));
}

void Player::setMode(const QString &mode, bool hp1, bool hp2, const QString &soundField)
{
    // ★ v10: 新エンジン再生中に「何も変わらない」setModeが来た場合は何もしない。
    //   曲が切り替わるたびにMainWindowが同じモードを設定し直してくるが、
    //   以前はそのたびに再オープン(reloadNewEngineForRateChange)していたため、
    //   ギャップレスでつながったばかりの曲がそこで途切れていた。
    //   （実際の出力レートが今のモードの目標レートと一致しているかも確認する。
    //    setModeQuiet()でモードだけ先に書き換わっている場合は再オープンが必要）
    if (m_useNewEngine && mode == m_mode && hp1 == m_hp1 && hp2 == m_hp2 && soundField == m_soundField
        && m_pcmEngine.GetSampleRate() == computeNewEngineTargetRate(m_pcmEngine.GetNativeSampleRate()))
        return;

    m_mode       = mode;
    m_hp1        = hp1;
    m_hp2        = hp2;
    m_soundField = soundField;
    if (m_useNewEngine) {
        // ★ HP補正・音場・ラウドネスは同レートのまま即座に反映（リロード不要）。
        //   ただしdsd8/hires4はレート変更(SincResamplerの目標レート)を伴うため、
        //   モード変更自体はmpv経路のapplyAudioChainAndReload()と同様、
        //   再オープンで反映する。
        m_newEngineDsp.SetHp(hp1, hp2);
        m_newEngineDsp.SetSoundField(soundFieldModeFromString(soundField));
        m_newEngineDsp.SetLoudnessOn(mode == "loudness");
        if (m_playing || m_paused) {
            reloadNewEngineForRateChange();
        }
    }
}

// static
SoundFieldMode Player::soundFieldModeFromString(const QString &s)
{
    if (s == "wowflutter") return SoundFieldMode::WowFlutter;
    if (s == "halltone")   return SoundFieldMode::HallTone;
    return SoundFieldMode::None;
}

double Player::getPosition() const
{
    if (m_useNewEngine) {
        // ★ m_newEnginePositionBase/m_newEngineElapsedTimerはチェーン開始
        //   からの累積経過時間であり、曲単体の位置ではない。表示用の
        //   「今の曲の中の位置」はm_gaplessChainと突き合わせて求める。
        if (!m_gaplessChain.isEmpty()) {
            return computeGaplessDisplayState().posInTrack;
        }
        // フォールバック（チェーン未設定時。通常は起きない）
        if (!m_playing) return m_newEnginePositionBase;
        double pos = m_newEnginePositionBase + m_newEngineElapsedTimer.elapsed() / 1000.0;
        if (pos > m_newEngineDuration) pos = m_newEngineDuration;
        return pos;
    }
    return 0.0;
}

double Player::getDuration() const
{
    return m_useNewEngine ? m_newEngineDuration : 0.0;
}

QString Player::getTagTitle() const
{
    if (m_currentIndex >= m_playlist.size()) return {};
    QString file = m_playlist[m_currentIndex];
    QString ext  = QFileInfo(file).suffix().toLower();
    if (ext == "flac") {
        TagLib::FLAC::File tf(file.toStdWString().c_str());
        if (tf.isValid() && tf.tag()) {
            QString t = QString::fromUtf8(tf.tag()->title().toCString(true));
            if (!t.isEmpty()) return t;
        }
    } else if (ext == "mp3") {
        TagLib::MPEG::File tf(file.toStdWString().c_str());
        if (tf.isValid() && tf.tag()) {
            QString t = QString::fromUtf8(tf.tag()->title().toCString(true));
            if (!t.isEmpty()) return t;
        }
    } else if (ext == "m4a" || ext == "mp4" || ext == "aac") {
        TagLib::MP4::File tf(file.toStdWString().c_str());
        if (tf.isValid() && tf.tag()) {
            QString t = QString::fromUtf8(tf.tag()->title().toCString(true));
            if (!t.isEmpty()) return t;
        }
    }
    return QFileInfo(file).completeBaseName();
}

QString Player::getTagArtist() const
{
    if (m_currentIndex >= m_playlist.size()) return {};
    QString file = m_playlist[m_currentIndex];
    QString ext  = QFileInfo(file).suffix().toLower();
    if (ext == "flac") {
        TagLib::FLAC::File tf(file.toStdWString().c_str());
        if (tf.isValid() && tf.tag())
            return QString::fromUtf8(tf.tag()->artist().toCString(true));
    } else if (ext == "mp3") {
        TagLib::MPEG::File tf(file.toStdWString().c_str());
        if (tf.isValid() && tf.tag())
            return QString::fromUtf8(tf.tag()->artist().toCString(true));
    } else if (ext == "m4a" || ext == "mp4" || ext == "aac") {
        TagLib::MP4::File tf(file.toStdWString().c_str());
        if (tf.isValid() && tf.tag())
            return QString::fromUtf8(tf.tag()->artist().toCString(true));
    }
    return {};
}

QString Player::getInfo(const QString &mode) const
{
    // コーデック名：再生中はmpvから、再生前は拡張子から
    QString info;
    if (m_playing && m_useNewEngine) {
        // ★ v10: 自作エンジン再生中はmpvが止まっているのでコーデック名を
        //   mpvから取れないので、拡張子を表示する。（開発中に入れていた
        //   「(Always Engine)」表記は、全形式が自作エンジンになったので外した）
        if (m_currentIndex >= 0 && m_currentIndex < m_playlist.size())
            info = QFileInfo(m_playlist[m_currentIndex]).suffix().toUpper();
    } else {
        if (m_currentIndex < m_playlist.size())
            info = QFileInfo(m_playlist[m_currentIndex]).suffix().toUpper();
    }

    // kbps：タグから読んだ平均ビットレート（v10: mpvのリアルタイム値は廃止）
    const int br = m_playing ? m_cachedBr : 0;

    // sr/bits はキャッシュ値（onTrackChangedで更新済み）
    int sr   = m_cachedSr;
    int bits = m_cachedBits;

    // 表示用モード
    const QString &dispMode = mode.isEmpty() ? m_mode : mode;
    bool hiRes = (sr > 48000);

    // 出力kHz：モードに連動
    double outKhz = 0.0;
    int    outBits = 24; // s24固定出力
    if (sr > 0) {
        if (dispMode == "dsd8") {
            outKhz = 352.8;
        } else if (dispMode == "hires4" && !hiRes) {
            outKhz = 176.4;
        } else if (dispMode == "loudness" && !hiRes) {
            outKhz = 176.4;
        } else {
            // pure / ハイレゾ：ソースのkHzをそのまま
            outKhz = sr / 1000.0;
            // ピュアでCD音源の場合のみ16bit表示
            if (dispMode == "pure" && !hiRes && bits > 0)
                outBits = bits;
        }
    }

    info += QString(" | %1 kbps").arg(br);
    // ★ v10: 自作エンジン再生中は、モードから推定した値ではなく、実際にDACへ
    //   出している値を表示する（DACが352.8kHzを受け付けず176.4kHzに下げた場合や、
    //   手動ビットパーフェクトで出力形式を指定した場合に、表示が食い違っていた）。
    if (m_useNewEngine && m_newEngineOutputSr > 0) {
        outKhz  = m_newEngineOutputSr / 1000.0;
        outBits = m_newEngineOutput.GetValidBits();
    }
    if (outKhz > 0)
        info += QString(" | %1 kHz").arg(outKhz, 0, 'f', 1);
    // v10: 共有モード（Bluetoothなど）はビット数ではなく「共有モード」と表示
    if (isSharedOutput())
        info += QString::fromUtf8(" / 共有モード");
    else
        info += QString(" / %1bit").arg(outBits);
    return info;
}

QString Player::getCoverArt() const
{
    if (m_currentIndex >= m_playlist.size()) return {};
    QString fp = m_playlist[m_currentIndex];
    // ★ CD再生中はアートなし
    if (fp.startsWith("cdda://")) return {};
    QFileInfo fi(fp);
    QString ext = fi.suffix().toLower();

    QByteArray imgData;
    QString    imgMime;

    auto normalizeMime = [](const QString &m) {
        QString mm = m.toLower();
        if (mm.contains("png")) return QString("image/png");
        if (mm.contains("jpg") || mm.contains("jpeg") || mm.contains("jpe") || mm.contains("jfif"))
            return QString("image/jpeg");
        return QString("image/jpeg"); // デフォルト
    };

    // ───────────────────────────────────────────────
    // MP3: APIC を全列挙し、Front Cover を優先
    // ───────────────────────────────────────────────
    if (ext == "mp3") {
        TagLib::MPEG::File f(fp.toStdWString().c_str());
        if (f.ID3v2Tag()) {
            auto frames = f.ID3v2Tag()->frameListMap()["APIC"];

            TagLib::ID3v2::AttachedPictureFrame *best = nullptr;

            for (auto *fr : frames) {
                auto *apic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(fr);
                if (!apic) continue;

                // type=3 が Front Cover
                if (apic->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) {
                    best = apic;
                    break;
                }

                // description に front が含まれる場合も優先
                QString desc = QString::fromUtf8(apic->description().toCString(true)).toLower();
                if (desc.contains("front")) {
                    best = apic;
                }

                // fallback として最初の1枚
                if (!best) best = apic;
            }

            if (best) {
                imgData = QByteArray(best->picture().data(), best->picture().size());
                imgMime = normalizeMime(QString::fromStdString(best->mimeType().to8Bit()));
            }
        }
    }

    // ───────────────────────────────────────────────
    // WAV: ID3v2タグのAPICを取得
    // ───────────────────────────────────────────────
    else if (ext == "wav") {
        TagLib::RIFF::WAV::File f(fp.toStdWString().c_str());
        if (f.hasID3v2Tag() && f.ID3v2Tag()) {
            auto frames = f.ID3v2Tag()->frameListMap()["APIC"];
            TagLib::ID3v2::AttachedPictureFrame *best = nullptr;
            for (auto *fr : frames) {
                auto *apic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(fr);
                if (!apic) continue;
                if (apic->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) { best = apic; break; }
                if (!best) best = apic;
            }
            if (best) {
                imgData = QByteArray(best->picture().data(), best->picture().size());
                imgMime = normalizeMime(QString::fromStdString(best->mimeType().to8Bit()));
            }
        }
    }

    // ───────────────────────────────────────────────
    // FLAC: PictureType=3（Front Cover）を優先
    // ───────────────────────────────────────────────
    else if (ext == "flac") {
        TagLib::FLAC::File f(fp.toStdWString().c_str());
        if (!f.pictureList().isEmpty()) {
            TagLib::FLAC::Picture *best = nullptr;

            for (auto *pic : f.pictureList()) {
                if (pic->type() == TagLib::FLAC::Picture::FrontCover) {
                    best = pic;
                    break;
                }
                if (!best) best = pic;
            }

            if (best) {
                imgData = QByteArray(best->data().data(), best->data().size());
                imgMime = normalizeMime(QString::fromStdString(best->mimeType().to8Bit()));
            }
        }
    }

    // ───────────────────────────────────────────────
    // MP4/M4A: covr を複数対応
    // ───────────────────────────────────────────────
    else if (ext == "m4a" || ext == "mp4" || ext == "aac") {
        TagLib::MP4::File f(fp.toStdWString().c_str());
        if (f.tag()) {
            auto items = f.tag()->itemMap();
            if (items.contains("covr")) {
                auto covers = items["covr"].toCoverArtList();
                if (!covers.isEmpty()) {
                    auto c = covers.front();
                    imgData = QByteArray(c.data().data(), c.data().size());
                    imgMime = "image/jpeg"; // MP4 はほぼ JPEG
                }
            }
        }
    }

    // ───────────────────────────────────────────────
    // 埋め込み画像があれば一時ファイルに書き出す
    // ───────────────────────────────────────────────
    if (!imgData.isEmpty()) {
        QString ext2 = imgMime.contains("png") ? ".png" : ".jpg";

        // キャッシュ対策：ファイルパスのハッシュを使う
        QString hash = QString::number(qHash(fp));
        QString tmp  = QDir::tempPath() + "/always_cover_" + hash + ext2;

        QFile tf(tmp);
        if (tf.open(QIODevice::WriteOnly)) {
            tf.write(imgData);
            tf.close();
            return tmp;
        }
    }

    // ───────────────────────────────────────────────
    // フォルダ画像 fallback（あなたの現行コードをそのまま活かす）
    // ───────────────────────────────────────────────
    QDir dir = fi.absoluteDir();
    static const QStringList candidates = {
        "cover.jpg","cover.png","cover.webp","cover.bmp",
        "Cover.jpg","Cover.png","Cover.webp",
        "folder.jpg","folder.png","folder.webp","Folder.jpg",
        "front.jpg","front.png","Front.jpg",
        "artwork.jpg","artwork.png","Artwork.jpg",
        "AlbumArt.jpg","AlbumArt.png","albumart.jpg",
        "thumb.jpg","thumb.png","Thumb.jpg",
        "image.jpg","image.png","Image.jpg",
        "album.jpg","album.png","Album.jpg",
    };
    for (const auto &name : candidates) {
        QString path = dir.absoluteFilePath(name);
        if (QFile::exists(path)) return path;
    }

    dir.setNameFilters({
        "*.jpg","*.jpeg","*.png","*.webp",
        "*.bmp","*.tiff","*.tif","*.gif",
        "*.JPG","*.JPEG","*.PNG","*.WEBP",
        "*.BMP","*.TIFF","*.GIF"
    });
    dir.setFilter(QDir::Files);
    auto list = dir.entryInfoList();
    if (!list.isEmpty()) return list.first().absoluteFilePath();

    return {};
}

QString Player::currentFile() const
{
    if (m_currentIndex < m_playlist.size())
        return QFileInfo(m_playlist[m_currentIndex]).fileName();
    return {};
}

QString Player::currentFilePath() const
{
    if (m_currentIndex < m_playlist.size())
        return m_playlist[m_currentIndex];
    return {};
}

QString Player::fileAt(int i) const
{
    if (i >= 0 && i < m_playlist.size())
        return QFileInfo(m_playlist[i]).fileName();
    return {};
}

QString Player::filePathAt(int i) const
{
    if (i >= 0 && i < m_playlist.size())
        return m_playlist[i];
    return {};
}

void Player::getAudioLevels(float &left, float &right) const
{
    // ★ v10: 自作エンジン（WASAPI排他）再生中は、エンジン自身が測った
    //   「DACへ渡した瞬間の実レベル」を使う。排他モードの音はループバックで
    //   取れず、mpvも止まっているため、従来の方法では針が動かなかった。
    if (m_useNewEngine) {
        if (m_playing) m_newEngineOutput.GetLevels(left, right);
        else left = right = 0.f;
        return;
    }
    left = right = 0.f;
}
