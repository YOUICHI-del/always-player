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
#include <windows.h>

const QStringList Player::SUPPORTED_EXT = {
    "mp3","aac","ogg","wav","flac","opus","dsf","dff","m4a","aiff","wv"
};

Player::Player(QObject *parent) : QObject(parent) {}

Player::~Player()
{
    stop();
    if (m_mpv) mpv_terminate_destroy(m_mpv);
}

bool Player::init()
{
    m_mpv = mpv_create();
    if (!m_mpv) { emit errorOccurred("mpv初期化失敗"); return false; }

    // ★ audio-raw-data-pull は option → initialize 前に設定
    mpv_set_option_string(m_mpv, "audio-raw-data-pull", "yes");

    // ★ configファイルを無効化（前回の状態が復元されるのを防ぐ）
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "no-resume-playback", "yes");

    mpv_set_option_string(m_mpv, "video",        "no");
    mpv_set_option_string(m_mpv, "really-quiet", "yes");
    mpv_set_option_string(m_mpv, "cache",        "yes");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes",      "512MiB");  // 最大512MBをRAMに展開
    mpv_set_option_string(m_mpv, "demuxer-readahead-secs", "300");     // 300秒先読み（曲丸ごと）
    mpv_set_option_string(m_mpv, "audio-buffer", "0.1");
    mpv_set_option_string(m_mpv, "demuxer",      "lavf"); // packet-bitrateを返す曲を増やす

    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

    // ★ initialize（ここより前は option、ここより後は property）
    if (mpv_initialize(m_mpv) < 0) {
        emit errorOccurred("mpv初期化失敗");
        return false;
    }

    // ★ audio-raw-data 系は initialize 後に property として設定
    mpv_set_property_string(m_mpv, "audio-raw-data", "yes");
    mpv_set_property_string(m_mpv, "audio-raw-data-format", "float");

    // ★ observe は initialize 後
    mpv_observe_property(m_mpv, 0, "audio-raw-data", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, 0, "playlist-pos", MPV_FORMAT_INT64);

    mpv_observe_property(m_mpv, 0, "time-pos",   MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "duration",   MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "demuxer-cache-state", MPV_FORMAT_NODE);

    QThread *th = new QThread(this);
    connect(th, &QThread::started, [this]{ mpvEventLoop(); });
    th->start();

    return true;
}

void Player::mpvEventLoop()
{
    qDebug() << "[RAW EVENT]";

    while (true) {
        mpv_event *ev = mpv_wait_event(m_mpv, 0.5);
        if (!ev) continue;
        if (ev->event_id == MPV_EVENT_SHUTDOWN) break;

        if (ev->event_id == MPV_EVENT_END_FILE) {
            auto *ef = (mpv_event_end_file*)ev->data;
            if (ef->reason == MPV_END_FILE_REASON_EOF) {
                // ★ 1曲ずつ方式：next()→play()→loadfile replace
                // mpv playlistを使わないのでdrain待ち不要
                QTimer::singleShot(0, this, [this]{
                    qDebug() << "[Player] EOF → next()";
                    next();
                });
            }
        }

        if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART) {
            emit playbackStarted();
        }

        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto *prop = (mpv_event_property*)ev->data;

            // デバッグ
            qDebug() << "[PROP]" << prop->name << prop->format;

            // demuxer-cache-state から packet-bitrate を取得（FLAC等のリアルタイムkbps）
            if (prop && strcmp(prop->name, "demuxer-cache-state") == 0 &&
                prop->format == MPV_FORMAT_NODE) {
                mpv_node *node = (mpv_node*)prop->data;
                if (node && node->format == MPV_FORMAT_NODE_MAP) {
                    mpv_node_list *lst = node->u.list;
                    for (int i = 0; i < lst->num; i++) {
                        if (strcmp(lst->keys[i], "packet-bitrate") == 0 &&
                            lst->values[i].format == MPV_FORMAT_DOUBLE) {
                            double pb = lst->values[i].u.double_;
                            if (pb > 0) m_realtimeBr = static_cast<int>(pb / 1000.0);
                        }
                    }
                }
            }

            if (prop && strcmp(prop->name, "audio-raw-data") == 0 &&
                prop->format == MPV_FORMAT_NODE) {

                mpv_node *node = (mpv_node*)prop->data;

                if (node && node->format == MPV_FORMAT_NODE_MAP) {
                    processAudioRawData(node);
                    qDebug() << "[RAW] PCM arrived";
                }
            }

            // ★ playlist-pos変化 → CD再生中のみtrackChangedを発火
            // ファイル再生はreplace方式のためplaylist-posは常に0になるので無視
            if (prop && strcmp(prop->name, "playlist-pos") == 0 &&
                prop->format == MPV_FORMAT_INT64 &&
                !m_playlist.isEmpty() && m_playlist[0].startsWith("cdda://")) {
                int64_t pos = *(int64_t*)prop->data;
                if (pos >= 0 && pos != m_currentIndex) {
                    m_currentIndex = static_cast<int>(pos);
                    int capturedIndex = m_currentIndex;
                    qDebug() << "[Player] playlist-pos changed (CD):" << capturedIndex;
                    QMetaObject::invokeMethod(this, [this, capturedIndex]{
                        emit trackChanged(capturedIndex, QString(), QString(), QString());
                    }, Qt::QueuedConnection);
                }
            }


        }
    }
}

