#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QMutex>
#include <mpv/client.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/wavfile.h>

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
    void setAudioDevice(const QString &deviceId);
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
    QStringList collectFiles(const QString &folder, int depth = 0);
    void mpvEventLoop();

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
    QString      m_soundField;
    QMutex       m_mutex;
    RepeatMode   m_repeatMode  = RepeatMode::None;
    ShuffleMode  m_shuffleMode = ShuffleMode::None;
    QList<int>   m_shuffleList;
    int          m_shufflePos  = 0;
    QStringList  m_favPaths;

    static const QStringList SUPPORTED_EXT;

    // VUメーター用音量レベル
    void processAudioRawData(void *node);
    mutable QMutex m_levelMutex;
    float m_levelLeft  = 0.f;
    float m_levelRight = 0.f;

    // infoラベル用キャッシュ（play()時に更新）
    int m_cachedBr   = 0;
    int m_realtimeBr = 0;   // demuxer-cache-stateから取得するリアルタイムkbps
    int m_cachedSr   = 0;
    int m_cachedBits = 0;
};
