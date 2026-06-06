#include "CdDrive.h"
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <ntddcdrm.h>
#include <ntddscsi.h>   // SCSI_PASS_THROUGH_DIRECT

static constexpr int CD_RAW_SECTOR_SIZE = 2352;

CdDrive::CdDrive() = default;

CdDrive::~CdDrive()
{
    close();
}

bool CdDrive::isOpen() const
{
    if (m_mode == DriveMode::Physical)
        return m_handle != INVALID_HANDLE_VALUE;
    else
        return m_binFile.isOpen();
}

// ────────────────────────────────────────────
// 物理CDドライブ
// ────────────────────────────────────────────

QStringList CdDrive::availableDrives()
{
    QStringList result;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1 << i))) continue;
        QString letter = QString("%1:").arg(QChar('A' + i));
        if (GetDriveTypeW(reinterpret_cast<LPCWSTR>((letter + "\\").utf16()))
                == DRIVE_CDROM) {
            result << letter;
        }
    }
    return result;
}

bool CdDrive::open(const QString &driveLetter)
{
    close();
    m_mode = DriveMode::Physical;
    QString path = "\\\\.\\" + driveLetter;
    m_handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) {
        qWarning() << "CdDrive: cannot open" << driveLetter;
        return false;
    }
    m_sourceName = driveLetter;
    return true;
}

// ────────────────────────────────────────────
// bin/cue ファイル
// ────────────────────────────────────────────

bool CdDrive::openCue(const QString &cuePath)
{
    close();
    m_mode = DriveMode::CueBin;

    m_cueDisc = parseCue(cuePath);
    if (m_cueDisc.tracks.isEmpty()) {
        qWarning() << "CdDrive: cue parse failed:" << cuePath;
        return false;
    }

    // 最初のトラックのbinファイルを開く
    QString binPath = m_cueDisc.tracks.first().binFile;
    m_binFile.setFileName(binPath);
    if (!m_binFile.open(QIODevice::ReadOnly)) {
        qWarning() << "CdDrive: cannot open bin:" << binPath;
        return false;
    }

    m_sourceName = QFileInfo(cuePath).fileName();
    return true;
}