// ── 音質チェーン適用
// audio-samplerateはmpv_initialize後はset_propertyで設定
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

void Player::applyAudioChain()
{
    QString af;

    // ソースのサンプリング周波数を取得（ハイレゾ判定用）
    int srcSr = 0;
    if (m_currentIndex < m_playlist.size()) {
        // ★ cdda:// URIはTagLibで開けないのでスキップ
        QString currentPath = m_playlist[m_currentIndex];
        if (!currentPath.startsWith("cdda://")) {
            TagLib::FileRef ref(currentPath.toStdWString().c_str());
            if (!ref.isNull() && ref.audioProperties())
                srcSr = ref.audioProperties()->sampleRate();
        }
    }
    bool isHiRes = (srcSr > 48000);

    // ── CPUスペックに応じたprecision自動判定
    // AVX-512 → precision=32 / AVX2 → precision=28 / SSE2 → precision=24
    static const int s_precision = []() -> int {
        int info[4] = {};
        __cpuid(info, 0);
        int nIds = info[0];
        if (nIds >= 7) {
            __cpuidex(info, 7, 0);
            if (info[1] & (1 << 16)) return 32;  // AVX-512F
            if (info[1] & (1 <<  5)) return 28;  // AVX2
        }
        __cpuid(info, 1);
        if (info[3] & (1 << 26)) return 24;  // SSE2
        return 24;
    }();
    const QString RESAMP = QString("aresample=resampler=swr:precision=%1:cutoff=0.9998")
                           .arg(s_precision);

    if (m_mode == "hires4") {
        if (isHiRes) {
            // ハイレゾはそのまま（アップサンプリングしない）
            mpv_set_property_string(m_mpv, "audio-samplerate", "0");
        } else {
            af = RESAMP;
            mpv_set_property_string(m_mpv, "audio-samplerate", "176400");
        }
    } else if (m_mode == "dsd8") {
        af = RESAMP;
        mpv_set_property_string(m_mpv, "audio-samplerate", "352800");
    } else if (m_mode == "loudness") {
        if (isHiRes) {
            af = "lavfi=loudnorm=I=-14:TP=-1:LRA=11";
            mpv_set_property_string(m_mpv, "audio-samplerate", "0");
        } else {
            af = RESAMP + ",lavfi=loudnorm=I=-14:TP=-1:LRA=11";
            mpv_set_property_string(m_mpv, "audio-samplerate", "176400");
        }
    } else {
        // pure
        mpv_set_property_string(m_mpv, "audio-samplerate", "0");
    }

    // ── SAEC的音質チェーン
    // [1] プレゼンス帯域 +0.8dB（3.2kHz中心）
    //     中音域の音像・実体感・定位を強調。equalizer は線形位相に近い。
    // [2] 高域 allpass（8kHz / Q=0.7）
    //     位相を整えて空間の見通し感・解像度感を出す。振幅特性は変えない。
    // [3] 空芯コイル模倣（lowpass poles=1 / f=36000）
    //     超高域をなだらかに丸める。倍音付加ゼロ。ユーザー設定値を反映。
QString saec =
    "equalizer=f=3200:width_type=o:width=1.5:g=0.5"
    ",allpass=f=8000:width_type=q:width=0.7"
    ",lowpass=f=45000:poles=1"
    ",lavfi=[aeval=exprs='val(0)+0.03*pow(val(0),2)|val(1)+0.03*pow(val(1),2)']";

af = af.isEmpty() ? saec : af + "," + saec;

// HP補正
if (m_hp1) {
    af += ",bs2b=cmoy";
} else if (m_hp2) {
    af += ",bs2b=jmeier";
}

// 音場効果（オプション）
if (m_soundField == "wowflutter") {
    af += ",vibrato=f=0.5:d=0.0008"; // ワウフラッター（アナログレコード化）
} else if (m_soundField == "halltone") {
    af += ",aecho=0.98:0.98:60:0.02"; // ホールトーン（3メーター以内の試聴で強化）
}

mpv_set_property_string(m_mpv, "audio-format", "s24"); // 24bit固定出力
mpv_set_property_string(m_mpv, "af", af.toUtf8().constData());
}

