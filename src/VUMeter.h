#pragma once
#include <QWidget>
#include <QTimer>

class Player;

class VUMeter : public QWidget
{
    Q_OBJECT

public:
    explicit VUMeter(QWidget *parent = nullptr);
    ~VUMeter();

    void setPlaying(bool playing);
    void setPlayer(Player *p);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void tick();

    Player *m_player = nullptr;

    double m_levelL  = 0.02;
    double m_levelR  = 0.02;
    double m_peakL   = 0.0;
    double m_peakR   = 0.0;
    bool   m_playing = false;

    QTimer *m_timer = nullptr;
};
