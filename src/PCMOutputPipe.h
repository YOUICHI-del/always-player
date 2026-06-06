#pragma once
#include <QThread>
#include <atomic>
#include <windows.h>
#include "RingBuffer.h"
#include <QDateTime>

class PCMOutputPipe : public QThread
{
    Q_OBJECT
public:
    explicit PCMOutputPipe(RingBuffer *buffer, QObject *parent = nullptr);
    ~PCMOutputPipe();

    void stopOutput();
    QString pipePath() const { return m_pipeName; }

private:
    QString m_pipeName;
    static QString generatePipeName() {
        return QString("\\\\.\\pipe\\AlwaysCD_%1")
               .arg(QDateTime::currentMSecsSinceEpoch());
    }

signals:
    void pipeCreated(const QString &pipePath); // Pipe作成完了→mpvに知らせる
    void pipeReady();   // WAVヘッダ送信後・CDReader起動タイミング
    void pipeError(const QString &msg);

protected:
    void run() override;

private:
    bool sendWavHeader();

    RingBuffer         *m_buffer;
    HANDLE              m_hPipe  = INVALID_HANDLE_VALUE;
    std::atomic<bool>   m_running { false };
};