// ─────────────────────────────────────────────
// 起動ウォームアップ
// ─────────────────────────────────────────────
void Player::preWarm()
{
    QThread::msleep(200);

    const char *cmd1[] = {
        "loadfile",
        "lavfi://anoisesrc=color=pink:amplitude=0.0003:duration=0.8",
        "replace",
        nullptr
    };
    mpv_command(m_mpv, cmd1);

    double vol = 1.0;
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &vol);

    QThread::msleep(800);

    const char *cmd2[] = {
        "loadfile",
        "lavfi://anullsrc=channel_layout=stereo:sample_rate=44100",
        "replace",
        nullptr
    };
    mpv_command(m_mpv, cmd2);

    QThread::msleep(400);

    double v = m_volume;
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &v);
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

void Player::loadFile(const QString &filePath)
{
    QMutexLocker lock(&m_mutex);
    stop();
    m_playlist.clear();
    m_playlist << filePath;
    m_currentIndex = 0;
}

// ★ CD再生用：cdda:// URI のリストをそのまま playlist にセット
void Player::loadPlaylist(const QStringList &paths)
{
    QMutexLocker lock(&m_mutex);
    stop();
    m_playlist     = paths;
    m_currentIndex = 0;
}

// ★ Named Pipe経由CDストリーミング再生
// mpvにraw PCMストリームとして渡す
// ★ 再生を止めずにプレイリストを追加・更新
void Player::appendPlaylist(const QStringList &paths)
{
    QMutexLocker lock(&m_mutex);
    // ★ 現在のインデックスを記録して全曲を追加
    // 1曲目は既にreplace済みなので2曲目以降をappendで追加
    int cur = m_currentIndex;
    m_playlist = paths;
    m_currentIndex = cur;

    // mpvのプレイリストに2曲目以降を追加
    for (int i = 1; i < paths.size(); i++) {
        QByteArray p = paths[i].toUtf8();
        const char *cmd[] = {"loadfile", p.constData(), "append", nullptr};
        mpv_command(m_mpv, cmd);
        qDebug() << "[Player] append:" << paths[i];
    }

    qDebug() << "[Player] appendPlaylist done: total=" << paths.size();
}

void Player::clearPlaylist()
{
    QMutexLocker lock(&m_mutex);
    m_playlist.clear();
    m_currentIndex = 0;
    m_playing = false;
    m_paused  = false;
}

void Player::loadCdDirect(const QString &driveLetter)
{
    // First ModeはWAVリッピング方式に変更（cdda://はmpvビルドに依存するため）
    // この関数は使用しない（MainWindowのstartCdFirstModeで直接制御）
    Q_UNUSED(driveLetter)
    qDebug() << "[Player] loadCdDirect: not used";
}

