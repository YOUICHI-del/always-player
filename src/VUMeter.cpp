#include "VUMeter.h"
#include "Player.h"
#include <QPainter>
#include <QRandomGenerator>
#include <cmath>

VUMeter::VUMeter(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(300, 160);
    setAttribute(Qt::WA_OpaquePaintEvent);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &VUMeter::tick);
    m_timer->start(33); // ~30fps
}

VUMeter::~VUMeter()
{
    m_timer->stop();
}

void VUMeter::setPlayer(Player *p)
{
    m_player = p;
}

void VUMeter::setPlaying(bool playing)
{
    m_playing = playing;
    if (!playing) {
        m_levelL = m_levelR = 0.02;
        m_peakL  = m_peakR  = 0.0;
    }
}

void VUMeter::tick()
{
    m_t += 0.04;

    if (m_playing && m_player) {

        float rawL = 0.f, rawR = 0.f;
        m_player->getAudioLevels(rawL, rawR);   // ★ 生PCM値を取得

        double tL = qBound(0.0, (double)rawL, 1.0);
        double tR = qBound(0.0, (double)rawR, 1.0);

        // スムージング（速い上昇・遅い下降）
        if (tL > m_levelL) m_levelL = m_levelL * 0.5 + tL * 0.5;
        else               m_levelL = m_levelL * 0.85 + tL * 0.15;

        if (tR > m_levelR) m_levelR = m_levelR * 0.5 + tR * 0.5;
        else               m_levelR = m_levelR * 0.85 + tR * 0.15;

        // ピークホールド
        if (m_levelL > m_peakL) m_peakL = m_levelL;
        else                    m_peakL *= 0.995;

        if (m_levelR > m_peakR) m_peakR = m_levelR;
        else                    m_peakR *= 0.995;

    } else if (m_playing) {
        // Player がまだセットされていない場合の簡易アニメーション
        double base = 0.45 + std::sin(m_t * 1.3) * 0.22 + std::sin(m_t * 2.7) * 0.1;
        double rndL = (QRandomGenerator::global()->generateDouble() - 0.5) * 0.08;
        double rndR = (QRandomGenerator::global()->generateDouble() - 0.5) * 0.08;
        double tL = qBound(0.05, base + rndL, 1.0);
        double tR = qBound(0.05, base + rndR, 1.0);
        m_velL += (tL - m_levelL) * 0.35; m_velL *= 0.72;
        m_velR += (tR - m_levelR) * 0.35; m_velR *= 0.72;
        m_levelL = qBound(0.0, m_levelL + m_velL, 1.0);
        m_levelR = qBound(0.0, m_levelR + m_velR, 1.0);
        m_peakL = qMax(m_peakL * 0.995, m_levelL);
        m_peakR = qMax(m_peakR * 0.995, m_levelR);
    } else {
        m_levelL *= 0.88;
        m_levelR *= 0.88;
        m_peakL  *= 0.97;
        m_peakR  *= 0.97;
    }

    update();
}