void CdDrive::close()
{
    if (m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    if (m_binFile.isOpen())
        m_binFile.close();
    m_cueDisc = DiscInfo();
}

// ────────────────────────────────────────────
// cueファイル解析
// ────────────────────────────────────────────

DiscInfo CdDrive::parseCue(const QString &cuePath)
{
    DiscInfo disc;
    disc.isCueMode = true;

    QFile f(cuePath);
    if (!f.open(QIODevice::ReadOnly)) return disc;

    // UTF-8 / Shift-JIS 両対応で読み込む
    QByteArray raw = f.readAll();
    f.close();

    QString content;
    // UTF-8 BOM チェック
    if (raw.startsWith("\xEF\xBB\xBF"))
        content = QString::fromUtf8(raw.mid(3));
    else if (raw.startsWith("\xFF\xFE"))
        content = QString::fromUtf16(reinterpret_cast<const char16_t*>(raw.mid(2).constData()));
    else {
        content = QString::fromUtf8(raw);
        if (content.contains(QChar(0xFFFD)))
            content = QString::fromLocal8Bit(raw);
    }

    QStringList lines = content.split('\n');
    QFileInfo cueInfo(cuePath);
    QString currentBin;
    bool isWave = false;

    struct RawTrack {
        int     number;
        QString binFile;
        bool    isWave;
        DWORD   indexSector;
        QString title;
    };
    QVector<RawTrack> rawTracks;

    auto msfToLba = [](int m, int s, int fr) -> DWORD {
        return (DWORD)(m * 60 + s) * 75 + fr;
    };

    auto unquote = [](const QString &line) -> QString {
        int q1 = line.indexOf('"');
        int q2 = line.lastIndexOf('"');
        if (q1 >= 0 && q2 > q1) return line.mid(q1+1, q2-q1-1);
        return QString();
    };

    for (QString line : lines) {
        line = line.trimmed();

        if (line.startsWith("PERFORMER", Qt::CaseInsensitive))
            disc.artist = unquote(line);
        else if (line.startsWith("TITLE", Qt::CaseInsensitive) && rawTracks.isEmpty())
            disc.albumTitle = unquote(line);
        else if (line.startsWith("REM DATE", Qt::CaseInsensitive))
            disc.date = line.split(' ').last().trimmed().replace("\"","");
        else if (line.startsWith("FILE", Qt::CaseInsensitive)) {
            QString name = unquote(line);
            currentBin = QDir(cueInfo.absolutePath()).filePath(name);
            isWave = line.toUpper().contains("WAVE");
        }
        else if (line.startsWith("TRACK", Qt::CaseInsensitive)) {
            RawTrack rt;
            QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            rt.number      = parts.size() >= 2 ? parts[1].toInt() : 0;
            rt.binFile     = currentBin;
            rt.isWave      = isWave;
            rt.indexSector = 0;
            rawTracks.append(rt);
        }
        else if (line.startsWith("TITLE", Qt::CaseInsensitive) && !rawTracks.isEmpty())
            rawTracks.last().title = unquote(line);
        else if (line.startsWith("INDEX 01", Qt::CaseInsensitive) && !rawTracks.isEmpty()) {
            QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() >= 3) {
                QStringList msf = parts[2].split(':');
                if (msf.size() == 3)
                    rawTracks.last().indexSector =
                        msfToLba(msf[0].toInt(), msf[1].toInt(), msf[2].toInt());
            }
        }
    }

    // WAVヘッダサイズを取得
    qint64 wavHeaderSize = 0;
    if (!rawTracks.isEmpty() && rawTracks.first().isWave) {
        QFile wf(rawTracks.first().binFile);
        if (wf.open(QIODevice::ReadOnly)) {
            // WAVヘッダを解析してdataチャンクを探す
            QByteArray header = wf.read(12);
            if (header.startsWith("RIFF") && header.mid(8,4) == "WAVE") {
                while (!wf.atEnd()) {
                    QByteArray chunkId = wf.read(4);
                    QByteArray szBuf   = wf.read(4);
                    if (szBuf.size() < 4) break;
                    quint32 chunkSize = *reinterpret_cast<const quint32*>(szBuf.constData());
                    if (chunkId == "data") {
                        wavHeaderSize = wf.pos();     // dataチャンクの開始位置
                        break;
                    }
                    wf.seek(wf.pos() + chunkSize);
                }
            }
            wf.close();
        }
        disc.wavHeaderSize = wavHeaderSize;
    }

    // TrackInfo に変換
    for (int i = 0; i < rawTracks.size(); ++i) {
        TrackInfo ti;
        ti.number      = rawTracks[i].number;
        ti.startSector = rawTracks[i].indexSector;
        ti.binFile     = rawTracks[i].binFile;
        ti.isWave      = rawTracks[i].isWave;
        ti.title       = rawTracks[i].title;

        if (i + 1 < rawTracks.size()) {
            ti.endSector = rawTracks[i+1].indexSector - 1;
        } else {
            QFile bin(ti.binFile);
            if (bin.open(QIODevice::ReadOnly)) {
                qint64 dataBytes = bin.size() - wavHeaderSize;
                ti.endSector = ti.startSector +
                    (DWORD)(dataBytes / CD_RAW_SECTOR_SIZE) - 1;
                bin.close();
            }
        }
        ti.durationSec = (int)(ti.endSector - ti.startSector + 1) / 75;
        disc.tracks.append(ti);
    }

    if (!disc.tracks.isEmpty())
        disc.totalSectors = disc.tracks.last().endSector + 1;

    return disc;
}



// ────────────────────────────────────────────
// リッピング速度制御
// ────────────────────────────────────────────

