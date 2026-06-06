#pragma once
#include <QWidget>
#include <QTimer>
#include <QPushButton>
#include <QVBoxLayout>

class CdRipProgressWidget : public QWidget
{
    Q_OBJECT
public:
    explicit CdRipProgressWidget(QWidget *parent = nullptr);

    void startRipping(int totalTracks, int estimatedTotalSec);
    void updateTrack(int trackNo, qint64 written, qint64 total);
    void stopRipping();

signals:
    void cancelRequested();  // キャンセルボタン押下

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private slots:
    void onTick();

private:
    QString fmt(int sec) const;
    void updateCancelBtnPos();

    int          m_totalTracks  = 1;
    int          m_currentTrack = 1;
    int          m_estimatedSec = 300;
    int          m_elapsedSec   = 0;
    double       m_totalRatio   = 0.0;
    bool         m_running      = false;
    QTimer      *m_timer        = nullptr;
    QPushButton *m_cancelBtn    = nullptr;

    static constexpr int BLOCKS = 30;
};
