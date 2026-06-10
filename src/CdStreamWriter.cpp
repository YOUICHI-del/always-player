#include "CdStreamWriter.h"
#include <QDir>
#include <QDateTime>
#include <QDebug>

CdStreamWriter::CdStreamWriter(RingBuffer *buffer, QObject *parent)
    : QThread(parent), m_buffer(buffer)
{}

CdStreamWriter::~CdStreamWriter()
{
    stop();
    wait();
    if (m_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hFile);
        m_hFile = INVALID_HANDLE_VALUE;
    }
}

bool CdStreamWriter::open()
{
    // ユニークな一時ファイル名を生成
    m_path = QDir::tempPath() +
             QString("/AlwaysCD_%1.wav").arg(QDateTime::currentMSecsSinceEpoch());

    // ★ FILE_SHARE_READ：mpvが同時に読み取れる
    m_hFile = CreateFileW(
        reinterpret_cast<LPCWSTR>(m_path.utf16()),
        GENERIC_WRITE | GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );

    if (m_hFile == INVALID_HANDLE_VALUE) {
        qDebug() << "[CdStreamWriter] CreateFile failed:" << GetLastError();
        return false;
    }

    writeWavHeader();
    qDebug() << "[CdStreamWriter] opened:" << m_path;
    return true;
}

void CdStreamWriter::writeWavHeader()
{
#pragma pack(push, 1)
    struct WavHeader {
        char     riff[4]       = {'R','I','F','F'};
        uint32_t fileSize      = 0xFFFFFFFF;
        char     wave[4]       = {'W','A','V','E'};
        char     fmt[4]        = {'f','m','t',' '};
        uint32_t fmtSize       = 16;
        uint16_t audioFormat   = 1;
        uint16_t channels      = 2;
        uint32_t sampleRate    = 44100;
        uint32_t byteRate      = 176400;
        uint16_t blockAlign    = 4;
        uint16_t bitsPerSample = 16;
        char     data[4]       = {'d','a','t','a'};
        uint32_t dataSize      = 0xFFFFFFFF;
    };
#pragma pack(pop)

    WavHeader hdr;
    DWORD written = 0;
    WriteFile(m_hFile, &hdr, sizeof(hdr), &written, nullptr);
    qDebug() << "[CdStreamWriter] WAV header written:" << written << "bytes";
}

void CdStreamWriter::stop()
{
    m_running.store(false);
}

void CdStreamWriter::run()
{
    m_running.store(true);
    m_totalBytes.store(0);

    constexpr size_t BUF_SIZE = 2352 * 16;
    uint8_t buf[BUF_SIZE];

    while (m_running.load()) {
        size_t avail = m_buffer->availableToRead();
        if (avail == 0) {
            msleep(2);
            continue;
        }

        size_t toRead = (avail < BUF_SIZE) ? avail : BUF_SIZE;
        toRead = (toRead / 2352) * 2352;
        if (toRead == 0) { msleep(2); continue; }

        size_t got = m_buffer->read(buf, toRead);
        if (got == 0) continue;

        DWORD written = 0;
        BOOL ok = WriteFile(m_hFile, buf, static_cast<DWORD>(got), &written, nullptr);
        if (!ok) {
            qDebug() << "[CdStreamWriter] WriteFile failed:" << GetLastError();
            emit writeError("WAV書き込みエラー");
            break;
        }
        m_totalBytes.fetch_add(written);
    }

    FlushFileBuffers(m_hFile);
    qDebug() << "[CdStreamWriter] done. total=" << m_totalBytes.load();
}