bool CdDrive::setReadSpeed(int speedKBps)
{
    if (m_handle == INVALID_HANDLE_VALUE) return false;

    CDROM_SET_SPEED req = {};
    req.RequestType     = CdromSetSpeed;
    req.ReadSpeed       = (USHORT)speedKBps;  // KB/s単位
    req.WriteSpeed      = 0;
    req.RotationControl = CdromDefaultRotation;

    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(
        m_handle, IOCTL_CDROM_SET_SPEED,
        &req, sizeof(req),
        nullptr, 0, &bytes, nullptr);

    qDebug() << "[CdDrive] setReadSpeed:" << speedKBps << "KB/s =>"
             << (ok ? "OK" : "FAILED") << "(err=" << GetLastError() << ")";
    return ok == TRUE;
}

// ────────────────────────────────────────────
// TOC読み取り
// ────────────────────────────────────────────

DiscInfo CdDrive::readToc()
{
    if (m_mode == DriveMode::CueBin)
        return m_cueDisc;

    DiscInfo info;
    if (m_handle == INVALID_HANDLE_VALUE) return info;

    CDROM_TOC toc = {};
    DWORD bytesReturned = 0;

    BOOL ok = DeviceIoControl(
        m_handle, IOCTL_CDROM_READ_TOC,
        nullptr, 0, &toc, sizeof(toc),
        &bytesReturned, nullptr);

    if (!ok) {
        qWarning() << "CdDrive: IOCTL_CDROM_READ_TOC failed:" << GetLastError();
        return info;
    }

    int firstTrack = toc.FirstTrack;
    int lastTrack  = toc.LastTrack;

    for (int t = firstTrack; t <= lastTrack; ++t) {
        int idx = t - firstTrack;
        TRACK_DATA &td   = toc.TrackData[idx];
        TRACK_DATA &next = toc.TrackData[idx + 1];

        DWORD startLba = ((DWORD)td.Address[1]*60   + td.Address[2])*75   + td.Address[3]   - 150;
        DWORD endLba   = ((DWORD)next.Address[1]*60 + next.Address[2])*75 + next.Address[3] - 150 - 1;

        TrackInfo ti;
        ti.number      = t;
        ti.startSector = startLba;
        ti.endSector   = endLba;
        ti.durationSec = (int)(endLba - startLba + 1) / 75;
        info.tracks.append(ti);
    }

    TRACK_DATA &leadout = toc.TrackData[lastTrack - firstTrack + 1];
    info.totalSectors = ((DWORD)leadout.Address[1]*60 + leadout.Address[2])*75
                        + leadout.Address[3] - 150;
    return info;
}

// ────────────────────────────────────────────
// セクタ読み取り
// ────────────────────────────────────────────

QByteArray CdDrive::readSectorRaw(DWORD lba)
{
    if (m_mode == DriveMode::CueBin)
        return readSectorFromBin(lba);
    else
        return readSectorFromDrive(lba);
}

QByteArray CdDrive::readSectorFromBin(DWORD lba)
{
    if (!m_binFile.isOpen()) return {};

    // WAVEの場合はヘッダ分オフセット
    qint64 headerOffset = m_cueDisc.wavHeaderSize;
    qint64 offset = headerOffset + (qint64)lba * CD_RAW_SECTOR_SIZE;
    if (!m_binFile.seek(offset)) return {};

    QByteArray buf = m_binFile.read(CD_RAW_SECTOR_SIZE);
    if (buf.size() != CD_RAW_SECTOR_SIZE) return {};
    return buf;
}

