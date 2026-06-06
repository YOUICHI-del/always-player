#pragma once
#include <QWidget>
#include <QTimer>

class Player; // 前方宣言

class VUMeter : public QWidget
{
    Q_OBJECT

public:
    explicit VUMeter(QWidget *parent = nullptr);
    ~VUMeter();

    void setPlaying(bool playing);
    void setPlayer(Player *p);   // ★追加：Player を受け取る

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void tick();

    Player *m_player = nullptr;  // ★追加：Player へのポインタ

    double m_levelL  = 0.02;
    double m_levelR  = 0.02;
    double m_peakL   = 0.0;
    double m_peakR   = 0.0;
    double m_velL    = 0.0;
    double m_velR    = 0.0;
    double m_t       = 0.0;
    bool   m_playing = false;

    QTimer *m_timer = nullptr;
};