void Player::loadCdStream(const QString &filePath)
{
    QMutexLocker lock(&m_mutex);
    m_playlist.clear();
    m_playlist << filePath;
    m_currentIndex = 0;

    qDebug() << "[Player] loadCdStream:" << filePath;

    // ★ シンプルに通常WAVとして扱う
    // まず音を出すことを優先（チューニングは後で）
    // RAM展開再生（init時に設定済み）

    QByteArray path = filePath.toUtf8();
    const char *cmd[] = {"loadfile", path.constData(), "replace", nullptr};
    mpv_command(m_mpv, cmd);

    m_playing = true;
    m_paused  = false;

    double vol = static_cast<double>(m_volume);
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
}

void Player::setMediaTitle(const QString &title)
{
    QMutexLocker lock(&m_mutex);
    QByteArray t = title.toUtf8();
    mpv_set_property_string(m_mpv, "force-media-title", t.constData());
    qDebug() << "[Player] setMediaTitle:" << title;
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
    if (m_playlist.isEmpty()) return;
    if (index >= 0) m_currentIndex = index;
    if (m_currentIndex >= m_playlist.size()) return;

    QString file = m_playlist[m_currentIndex];

    // ★ cdda:// URI は拡張子チェック・TagLib処理をスキップ
    bool isCd = file.startsWith("cdda://");

    if (!isCd) {
        QString ext = QFileInfo(file).suffix().toLower();
        if (!SUPPORTED_EXT.contains(ext)) {
            emit errorOccurred(QString("非対応フォーマット: %1").arg(ext));
            return;
        }
    }

// 音質チェーン適用
applyAudioChain();

// ★ 1曲だけloadfile replace（mpv playlist使用禁止）
// mpv playlist + RAW PCM pull の組み合わせはEOFタイミングが壊れる
QByteArray utf8 = file.toUtf8();
const char *cmd[] = {"loadfile", utf8.constData(), "replace", nullptr};
mpv_command(m_mpv, cmd);

m_playing = true;
m_paused  = false;

// 音量を元に戻す
double vol = static_cast<double>(m_volume);
mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &vol);

// ★ CD再生はタグなし：インデックスのみ通知して終了
if (isCd) {
    int capturedIndex = m_currentIndex;
    QMetaObject::invokeMethod(this, [this, capturedIndex]{
        emit trackChanged(capturedIndex, QString(), QString(), QString());
    }, Qt::QueuedConnection);
    return;
}