QByteArray CdDrive::readSectorFromDrive(DWORD lba)
{
    // ── 複数セクタまとめ読み方式 ────────────────────────────────
    // IOCTL_CDROM_RAW_READ は1セクタずつ読むとドライブが同じ位置を
    // 繰り返し読むループが起きる場合がある。
    // fre:ac / CDparanoia と同様に複数セクタをまとめて読み、
    // 目的のセクタだけを切り出す方式に変更。
    static constexpr int READ_SECTORS   = 27;  // 1回で読むセクタ数
    static constexpr int PHASE1_RETRIES =  5;  // 高速リトライ
    static constexpr int PHASE2_RETRIES = 20;  // キャッシュフラッシュ＋短待機
    static constexpr int PHASE3_RETRIES = 10;  // 長待機（ドライブを休ませる）

    m_lastRetryCount    = 0;
    m_lastC2Errors      = 0;
    m_lastReadSpeedKBps = 0;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    // 読み取り開始セクタ（目的LBAの少し手前から読む）
    DWORD startLba = (lba >= (DWORD)(READ_SECTORS / 2))
                     ? lba - READ_SECTORS / 2 : 0;
    int   offset   = (int)(lba - startLba) * CD_RAW_SECTOR_SIZE;

    QByteArray multiBuf(CD_RAW_SECTOR_SIZE * READ_SECTORS, '\0');

    auto tryReadMulti = [&](DWORD sLba) -> bool {
        RAW_READ_INFO rri = {};
        rri.DiskOffset.QuadPart = (LONGLONG)sLba * 2048;
        rri.SectorCount = READ_SECTORS;
        rri.TrackMode   = CDDA;
        DWORD bytesReturned = 0;
        BOOL ok = DeviceIoControl(
            m_handle, IOCTL_CDROM_RAW_READ,
            &rri, sizeof(rri),
            multiBuf.data(), CD_RAW_SECTOR_SIZE * READ_SECTORS,
            &bytesReturned, nullptr);
        return ok && bytesReturned == (DWORD)(CD_RAW_SECTOR_SIZE * READ_SECTORS);
    };

    // ── フェーズ1: 待機なし高速リトライ ─────────────────────────
    for (int i = 0; i < PHASE1_RETRIES; ++i) {
        if (tryReadMulti(startLba)) {
            QueryPerformanceCounter(&t1);
            double sec = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
            if (sec > 0.0)
                m_lastReadSpeedKBps = (int)(CD_RAW_SECTOR_SIZE / sec / 1024.0);
            m_lastRetryCount = i;
            return multiBuf.mid(offset, CD_RAW_SECTOR_SIZE);
        }
    }

    // ── フェーズ2: キャッシュフラッシュ＋10ms待機 ───────────────
    for (int i = 0; i < PHASE2_RETRIES; ++i) {
        DWORD flushLba = (startLba > 100) ? startLba - 100 : startLba + 100;
        RAW_READ_INFO flush = {};
        flush.DiskOffset.QuadPart = (LONGLONG)flushLba * 2048;
        flush.SectorCount = READ_SECTORS;
        flush.TrackMode   = CDDA;
        QByteArray tmp(CD_RAW_SECTOR_SIZE * READ_SECTORS, '\0');
        DWORD dummy = 0;
        DeviceIoControl(m_handle, IOCTL_CDROM_RAW_READ,
            &flush, sizeof(flush), tmp.data(),
            CD_RAW_SECTOR_SIZE * READ_SECTORS, &dummy, nullptr);
        Sleep(10);
        if (tryReadMulti(startLba)) {
            QueryPerformanceCounter(&t1);
            double sec = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
            if (sec > 0.0)
                m_lastReadSpeedKBps = (int)(CD_RAW_SECTOR_SIZE / sec / 1024.0);
            m_lastRetryCount = PHASE1_RETRIES + i;
            return multiBuf.mid(offset, CD_RAW_SECTOR_SIZE);
        }
    }

    // ── フェーズ3: 100ms休ませてリトライ ────────────────────────
    for (int i = 0; i < PHASE3_RETRIES; ++i) {
        Sleep(100);
        if (tryReadMulti(startLba)) {
            QueryPerformanceCounter(&t1);
            double sec = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
            if (sec > 0.0)
                m_lastReadSpeedKBps = (int)(CD_RAW_SECTOR_SIZE / sec / 1024.0);
            m_lastRetryCount = PHASE1_RETRIES + PHASE2_RETRIES + i;
            return multiBuf.mid(offset, CD_RAW_SECTOR_SIZE);
        }
    }

    // ── 全フェーズ失敗 ───────────────────────────────────────────
    m_lastRetryCount = PHASE1_RETRIES + PHASE2_RETRIES + PHASE3_RETRIES;
    m_lastC2Errors   = 1;
    return {};
}
