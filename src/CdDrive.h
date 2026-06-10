#pragma once
#include <QString>
#include <QVector>
#include <QFile>
#include <windows.h>

struct TrackInfo {
    int     number;
    QString title;
    DWORD   startSector;
    DWORD   endSector;
    int     durationSec;
    QString binFile;
    bool    isWave = false;
};

struct DiscInfo {
    QString            discId;
    QString            albumTitle;
    QString            artist;
    QString            date;
    QVector<TrackInfo> tracks;
    DWORD              totalSectors  = 0;
    bool               isCueMode    = false;
    qint64             wavHeaderSize = 0;
};

enum class DriveMode {
    Physical,   // 物理CDドライブ
    CueBin      // bin/cueファイル
};

class CdDrive
{
public:
    CdDrive();
    ~CdDrive();

    // ── 物理CDドライブ ──
    static QStringList availableDrives();
    bool open(const QString &driveLetter);

    // ── bin/cueファイル ──
    bool openCue(const QString &cuePath);

    void    close();
    bool    isOpen() const;
    QString sourceName() const { return m_sourceName; }
    DriveMode mode() const { return m_mode; }

    // TOC読み取り（物理 / cue 両対応）
    DiscInfo readToc();

    // セクタ単位で生PCMを読む（2352バイト/セクタ）
    QByteArray readSectorRaw(DWORD lba);

    int  sampleOffset() const { return m_sampleOffset; }
    void setSampleOffset(int v) { m_sampleOffset = v; }

    // リッピング速度制御（物理CDドライブのみ有効）
    // speedKBps: 300=2x / 600=4x / 1200=8x / 2400=16x
    bool setReadSpeed(int speedKBps);

    // 速度定数（音楽CD基準: 1x = 150 KB/s）
    static constexpr int SPEED_2X  = 300;
    static constexpr int SPEED_4X  = 600;
    static constexpr int SPEED_8X  = 1200;
    static constexpr int SPEED_16X = 2400;

    // cue解析（publicで使用可能）
    DiscInfo parseCue(const QString &cuePath);
    DiscInfo parseCueFile(const QString &cuePath) { return parseCue(cuePath); }

    // 直前のセクタ読み取りでのリトライ回数
    int lastRetryCount()    const { return m_lastRetryCount; }
    int lastC2Errors()      const { return m_lastC2Errors; }
    int lastReadSpeedKBps() const { return m_lastReadSpeedKBps; }

private:
    // 物理ドライブ
    HANDLE    m_handle       = INVALID_HANDLE_VALUE;

    // bin/cueモード
    DiscInfo  m_cueDisc;
    QFile     m_binFile;

    // 共通
    DriveMode m_mode         = DriveMode::Physical;
    QString   m_sourceName;
    int       m_sampleOffset = 0;

    QByteArray readSectorFromBin(DWORD lba);
    QByteArray readSectorFromDrive(DWORD lba);

    int m_lastRetryCount    = 0;
    int m_lastC2Errors      = 0;
    int m_lastReadSpeedKBps = 0;
};

