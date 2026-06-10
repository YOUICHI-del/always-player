#pragma once
#include <QThread>
#include <QObject>
#include <QString>
#include <atomic>
#include <windows.h>
#include "RingBuffer.h"

// ★ CD Stream Mode用：一時WAVファイルにリアルタイム書き込み
// FILE_SHARE_READでmpvが同時に読み取れる
class CdStreamWriter : public QThread
{
    Q_OBJECT
public:
    explicit CdStreamWriter(RingBuffer *buffer, QObject *parent = nullptr);
    ~CdStreamWriter();

    bool open();       // 一時ファイル作成・WAVヘッダ書き込み
    void stop();
    QString filePath() const { return m_path; }
    qint64 totalBytes() const { return m_totalBytes.load(); }

signals:
    void writeError(const QString &msg);

protected:
    void run() override;

private:
    void writeWavHeader();

    RingBuffer          *m_buffer;
    QString              m_path;
    HANDLE               m_hFile = INVALID_HANDLE_VALUE;
    std::atomic<bool>    m_running { false };
    std::atomic<qint64>  m_totalBytes { 0 };
};
