#include "CdMetaFetcher.h"
#include "MusicBrainz.h"
#include "CoverArt.h"
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

CdMetaFetcher::CdMetaFetcher(QObject *parent) : QObject(parent) {}

void CdMetaFetcher::fetchAsync(const DiscInfo &disc)
{
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher]() {
        emit metaReady(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(&CdMetaFetcher::doFetch, disc));
}

CdMetaFetcher::Result CdMetaFetcher::doFetch(DiscInfo disc)
{
    Result res;
    if (disc.tracks.isEmpty()) return res;

    // ══ Step 1: MusicBrainz DiscID ════════════════
    MbRelease mb = MusicBrainz::lookup(disc);

    if (!mb.title.isEmpty()) {
        res.found      = true;
        res.albumTitle = mb.title;
        res.artist     = mb.artist;
        res.year       = mb.date;

        const auto &tracks = mb.discs.isEmpty() ? mb.tracks : mb.discs[0].tracks;
        for (int i = 0; i < disc.tracks.size(); ++i) {
            if (i < tracks.size())
                res.trackNames << tracks[i].title;
            else
                res.trackNames << QString("Track %1").arg(i + 1, 2, 10, QChar('0'));
        }

        // アルバムアート：Cover Art Archive
        if (!mb.mbid.isEmpty()) {
            qDebug() << "[CdMetaFetcher] fetching CAA art for mbid=" << mb.mbid;
            QByteArray img = MusicBrainz::fetchCoverArt(mb.mbid);
            qDebug() << "[CdMetaFetcher] CAA bytes=" << img.size();
            if (!img.isEmpty()) {
                bool ok = res.coverArt.loadFromData(img);
                qDebug() << "[CdMetaFetcher] CAA loadFromData=" << ok;
            }
        }

        // アートが取れなかった場合は iTunes からも試みる
        if (res.coverArt.isNull() && !mb.artist.isEmpty() && !mb.title.isEmpty()) {
            CoverArt ca;
            iTunesAlbumInfo info = ca.searchITunesFull(mb.artist, mb.title);
            if (!info.artUrl.isEmpty()) {
                QByteArray img = MusicBrainz::httpGet(info.artUrl);
                if (!img.isEmpty()) res.coverArt.loadFromData(img);
            }
        }

        qDebug() << "[CdMetaFetcher] MB hit:"
                 << res.albumTitle << "/" << res.artist
                 << "tracks:" << res.trackNames.size()
                 << "art:" << !res.coverArt.isNull();
        return res;
    }

    // ══ Step 2: iTunes TOC lookup ════════════════
    // Apple Music API: /v1/catalog/jp/albums?filter[equivalents]=...
    // フォールバック：トラック数ベースで iTunes Search API を使う
    {
        // TOC形式でiTunes Music Store APIに問い合わせ
        // 正しいエンドポイント: https://itunes.apple.com/lookup?toc=...
        int first = disc.tracks.first().number;
        int last  = disc.tracks.last().number;
        quint32 leadout = static_cast<quint32>(disc.totalSectors + 150);

        QStringList tocParts;
        tocParts << QString::number(first)
                 << QString::number(last)
                 << QString::number(leadout);
        for (const auto &t : disc.tracks)
            tocParts << QString::number(static_cast<quint32>(t.startSector + 150));

        QString tocStr = tocParts.join("+");
        // iTunes Store TOC lookup（正式エンドポイント）
        QString tocUrl = QString(
            "https://itunes.apple.com/lookup?toc=%1&country=JP&lang=ja_jp&entity=album")
            .arg(tocStr);

        qDebug() << "[CdMetaFetcher] iTunes TOC URL:" << tocUrl;
        QByteArray tocData = MusicBrainz::httpGet(tocUrl);
        qDebug() << "[CdMetaFetcher] iTunes TOC response:" << tocData.left(200);

        if (!tocData.isEmpty()) {
            QJsonDocument doc = QJsonDocument::fromJson(tocData);
            QJsonObject root = doc.object();
            int resultCount = root.value("resultCount").toInt();
            QJsonArray results = root.value("results").toArray();
            qDebug() << "[CdMetaFetcher] iTunes TOC resultCount=" << resultCount;

            // resultsからアルバム情報を抽出
            for (const QJsonValue &rv : results) {
                QJsonObject obj = rv.toObject();
                QString wrapperType = obj.value("wrapperType").toString();
                QString kind = obj.value("kind").toString();
                // アルバム情報（collectionType=="Album"）
                if (obj.contains("collectionName") && !obj.value("collectionName").toString().isEmpty()) {
                    QString artist  = obj.value("artistName").toString();
                    QString album   = obj.value("collectionName").toString();
                    QString artUrl  = obj.value("artworkUrl100").toString()
                                        .replace("100x100bb", "600x600bb");
                    QString year    = obj.value("releaseDate").toString().left(4);
                    qlonglong collId = static_cast<qlonglong>(obj.value("collectionId").toDouble());

                    if (!album.isEmpty()) {
                        qDebug() << "[CdMetaFetcher] iTunes TOC hit:" << album << "/" << artist;

                        // トラック情報を取得
                        CoverArt ca;
                        iTunesAlbumInfo info = ca.searchITunesFull(artist, album);

                        res.found      = true;
                        res.albumTitle = album;
                        res.artist     = artist;
                        res.year       = year;

                        int diff = qAbs(info.tracks.size() - disc.tracks.size());
                        if (info.isValid() && diff <= 1) {
                            for (int i = 0; i < disc.tracks.size(); ++i) {
                                if (i < info.tracks.size())
                                    res.trackNames << info.tracks[i];
                                else
                                    res.trackNames << QString("Track %1").arg(i+1, 2, 10, QChar('0'));
                            }
                        } else {
                            for (int i = 0; i < disc.tracks.size(); ++i)
                                res.trackNames << QString("Track %1").arg(i+1, 2, 10, QChar('0'));
                        }

                        // アルバムアート
                        if (!artUrl.isEmpty()) {
                            QByteArray img = MusicBrainz::httpGet(artUrl);
                            if (!img.isEmpty()) res.coverArt.loadFromData(img);
                        }
                        return res;
                    }
                }
            }
        }
    }

    // ══ Step 3: MusicBrainz TOC fuzzy search ════
    {
        int first = disc.tracks.first().number;
        int last  = disc.tracks.last().number;
        quint32 leadout = static_cast<quint32>(disc.totalSectors + 150);
        QStringList tocParts;
        tocParts << QString::number(first) << QString::number(last) << QString::number(leadout);
        for (const auto &t : disc.tracks)
            tocParts << QString::number(static_cast<quint32>(t.startSector + 150));
        QString encodedToc = tocParts.join("+");

        QString mbTocUrl = QString(
            "https://musicbrainz.org/ws/2/discid/-"
            "?toc=%1&inc=recordings+artists&fmt=json&cdstubs=no")
            .arg(encodedToc);
        qDebug() << "[CdMetaFetcher] MB TOC fuzzy URL:" << mbTocUrl;
        QByteArray mbTocData = MusicBrainz::httpGet(mbTocUrl);
        qDebug() << "[CdMetaFetcher] MB TOC response:" << mbTocData.left(200);

        if (!mbTocData.isEmpty()) {
            QJsonDocument doc = QJsonDocument::fromJson(mbTocData);
            QJsonArray releases = doc.object().value("releases").toArray();
            qDebug() << "[CdMetaFetcher] MB TOC releases:" << releases.size();
            if (!releases.isEmpty()) {
                QJsonObject rel = releases[0].toObject();
                QString title = rel.value("title").toString();
                QString mbid  = rel.value("id").toString();
                QJsonArray credits = rel.value("artist-credit").toArray();
                QString artist;
                if (!credits.isEmpty())
                    artist = credits[0].toObject().value("artist").toObject().value("name").toString();

                if (!title.isEmpty()) {
                    qDebug() << "[CdMetaFetcher] MB TOC hit:" << title << "/" << artist;
                    res.found      = true;
                    res.albumTitle = title;
                    res.artist     = artist;
                    res.year       = rel.value("date").toString().left(4);

                    // トラック数が一致するmediaを探す
                    QJsonArray media = rel.value("media").toArray();
                    QJsonArray tracks;
                    for (const QJsonValue &mv : media) {
                        QJsonArray mt = mv.toObject().value("tracks").toArray();
                        if (mt.size() == disc.tracks.size()) { tracks = mt; break; }
                    }
                    if (tracks.isEmpty() && !media.isEmpty())
                        tracks = media[0].toObject().value("tracks").toArray();

                    for (int i = 0; i < disc.tracks.size(); ++i) {
                        if (i < tracks.size())
                            res.trackNames << tracks[i].toObject().value("title").toString();
                        else
                            res.trackNames << QString("Track %1").arg(i+1, 2, 10, QChar('0'));
                    }

                    // アルバムアート
                    if (!mbid.isEmpty()) {
                        QByteArray img = MusicBrainz::fetchCoverArt(mbid);
                        if (!img.isEmpty()) res.coverArt.loadFromData(img);
                    }
                    if (res.coverArt.isNull() && !artist.isEmpty()) {
                        CoverArt ca2;
                        iTunesAlbumInfo info2 = ca2.searchITunesFull(artist, title);
                        if (!info2.artUrl.isEmpty()) {
                            QByteArray img = MusicBrainz::httpGet(info2.artUrl);
                            if (!img.isEmpty()) res.coverArt.loadFromData(img);
                        }
                    }
                    return res;
                }
            }
        }
    }

    // ══ Step 4: disc.artist/albumTitle があればiTunes検索 ════
    QString fbArtist = disc.artist.trimmed();
    QString fbAlbum  = disc.albumTitle.trimmed();

    if (fbArtist.isEmpty() && fbAlbum.isEmpty()) {
        qDebug() << "[CdMetaFetcher] no fallback info, giving up";
        return res;
    }

    CoverArt ca;
    iTunesAlbumInfo info = ca.searchITunesFull(fbArtist, fbAlbum);

    if (info.isValid()) {
        res.found      = true;
        res.albumTitle = info.album;
        res.artist     = info.artist;
        res.year       = info.year;

        int diff = qAbs(info.tracks.size() - disc.tracks.size());
        if (diff <= 1 && info.tracks.size() > 0) {
            for (int i = 0; i < disc.tracks.size(); ++i) {
                if (i < info.tracks.size())
                    res.trackNames << info.tracks[i];
                else
                    res.trackNames << QString("Track %1").arg(i+1, 2, 10, QChar('0'));
            }
        } else {
            for (int i = 0; i < disc.tracks.size(); ++i)
                res.trackNames << QString("Track %1").arg(i+1, 2, 10, QChar('0'));
        }

        if (!info.artUrl.isEmpty()) {
            QByteArray img = MusicBrainz::httpGet(info.artUrl);
            if (!img.isEmpty()) res.coverArt.loadFromData(img);
        }

        qDebug() << "[CdMetaFetcher] iTunes fallback hit:"
                 << res.albumTitle << "/" << res.artist;
    } else {
        qWarning() << "[CdMetaFetcher] all sources missed";
    }

    return res;
}
