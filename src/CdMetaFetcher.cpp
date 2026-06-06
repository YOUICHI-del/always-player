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
                qDebug() << "[CdMetaFetcher] CAA loadFromData=" << ok << "null=" << res.coverArt.isNull();
            }
        }

        // アートが取れなかった場合は iTunes からも試みる
        if (res.coverArt.isNull() && !mb.artist.isEmpty() && !mb.title.isEmpty()) {
            qDebug() << "[CdMetaFetcher] trying iTunes art for" << mb.artist << mb.title;
            CoverArt ca;
            iTunesAlbumInfo info = ca.searchITunesFull(mb.artist, mb.title);
            qDebug() << "[CdMetaFetcher] iTunes artUrl=" << info.artUrl;
            if (!info.artUrl.isEmpty()) {
                QByteArray img = MusicBrainz::httpGet(info.artUrl);
                qDebug() << "[CdMetaFetcher] iTunes art bytes=" << img.size();
                if (!img.isEmpty()) res.coverArt.loadFromData(img);
            }
        }

        qDebug() << "[CdMetaFetcher] MB hit:"
                 << res.albumTitle << "/" << res.artist
                 << "tracks:" << res.trackNames.size()
                 << "art:" << !res.coverArt.isNull();
        return res;
    }

    // ══ Step 2: iTunes Store JP フォールバック ════
    // MusicBrainz ミス時は disc.artist/albumTitle が空でも
    // トラック数ベースで iTunes TOC 検索を試みる
    QString fbArtist = disc.artist.trimmed();
    QString fbAlbum  = disc.albumTitle.trimmed();

    // DiscID から TOC 文字列を生成して iTunes で検索
    // iTunes の /lookup?toc= エンドポイントを使う
    {
        // TOC形式: first_track last_track leadout_offset track1_offset track2_offset ...
        // offsetはLBA (sector)
        if (!disc.tracks.isEmpty()) {
            int first = disc.tracks.first().number;
            int last  = disc.tracks.last().number;
            quint32 leadout = static_cast<quint32>(disc.totalSectors + 150);

            QStringList toc;
            toc << QString::number(first)
                << QString::number(last)
                << QString::number(leadout);
            for (const auto &t : disc.tracks)
                toc << QString::number(static_cast<quint32>(t.startSector + 150));

            QString tocStr = toc.join("+");
            QString itunesTocUrl = QString(
                "https://itunes.apple.com/WebObjects/MZStoreServices.woa/wa/wsLookup"
                "?lookup=1&country=JP&lang=ja_jp&entity=album&toc=%1").arg(tocStr);

            qDebug() << "[CdMetaFetcher] iTunes TOC URL:" << itunesTocUrl;
            QByteArray tocData = MusicBrainz::httpGet(itunesTocUrl);

            if (!tocData.isEmpty()) {
                QJsonDocument doc = QJsonDocument::fromJson(tocData);
                QJsonArray results = doc.object().value("results").toArray();
                if (!results.isEmpty()) {
                    QJsonObject best = results[0].toObject();
                    fbArtist = best.value("artistName").toString();
                    fbAlbum  = best.value("collectionName").toString();
                    qDebug() << "[CdMetaFetcher] iTunes TOC hit:" << fbAlbum << "/" << fbArtist;
                }
            }
        }
    }

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

        // トラック数が±1以内なら採用（日本盤/海外盤の差を許容）
        int diff = qAbs(info.tracks.size() - disc.tracks.size());
        if (diff <= 1 && info.tracks.size() > 0) {
            for (int i = 0; i < disc.tracks.size(); ++i) {
                if (i < info.tracks.size())
                    res.trackNames << info.tracks[i];
                else
                    res.trackNames << QString("Track %1").arg(i + 1, 2, 10, QChar('0'));
            }
        } else {
            qWarning() << "[CdMetaFetcher] iTunes track count mismatch:"
                       << info.tracks.size() << "vs" << disc.tracks.size();
            for (int i = 0; i < disc.tracks.size(); ++i)
                res.trackNames << QString("Track %1").arg(i + 1, 2, 10, QChar('0'));
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
