#pragma once
#include <QMutex>
#include <QMutexLocker>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QStringList>
#include <QDebug>
#include <windows.h>
#include <io.h>        // _get_osfhandle
#include <string>

// ★ v10: AlwaysPlayer.ini を書き換える処理（MainWindowのお気に入り・設定保存、
//   Playerの最後のフォルダ保存、表示言語の保存）は、すべてこのmutexで1本に直列化する。
//   別々のスレッドが同じiniを「読んで→書き換えて→置き換える」と、
//   後から書いた方が先の変更を上書きして消してしまうため。
inline QMutex &iniFileMutex()
{
    static QMutex m;
    return m;
}

inline QString iniFilePath()
{
    return QDir::homePath() + "/AlwaysPlayer.ini";
}

// ★ v10: [section] の key= の値を読む（無ければ空文字）。
inline QString iniReadValue(const QString &section, const QString &key)
{
    QMutexLocker lock(&iniFileMutex());
    QFile f(iniFilePath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    const QString header = "[" + section + "]";
    const QString prefix = key + "=";
    bool inSection = false;
    while (!in.atEnd()) {
        const QString t = in.readLine().trimmed();
        if (t.startsWith('[')) { inSection = (t == header); continue; }
        if (inSection && t.startsWith(prefix)) return t.mid(prefix.size());
    }
    return {};
}

// ★ v10: [section] の key= だけを書き換える（他の行はすべてそのまま残す）。
//   tmpに書いてディスクまでフラッシュし、MoveFileExWで一度に置き換える。
inline bool iniWriteValue(const QString &section, const QString &key, const QString &value)
{
    QMutexLocker lock(&iniFileMutex());
    const QString ini = iniFilePath();
    const QString tmp = ini + ".kv.tmp";

    QStringList lines;
    {
        QFile rf(ini);
        if (rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&rf);
            in.setEncoding(QStringConverter::Utf8);
            while (!in.atEnd()) lines << in.readLine();
        } else if (QFile::exists(ini)) {
            return false;   // 存在するのに読めない：壊さないよう何もしない
        }
    }
    const QString header = "[" + section + "]";
    const QString prefix = key + "=";
    bool inSection = false, replaced = false;
    int headerIdx = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = lines[i].trimmed();
        if (t.startsWith('[')) {
            inSection = (t == header);
            if (inSection && headerIdx < 0) headerIdx = i;
            continue;
        }
        if (inSection && t.startsWith(prefix)) { lines[i] = prefix + value; replaced = true; break; }
    }
    if (!replaced) {
        if (headerIdx >= 0) lines.insert(headerIdx + 1, prefix + value);
        else { lines.prepend(prefix + value); lines.prepend(header); }
    }

    {
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
        QTextStream out(&f);
        out.setEncoding(QStringConverter::Utf8);
        for (const QString &l : lines) out << l << "\n";
        out.flush();
        f.flush();
        if (f.handle() >= 0)
            FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(f.handle())));
        f.close();
        if (out.status() != QTextStream::Ok || f.error() != QFileDevice::NoError) {
            QFile::remove(tmp);
            return false;
        }
    }
    const std::wstring tmpW = QDir::toNativeSeparators(tmp).toStdWString();
    const std::wstring iniW = QDir::toNativeSeparators(ini).toStdWString();
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (attempt > 0) Sleep(100);
        if (MoveFileExW(tmpW.c_str(), iniW.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    }
    qWarning() << "[INI] write failed:" << GetLastError();
    QFile::remove(tmp);
    return false;
}
