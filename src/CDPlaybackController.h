#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>
#include "CdDrive.h"

// ─────────────────────────────────────────────
// CDPlaybackController（シンプル全曲リッピング方式）
//
// 1. startRipping(drive) → 全トラックを順番にWAVへリッピング
// 2. 各トラック完了ごとに trackRipped(index, path) シグナル
// 3. 全完了で allTracksRipped(paths) シグナル
// 4. MainWindowはpathsをloadFolder相当で読み込んで通常再生
// ─────────────────────────────────────────────
class CDRipWorker;

class CDPlaybackController : public QObject {
    Q_OBJECT
public:
    explicit CDPlaybackController(QObject *parent = nullptr);
    ~CDPlaybackController();

    bool init(const QString &drive);
    const DiscInfo &discInfo() const { return m_discInfo; }

    void startRipping();
    void startRippingFrom(int trackIndex);
    void stop();

    int totalTracks() const { return m_discInfo.tracks.size(); }

signals:
    void trackRipped(int trackIndex, const QString &wavPath);  // 1曲完了
    void ripProgress(int trackNo, qint64 written, qint64 total); // 進捗
    void allTracksRipped(const QStringList &wavPaths);          // 全曲完了
    void ripError(const QString &msg);

private slots:
    void onWorkerProgress(int trackNo, qint64 written, qint64 total);
    void onWorkerTrackDone(int trackIndex, const QString &path);
    void onWorkerFinished(const QStringList &paths);
    void onWorkerError(const QString &msg);

private:
    DiscInfo     m_discInfo;
    QString      m_drive;
    CDRipWorker *m_worker = nullptr;
};
