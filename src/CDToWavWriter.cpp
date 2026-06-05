#include "CDToWavWriter.h"
#include <QDataStream>
#include <QDebug>

CDToWavWriter::CDToWavWriter(RingBuffer *buffer, const QString &path, qint64 totalPcmBytesExpected, QObject *parent)
    : QThread(parent)
    , m_buffer(buffer)
    , m_path(path)
    , m_expectedBytes(totalPcmBytesExpected)
{}

void CDToWavWriter::stopWriting()
{
    m_running.store(false);
}

void CDToWavWriter::run()
{
    QFile f(m_path);
    if (!f.open(QIODevice::WriteOnly)) {
        emit writeError(QString("一時WAVを開けません: %1").arg(m_path));
        return;
    }

    writeDummyHeader(f);
    m_running.store(true);
    m_totalBytes = 0;

    constexpr int BUF_SIZE = 2352 * 8;
    QByteArray buf(BUF_SIZE, 0);

    while (m_running.load()) {
        size_t avail = m_buffer->availableToRead();
        if (avail == 0) {
            msleep(2);
            continue;
        }
        size_t toRead = (avail < static_cast<size_t>(BUF_SIZE))
                        ? avail : static_cast<size_t>(BUF_SIZE);
        toRead = (toRead / 2352) * 2352;
        if (toRead == 0) { msleep(2); continue; }

        size_t got = m_buffer->read(
            reinterpret_cast<uint8_t*>(buf.data()), toRead);
        if (got == 0) continue;

        qint64 written = f.write(buf.constData(), static_cast<qint64>(got));
        if (written <= 0) {
            emit writeError("WAV書き込みエラー");
            break;
        }
        m_totalBytes += written;
        emit bytesWritten(m_totalBytes);
    }

    // ヘッダを正しいサイズで書き直す
    fixHeader(f, m_totalBytes);
    f.close();
    qDebug() << "[CDToWavWriter] done. total=" << m_totalBytes << "bytes";
}

void CDToWavWriter::writeDummyHeader(QFile &f)
{
    // 44.1kHz / 16bit / 2ch 固定
    QByteArray hdr;
    QDataStream s(&hdr, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);

    // ★ 期待されるサイズが分かっている場合は最初から正確な値を書く
    // → mpvがdurationを正しく計算できる
    quint32 dataSize = (m_expectedBytes > 0)
        ? static_cast<quint32>(m_expectedBytes)
        : 0;
    quint32 riffSize = (dataSize > 0)
        ? 4 + (8 + 16) + (8 + dataSize)
        : 0;

    s.writeRawData("RIFF", 4);
    s << riffSize;                 // riffSize（正確 or 仮）
    s.writeRawData("WAVE", 4);

    s.writeRawData("fmt ", 4);
    s << quint32(16);              // fmtSize
    s << quint16(1);               // PCM
    s << quint16(2);               // 2ch
    s << quint32(44100);           // sampleRate
    s << quint32(44100 * 2 * 2);   // byteRate
    s << quint16(2 * 2);           // blockAlign
    s << quint16(16);              // bitsPerSample

    s.writeRawData("data", 4);
    s << dataSize;                 // dataSize（正確 or 仮）

    f.write(hdr);
    qDebug() << "[CDToWavWriter] header written: expectedBytes=" << m_expectedBytes;
}

void CDToWavWriter::fixHeader(QFile &f, qint64 dataBytes)
{
    if (!f.isOpen()) {
        f.open(QIODevice::ReadWrite);
    }

    quint32 dataSize = static_cast<quint32>(dataBytes);
    // riffSize = "WAVE"(4) + "fmt "(4)+4+16 + "data"(4)+4 + dataSize
    quint32 riffSize = 4 + (8 + 16) + (8 + dataSize);

    // RIFF size（オフセット4）
    f.seek(4);
    QDataStream s(&f);
    s.setByteOrder(QDataStream::LittleEndian);
    s << riffSize;

    // data size（オフセット4+4+4 + 4+4+16 + 4 = 40）
    f.seek(40);
    s << dataSize;

    qDebug() << "[CDToWavWriter] header fixed: dataSize=" << dataSize;
}
