#include "CDPlaybackController.h"
#include "CDToWavWriter.h"
#include "CDReader.h"
#include "RingBuffer.h"
#include <QThread>
#include <QDir>
#include <QDateTime>
#include <QDebug>

// ─────────────────────────────────────────────
// CDRipWorker：2曲ずつ結合してリッピング
// 曲間の途切れを防ぐため、2曲分を1つのWAVに書き出す
// ─────────────────────────────────────────────
class CDRipWorker : public QThread {
    Q_OBJECT
public:
    CDRipWorker(const QString &drive, const DiscInfo &disc, int startFrom = 0, QObject *parent = nullptr)
        : QThread(parent), m_drive(drive), m_discInfo(disc), m_startFrom(startFrom) {}

    void stopRipping() { m_stop = true; }

signals:
    void progress(int trackNo, qint64 written, qint64 total);
    void trackDone(int trackIndex, const QString &path);  // 2曲ペアの先頭インデックス
    void finished(const QStringList &paths);
    void error(const QString &msg);

protected:
    void run() override
    {
        QStringList paths;
        int total = m_discInfo.tracks.size();

        // 最高速度を強制設定
        {
            CdDrive tmp;
            if (tmp.open(m_drive)) {
                tmp.setReadSpeed(0xFFFF);
                tmp.close();
                qDebug() << "[CDRipWorker] setReadSpeed: MAX (0xFFFF)";
            }
        }

        // ★ 1曲ずつ処理（m_startFromから開始）
        for (int i = m_startFrom; i < total; i++) {
            if (m_stop) break;

            quint32 startSector = m_discInfo.tracks[i].startSector;
            quint32 endSector   = m_discInfo.tracks[i].endSector;
            qint64 expectedBytes = static_cast<qint64>(endSector - startSector + 1) * 2352;

            QString wavPath = QDir::tempPath() +
                QString("/AlwaysCD_%1_%2.wav")
                .arg(i + 1)
                .arg(QDateTime::currentMSecsSinceEpoch());

            qDebug() << "[CDRipWorker] Track" << (i+1)
                     << "sectors" << startSector << "-" << endSector
                     << "->" << wavPath;

            constexpr size_t BUF = 44100 * 2 * 2 * 8;
            RingBuffer    buf(BUF);
            CDReader      reader(&buf);
            CDToWavWriter writer(&buf, wavPath, expectedBytes);

            reader.startTrack(m_drive, startSector, endSector);
            writer.start();

            qint64 lastWritten = 0;
            while (!m_stop) {
                msleep(200);
                qint64 written = writer.totalPcmBytes();
                if (written != lastWritten) {
                    lastWritten = written;
                    emit progress(i + 1, written, expectedBytes);
                }
                if (!reader.isRunning() && written >= expectedBytes - 2352) break;
                if (!reader.isRunning() && written == lastWritten)          break;
            }

            writer.stopWriting();
            writer.wait(10000);
            reader.wait(3000);
            emit progress(i + 1, expectedBytes, expectedBytes);

            if (m_stop) break;

            paths << wavPath;
            emit trackDone(i - m_startFrom, wavPath);  // 相対インデックス
            qDebug() << "[CDRipWorker] Track" << (i+1) << "done";
        }

        if (!m_stop)
            emit finished(paths);
    }

private:
    QString   m_drive;
    DiscInfo  m_discInfo;
    int       m_startFrom = 0;
    bool      m_stop = false;
};

#include "CDPlaybackController.moc"

// ─────────────────────────────────────────────
// CDPlaybackController
// ─────────────────────────────────────────────
CDPlaybackController::CDPlaybackController(QObject *parent)
    : QObject(parent)
{}

CDPlaybackController::~CDPlaybackController()
{
    stop();
}

bool CDPlaybackController::init(const QString &drive)
{
    CdDrive tmp;
    if (!tmp.open(drive)) return false;
    m_discInfo = tmp.readToc();
    tmp.close();
    m_drive = drive;
    return !m_discInfo.tracks.isEmpty();
}

void CDPlaybackController::startRipping()
{
    stop();

    m_worker = new CDRipWorker(m_drive, m_discInfo, 0, this);

    connect(m_worker, &CDRipWorker::progress,
            this, &CDPlaybackController::onWorkerProgress);
    connect(m_worker, &CDRipWorker::trackDone,
            this, &CDPlaybackController::onWorkerTrackDone);
    connect(m_worker, &CDRipWorker::finished,
            this, &CDPlaybackController::onWorkerFinished);
    connect(m_worker, &CDRipWorker::error,
            this, &CDPlaybackController::onWorkerError);
    connect(m_worker, &CDRipWorker::finished,
            m_worker, &QObject::deleteLater);

    m_worker->start();
}

void CDPlaybackController::startRippingFrom(int trackIndex)
{
    stop();

    // trackIndexを2曲ペアの先頭に揃える（偶数にする）
    int startFrom = (trackIndex / 2) * 2;
    qDebug() << "[CDPlaybackController] startRippingFrom track=" << trackIndex
             << "pair start=" << startFrom;

    m_worker = new CDRipWorker(m_drive, m_discInfo, startFrom, this);

    connect(m_worker, &CDRipWorker::progress,
            this, &CDPlaybackController::onWorkerProgress);
    connect(m_worker, &CDRipWorker::trackDone,
            this, &CDPlaybackController::onWorkerTrackDone);
    connect(m_worker, &CDRipWorker::finished,
            this, &CDPlaybackController::onWorkerFinished);
    connect(m_worker, &CDRipWorker::error,
            this, &CDPlaybackController::onWorkerError);
    connect(m_worker, &CDRipWorker::finished,
            m_worker, &QObject::deleteLater);

    m_worker->start();
}

void CDPlaybackController::stop()
{
    if (m_worker) {
        m_worker->stopRipping();
        m_worker->wait(5000);
        m_worker = nullptr;
    }
}

void CDPlaybackController::onWorkerProgress(int trackNo, qint64 written, qint64 total)
{
    emit ripProgress(trackNo, written, total);
}

void CDPlaybackController::onWorkerTrackDone(int trackIndex, const QString &path)
{
    emit trackRipped(trackIndex, path);
}

void CDPlaybackController::onWorkerFinished(const QStringList &paths)
{
    qDebug() << "[CDPlaybackController] All done:" << paths.size() << "files";
    emit allTracksRipped(paths);
}

void CDPlaybackController::onWorkerError(const QString &msg)
{
    emit ripError(msg);
}