// ── 1チャンネル描画
static void drawChannel(QPainter &p, double cx, double cy, double R,
                        double level, double peak, const char *label)
{
    const int    STICKS = 32;
    const double startA = -M_PI * 0.82;
    const double endA   = -M_PI * 0.18;
    const double sweep  = endA - startA;

    for (int i = 0; i < STICKS; i++) {
        double t     = (double)i / (STICKS - 1);
        double angle = startA + sweep * t;
        double cosA  = std::cos(angle);
        double sinA  = std::sin(angle);
        double inner = R * 0.52;
        double outer = R * 0.86;
        double x1 = cx + cosA * inner;
        double y1 = cy + sinA * inner;
        double x2 = cx + cosA * outer;
        double y2 = cy + sinA * outer;
        double db   = -20.0 + t * 23.0;
        bool active = t <= level;
        bool isPeak = std::abs(t - peak) < 0.018;

        if (isPeak && peak > 0.05) {
            QColor pc = peak >= 0.87 ? QColor(255,80,80,220) : QColor(120,200,255,220);
            p.setPen(QPen(pc, i%4==0 ? 3.0 : 2.0, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(x1,y1), QPointF(x2,y2));
            continue;
        }

        QColor color;
        if (!active) {
            color = QColor(10,20,28);
        } else if (db >= 3) {
            p.setPen(QPen(QColor(255,60,60,60), i%4==0?4.0:2.5, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(x1,y1), QPointF(x2,y2));
            color = QColor(220,55,55,210);
        } else if (db >= 0) {
            p.setPen(QPen(QColor(50,150,255,70), i%4==0?3.5:2.0, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(x1,y1), QPointF(x2,y2));
            color = QColor(40, 160+int(t*60), 255, 235);
        } else {
            p.setPen(QPen(QColor(30,120,255,50), i%4==0?3.0:1.5, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(x1,y1), QPointF(x2,y2));
            color = QColor(20, int(100+t*120), 255, int(100+t*155));
        }

        QPen pen(color, i%4==0 ? 2.0 : 1.1, Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(x1,y1), QPointF(x2,y2));
    }

    // 目盛り
    struct Scale { double t; const char *lb; };
    static const Scale scales[] = {
        {0.00,"-20"},{0.25,"-10"},{0.40,"-7"},{0.52,"-5"},
        {0.63,"-3"},{0.75,"-1"},{0.83,"0"},{0.90,"+1"},{1.00,"+3"},
    };
    p.setFont(QFont("Consolas", 7));
    for (const auto &s : scales) {
        double angle = startA + sweep * s.t;
        double lr = R * 0.38;
        double x = cx + std::cos(angle) * lr;
        double y = cy + std::sin(angle) * lr;
        double db = -20.0 + s.t * 23.0;
        QColor tc = db >= 1 ? QColor(30,6,6) : db >= 0 ? QColor(6,16,36) : QColor(6,14,22);
        p.setPen(tc);
        p.drawText(QRectF(x-12,y-7,24,14), Qt::AlignCenter, s.lb);
    }

    // 針
    double na = startA + sweep * level;
    QPointF tip(cx + std::cos(na)*R*0.80, cy + std::sin(na)*R*0.80);
    p.setPen(QPen(QColor(100,180,255,100), 4.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(cx,cy), tip);
    p.setPen(QPen(QColor(180,225,255,230), 1.2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(cx,cy), tip);

    // 軸
    p.setBrush(QColor(100,180,255,220));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(cx,cy), 3.5, 3.5);

    // チャンネルラベル
    p.setFont(QFont("Consolas", 9, QFont::Bold));
    p.setPen(QColor(14,30,46));
    p.drawText(QRectF(cx-16, cy-R*0.16, 32, 14), Qt::AlignCenter, label);

    // dBラベル
    int dbVal = int(std::round(-20.0 + level * 23.0));
    QString dbStr = (dbVal > 0 ? "+" : "") + QString::number(dbVal) + " dB";
    p.setFont(QFont("Consolas", 8));
    p.setPen(level >= 0.87 ? QColor(180,40,40) :
             level >= 0.83 ? QColor(40,110,180) : QColor(20,60,110));
    p.drawText(QRectF(cx-30, cy+R*0.06, 60, 14), Qt::AlignCenter, dbStr);
}

void VUMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(2, 2, 2));

    // タイトル
    p.setFont(QFont("Meiryo", 8));
    p.setPen(QColor(12,26,38));
    p.drawText(rect().adjusted(0,4,0,0),
               Qt::AlignHCenter | Qt::AlignTop,
               m_player ? "VU METER  [ LIVE ]" : "VU METER");

    double w = width(), h = height();
    double R  = qMin(w * 0.42, h * 0.84);
    double cy = h * 0.90;
    double cxL = w * 0.28;
    double cxR = w * 0.72;

    drawChannel(p, cxL, cy, R, m_levelL, m_peakL, "L");
    drawChannel(p, cxR, cy, R, m_levelR, m_peakR, "R");
}
