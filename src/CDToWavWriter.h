#pragma once
#include <QThread>
#include <QFile>
#include <atomic>
#include "RingBuffer.h"

// ─────────────────────────────────────────────
// CDToWavWriter
// RingBufferからPCMを読み出し、一時WAVファイルに書き出す
// 3秒分書けたらbytesWrittenシグナルでmpv起動を通知
// ─────────────────────────────────────────────
class CDToWavWriter : public QThread {
    Q_OBJECT
public:
    CDToWavWriter(RingBuffer *buffer, const QString &path, qint64 totalPcmBytesExpected = 0, QObject *parent = nullptr);
    void stopWriting();
    qint64 totalPcmBytes() const { return m_totalBytes; }

signals:
    void bytesWritten(qint64 total);
    void writeError(const QString &msg);

protected:
    void run() override;

private:
    void writeDummyHeader(QFile &f);
    void fixHeader(QFile &f, qint64 dataBytes);

    RingBuffer          *m_buffer;
    QString              m_path;
    std::atomic<bool>    m_running { false };
    qint64               m_totalBytes = 0;
    qint64               m_expectedBytes = 0;
};