// タグ読み取りを別スレッドで行いUIスレッドをブロックしない
int capturedIndex = m_currentIndex;
QString capturedFile = file;
QtConcurrent::run([this, capturedFile, capturedIndex]{
    QString tagTitle, tagArtist;
    int br = 0, sr = 0, bits = 0;
    QString ext2 = QFileInfo(capturedFile).suffix().toLower();

    if (ext2 == "flac") {
        TagLib::FLAC::File tf(capturedFile.toStdWString().c_str());
        if (tf.isValid()) {
            if (tf.tag()) {
                tagTitle  = QString::fromStdString(tf.tag()->title().to8Bit(true));
                tagArtist = QString::fromStdString(tf.tag()->artist().to8Bit(true));
            }
            if (tf.audioProperties()) {
                br   = tf.audioProperties()->bitrate();
                sr   = tf.audioProperties()->sampleRate();
                bits = tf.audioProperties()->bitsPerSample();
            }
        }
    } else if (ext2 == "mp3") {
        TagLib::MPEG::File tf(capturedFile.toStdWString().c_str());
        if (tf.isValid()) {
            if (tf.tag()) {
                tagTitle  = QString::fromStdString(tf.tag()->title().to8Bit(true));
                tagArtist = QString::fromStdString(tf.tag()->artist().to8Bit(true));
            }
            if (tf.audioProperties()) {
                br = tf.audioProperties()->bitrate();
                sr = tf.audioProperties()->sampleRate();
            }
        }
    } else if (ext2 == "m4a" || ext2 == "mp4" || ext2 == "aac") {
        TagLib::MP4::File tf(capturedFile.toStdWString().c_str());
        if (tf.isValid()) {
            if (tf.tag()) {
                tagTitle  = QString::fromStdString(tf.tag()->title().to8Bit(true));
                tagArtist = QString::fromStdString(tf.tag()->artist().to8Bit(true));
            }
            if (tf.audioProperties()) {
                br   = tf.audioProperties()->bitrate();
                sr   = tf.audioProperties()->sampleRate();
                bits = tf.audioProperties()->bitsPerSample();
            }
        }
    } else if (ext2 == "wav" || ext2 == "aiff" || ext2 == "wv") {
        TagLib::FileRef tf(capturedFile.toStdWString().c_str());
        if (!tf.isNull()) {
            if (tf.tag()) {
                tagTitle  = QString::fromStdString(tf.tag()->title().to8Bit(true));
                tagArtist = QString::fromStdString(tf.tag()->artist().to8Bit(true));
            }
            if (tf.audioProperties()) {
                br   = tf.audioProperties()->bitrate();
                sr   = tf.audioProperties()->sampleRate();
            }
        }
    }
    if (tagTitle.isEmpty())
        tagTitle = QFileInfo(capturedFile).completeBaseName();

    // キャッシュ更新とシグナル発行はUIスレッドで
    // ★ 現在のインデックスと一致する場合のみ発行（古いスレッド結果を捨てる）
    QMetaObject::invokeMethod(this, [this, capturedIndex, capturedFile,
                                      tagTitle, tagArtist, br, sr, bits]{
        if (capturedIndex != m_currentIndex) {
            qDebug() << "[Player] trackChanged discarded (stale):" << capturedIndex << "current:" << m_currentIndex;
            return;
        }
        m_cachedBr   = br;
        m_cachedSr   = sr;
        m_cachedBits = bits;
        emit trackChanged(capturedIndex, QFileInfo(capturedFile).fileName(),
                          tagTitle, tagArtist);
    }, Qt::QueuedConnection);
});
}
void Player::pause()
{
    if (!m_playing) return;
    int v = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &v);
    m_playing = false;
    m_paused  = true;
    emit playbackPaused();
}

void Player::resume()
{
    if (m_paused) {
        int v = 0;
        mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &v);
        m_playing = true;
        m_paused  = false;
        emit playbackStarted();
    } else {
        play();
    }
}

void Player::stop()
{
    if (m_mpv) {
        // ★ stopしてプレイリストもクリア
        const char *stopCmd[] = {"stop", nullptr};
        mpv_command(m_mpv, stopCmd);
        const char *clearCmd[] = {"playlist-clear", nullptr};
        mpv_command(m_mpv, clearCmd);
    }
    m_playing    = false;
    m_paused     = false;
    m_realtimeBr = 0;
    emit playbackStopped();
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

void Player::setVolume(int vol)
{
    m_volume = qBound(0, vol, 100);
    if (m_mpv) {
        double v = m_volume;
        mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &v);
    }
}

void Player::setMode(const QString &mode, bool hp1, bool hp2, const QString &soundField)
{
    m_mode       = mode;
    m_hp1        = hp1;
    m_hp2        = hp2;
    m_soundField = soundField;
    if (m_playing) {
        applyAudioChain();
    }
}

void Player::setAudioDevice(const QString &deviceId)
{
    if (deviceId.isEmpty())
        mpv_set_option_string(m_mpv, "audio-device", "auto");
    else
        mpv_set_option_string(m_mpv, "audio-device", deviceId.toUtf8().constData());

    bool wasPlaying = m_playing;
    int idx = m_currentIndex;
    if (wasPlaying) { stop(); play(idx); }
}

double Player::getPosition() const
{
    if (!m_mpv || !m_playing) return 0.0;
    double pos = 0.0;
    mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    return pos;
}

double Player::getDuration() const
{
    if (!m_mpv) return 0.0;
    double dur = 0.0;
    mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
    return dur;
}

