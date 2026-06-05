#include "CdRipProgressWidget.h"
#include <QPainter>

CdRipProgressWidget::CdRipProgressWidget(QWidget *parent)
    : QWidget(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &CdRipProgressWidget::onTick);
    setAttribute(Qt::WA_TranslucentBackground);

    // ★ キャンセルボタン
    m_cancelBtn = new QPushButton("Cancel", this);
    m_cancelBtn->setFixedSize(100, 30);
    m_cancelBtn->setStyleSheet(
        "QPushButton { background: #1a2a3a; color: #3a8fe8; "
        "border: 1px solid #3a8fe8; border-radius: 4px; font-size: 11px; }"
        "QPushButton:hover { background: #2a3a4a; }"
    );
    m_cancelBtn->hide();
    connect(m_cancelBtn, &QPushButton::clicked, this, &CdRipProgressWidget::cancelRequested);
}

void CdRipProgressWidget::startRipping(int totalTracks, int estimatedTotalSec)
{
    m_totalTracks  = totalTracks;
    m_currentTrack = 1;
    m_estimatedSec = qMax(10, estimatedTotalSec);
    m_elapsedSec   = 0;
    m_totalRatio   = 0.0;
    m_running      = true;
    m_timer->start();
    m_cancelBtn->show();
    updateCancelBtnPos();
    update();
}

void CdRipProgressWidget::updateTrack(int trackNo, qint64 written, qint64 total)
{
    m_currentTrack = trackNo;
    // 全体進捗 = (完了トラック + 現在トラック進捗) / 総トラック数
    double trackRatio = (total > 0) ? static_cast<double>(written) / total : 0.0;
    m_totalRatio = qMin(1.0, (m_currentTrack - 1 + trackRatio) / m_totalTracks);
    update();
}

void CdRipProgressWidget::stopRipping()
{
    m_running = false;
    m_timer->stop();
    m_cancelBtn->hide();
    update();
}

void CdRipProgressWidget::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    updateCancelBtnPos();
}

void CdRipProgressWidget::updateCancelBtnPos()
{
    // ボタンを中央下部に配置
    int x = (width()  - m_cancelBtn->width())  / 2;
    int y =  height() - m_cancelBtn->height() - 20;
    m_cancelBtn->move(x, y);
}

void CdRipProgressWidget::onTick()
{
    if (!m_running) return;
    m_elapsedSec++;
    // updateTrackからデータが来ていなければ時間ベースで進める
    if (m_estimatedSec > 0) {
        double timeBased = qMin(0.99, static_cast<double>(m_elapsedSec) / m_estimatedSec);
        if (timeBased > m_totalRatio)
            m_totalRatio = timeBased;
    }
    update();
}

QString CdRipProgressWidget::fmt(int sec) const
{
    return QString("%1:%2").arg(sec / 60).arg(sec % 60, 2, 10, QChar('0'));
}

void CdRipProgressWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(6, 6, 6));
    if (!m_running) return;

    const QColor blue(0x3a, 0x8f, 0xe8);
    const QColor gray(0x55, 0x75, 0x90);
    const QColor dim (0x30, 0x45, 0x58);

    int w = width();
    int cy = height() / 2 - 50;

    auto drawC = [&](const QString &text, int fontSize, const QColor &col, int y) -> int {
        QFont f("Consolas", fontSize);
        p.setFont(f);
        p.setPen(col);
        QFontMetrics fm(f);
        p.drawText((w - fm.horizontalAdvance(text)) / 2, y, text);
        return y + fm.height() + 4;
    };

    // ── Please Wait
    cy = drawC("Please Wait", 13, blue, cy);
    cy += 10;

    // ── ブロックメーター |■■■□□□□□□□□|
    int filled = static_cast<int>(m_totalRatio * BLOCKS);
    QString meter = "|";
    for (int i = 0; i < BLOCKS; i++)
        meter += (i < filled) ? QChar(0x25A0) : QChar(0x25A1);  // ■ □
    meter += "|";
    cy = drawC(meter, 11, blue, cy);
    cy += 8;

    // ── Elapsed / Remaining
    int remaining = qMax(0, m_estimatedSec - m_elapsedSec);
    drawC(QString("Elapsed: %1   Remaining: %2").arg(fmt(m_elapsedSec)).arg(fmt(remaining)),
          8, dim, cy);
}
