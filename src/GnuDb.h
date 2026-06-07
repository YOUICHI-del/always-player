#pragma once
#include <QString>
#include <QVector>
#include "CdDrive.h"

struct GnuDbTrack {
    int     number;
    QString title;
};

struct GnuDbResult {
    QString          artist;
    QString          album;
    QString          year;
    QString          genre;
    QVector<GnuDbTrack> tracks;
    bool isValid() const { return !album.isEmpty(); }
};

class GnuDb
{
public:
    // メイン：DiscInfoからアルバム情報を取得
    static GnuDbResult lookup(const DiscInfo &disc);

    // CDDB DiscID計算（freedb/gnudb用）
    static QString calcDiscId(const DiscInfo &disc);

public:
    static QByteArray httpGet(const QString &url);
    static GnuDbResult parseEntry(const QString &entry);
};
