#include "CDReader.h"
#include <QDebug>

CDReader::CDReader(RingBuffer *buffer, QObject *parent)
    : QThread(parent)
    , m_buffer(buffer)
{}

CDReader::~CDReader()
{
    stopReading();
    wait();
}

void CDReader::startTrack(const QString &drive, DWORD startSector, DWORD endSector)
{
    stopReading();
    wait();

    m_driveLetter  = drive;
    m_startSector  = startSector;
    m_endSector    = endSector;
    m_currentSector.store(startSector);
    m_seekRequested.store(false);
    m_running.store(true);

    m_buffer->clear();
    start(QThread::HighPriority);
}

void CDReader::requestSeek(DWORD sector)
{
    m_seekSector.store(sector);
    m_seekRequested.store(true);
    m_buffer->clear();
}

void CDReader::stopReading()
{
    m_running.store(false);
}

void CDReader::run()
{
    if (!m_drive.open(m_driveLetter)) {
        emit readError(QString("CDドライブを開けません: %1").arg(m_driveLetter));
        return;
    }

    // 2x静音読み取り（Always CD Ripperと同じ）
    m_drive.setReadSpeed(0xFFFF);  // ★ 最高速度を強制設定（Always CD Ripperのキャッシュを上書き）

    DWORD sector = m_startSector;

    // バッファが満杯なら少し待つ間隔（ms）
    constexpr int WAIT_MS = 5;

    while (m_running.load() && sector <= m_endSector) {

        // シーク要求チェック
        if (m_seekRequested.load()) {
            sector = m_seekSector.load();
            m_seekRequested.store(false);
            qDebug() << "[CDReader] Seek to sector" << sector;
        }

        // バッファに空きがなければ待機
        if (m_buffer->availableToWrite() < 2352) {
            msleep(WAIT_MS);
            continue;
        }

        // セクタ読み取り（CdDriveのreadSectorRawを流用）
        QByteArray raw = m_drive.readSectorRaw(sector);
        if (raw.size() == 2352) {
            // CD-DAセクタはヘッダなし生PCM
            // ただしCDドライブによってはSync/Header/EDCが付く場合があるが
            // CdDrive::readSectorRaw()はPCM部分のみ返す設計
            const uint8_t *pcm = reinterpret_cast<const uint8_t*>(raw.constData());
            while (m_running.load() && !m_seekRequested.load()) {
                size_t written = m_buffer->write(pcm, 2352);
                if (written == 2352) break;
                msleep(WAIT_MS);  // バッファ待ち
            }
        } else {
            qDebug() << "[CDReader] Read error at sector" << sector
                     << "size=" << raw.size();
            // エラー時はゼロで埋めて続行（無音で継続）
            QByteArray zeros(2352, 0);
            m_buffer->write(reinterpret_cast<const uint8_t*>(zeros.constData()), 2352);
        }

        m_currentSector.store(sector);
        sector++;
    }

    m_drive.close();
    emit finished();
    qDebug() << "[CDReader] Done.";
}