QString Player::getTagTitle() const
{
    if (m_currentIndex >= m_playlist.size()) return {};
    QString file = m_playlist[m_currentIndex];
    QString ext  = QFileInfo(file).suffix().toLower();
    if (ext == "flac") {
        TagLib::FLAC::File tf(file.toStdWString().c_str());
        if (tf.isValid() && tf.tag()) {
            QString t = QString::fromStdString(tf.tag()->title().to8Bit(true));
            if (!t.isEmpty()) return t;
        }
    } else if (ext == "mp3") {
        TagLib::MPEG::File tf(file.toStdWString().c_str());
        if (tf.isValid() && tf.tag()) {
            QString t = QString::fromStdString(tf.tag()->title().to8Bit(true));
            if (!t.isEmpty()) return t;
        }
    } else if (ext == "m4a" || ext == "mp4" || ext == "aac") {
        TagLib::MP4::File tf(file.toStdWString().c_str());
        if (tf.isValid() && tf.tag()) {
            QString t = QString::fromStdString(tf.tag()->title().to8Bit(true));
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
            return QString::fromStdString(tf.tag()->artist().to8Bit(true));
    } else if (ext == "mp3") {
        TagLib::MPEG::File tf(file.toStdWString().c_str());
        if (tf.isValid() && tf.tag())
            return QString::fromStdString(tf.tag()->artist().to8Bit(true));
    } else if (ext == "m4a" || ext == "mp4" || ext == "aac") {
        TagLib::MP4::File tf(file.toStdWString().c_str());
        if (tf.isValid() && tf.tag())
            return QString::fromStdString(tf.tag()->artist().to8Bit(true));
    }
    return {};
}

QString Player::getInfo(const QString &mode) const
{
    if (!m_mpv) return "";

    // コーデック名：再生中はmpvから、再生前は拡張子から
    QString info;
    if (m_playing) {
        char *codec = nullptr;
        mpv_get_property(m_mpv, "audio-codec-name", MPV_FORMAT_STRING, &codec);
        if (codec) { info = QString(codec).toUpper(); mpv_free(codec); }
    } else {
        if (m_currentIndex < m_playlist.size())
            info = QFileInfo(m_playlist[m_currentIndex]).suffix().toUpper();
    }

    // kbps：demuxer-cache-stateのpacket-bitrateを優先（FLAC等のリアルタイム変動に対応）
    int br = 0;
    if (m_playing) {
        if (m_realtimeBr > 0) {
            br = m_realtimeBr;
        } else {
            double mpvBr = 0.0;
            mpv_get_property(m_mpv, "audio-bitrate", MPV_FORMAT_DOUBLE, &mpvBr);
            br = (mpvBr > 0) ? static_cast<int>(mpvBr / 1000.0) : m_cachedBr;
        }
    }

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
    if (outKhz > 0)
        info += QString(" | %1 kHz").arg(outKhz, 0, 'f', 1);
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

void Player::processAudioRawData(void *nodePtr)
{
    // af-metadataからastatsのRMSレベルを取得してm_levelLeft/Rightに格納
    mpv_node *node = static_cast<mpv_node*>(nodePtr);
    if (!node || node->format != MPV_FORMAT_NODE_MAP) return;

    float l = 0.f, r = 0.f;
    for (int i = 0; i < node->u.list->num; i++) {
        if (node->u.list->values[i].format != MPV_FORMAT_STRING) continue;
        QString key = QString::fromUtf8(node->u.list->keys[i]);
        QString val = QString::fromUtf8(node->u.list->values[i].u.string);
        if (key.contains("RMS_level")) {
            float db = val.toFloat();
            if (db > -200.f) {
                float linear = qBound(0.f, std::pow(10.f, db / 20.f), 1.f);
                if (key.contains(".1.") || key.contains("_L"))
                    l = linear;
                else if (key.contains(".2.") || key.contains("_R"))
                    r = linear;
                else if (l == 0.f)
                    l = r = linear;
            }
        }
    }
    if (l > 0.f && r == 0.f) r = l;
    if (r > 0.f && l == 0.f) l = r;

    QMutexLocker lock(&m_levelMutex);
    m_levelLeft  = l;
    m_levelRight = r;
}

void Player::getAudioLevels(float &left, float &right) const
{
    QMutexLocker lock(&m_levelMutex);
    left  = m_levelLeft;
    right = m_levelRight;
}
