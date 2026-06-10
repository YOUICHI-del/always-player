#pragma once
#include <QThread>
#include <atomic>
#include "RingBuffer.h"
#include "CdDrive.h"

// ─────────────────────────────────────────────
// CDReader
// CDドライブからセクタを順次読み取り、
// RingBufferへPCMを書き込むスレッド
// 1セクタ = 2352バイト = 588サンプル
// ─────────────────────────────────────────────
class CDReader : public QThread {
    Q_OBJECT
public:
    explicit CDReader(RingBuffer *buffer, QObject *parent = nullptr);
    ~CDReader();

    // トラック再生を開始（startSector〜endSector）
    void startTrack(const QString &drive, DWORD startSector, DWORD endSector);

    // シーク（セクタ指定）
    void requestSeek(DWORD sector);

    // 停止
    void stopReading();

    // 現在読み取り中のセクタ
    DWORD currentSector() const { return m_currentSector.load(); }

signals:
    void readError(const QString &msg);
    void finished();

protected:
    void run() override;

private:
    RingBuffer           *m_buffer;
    CdDrive               m_drive;
    QString               m_driveLetter;
    DWORD                 m_startSector   = 0;
    DWORD                 m_endSector     = 0;
    std::atomic<DWORD>    m_currentSector { 0 };
    std::atomic<bool>     m_running       { false };
    std::atomic<bool>     m_seekRequested { false };
    std::atomic<DWORD>    m_seekSector    { 0 };
};
