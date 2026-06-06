#include "CdMetaFetcher.h"
#include "MusicBrainz.h"
#include "CoverArt.h"
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QDebug>

CdMetaFetcher::CdMetaFetcher(QObject *parent) : QObject(parent) {}

// ─────────────────────────────────────────────
//  fetchAsync : ワーカースレッドを起動して即リターン
// ─────────────────────────────────────────────
void CdMetaFetcher::fetchAsync(const DiscInfo &disc)
{
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher]() {
        emit metaReady(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(&CdMetaFetcher::doFetch, disc));
}

// ─────────────────────────────────────────────
//  doFetch : バックグラウンドスレッドで実行
//  MusicBrainz DiscID → Cover Art Archive
//  ↓ミス
//  iTunes Store JP フォールバック
// ─────────────────────────────────────────────
CdMetaFetcher::Result CdMetaFetcher::doFetch(DiscInfo disc)
{
    Result res;
    if (disc.tracks.isEmpty()) return res;

    // ══ Step 1: MusicBrainz DiscID ════════════════
    MbRelease mb = MusicBrainz::lookup(disc);

    if (!mb.title.isEmpty()) {
        // ── MusicBrainz ヒット ──
        res.found      = true;
        res.albumTitle = mb.title;
        res.artist     = mb.artist;
        res.year       = mb.date;

        // トラック名（disc 1 のみ。多枚組でも disc 1 が対象）
        const auto &tracks = mb.discs.isEmpty() ? mb.tracks : mb.discs[0].tracks;
        for (int i = 0; i < disc.tracks.size(); ++i) {
            if (i < tracks.size())
                res.trackNames << tracks[i].title;
            else
                res.trackNames << QString("Track %1").arg(i + 1, 2, 10, QChar('0'));
        }

        // アルバムアート：Cover Art Archive
        if (!mb.mbid.isEmpty()) {
            QByteArray img = MusicBrainz::fetchCoverArt(mb.mbid);
            if (!img.isEmpty())
                res.coverArt.loadFromData(img);
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

    // ══ Step 2: iTunes Store JP フォールバック ════
    // MusicBrainz ミスの場合、DiscInfoにアーティスト/アルバムが無い可能性が高い。
    // GnuDB 相当の情報が albumTitle / artist に入っている場合のみ試みる。
    QString fbArtist = disc.artist.trimmed();
    QString fbAlbum  = disc.albumTitle.trimmed();

    if (fbArtist.isEmpty() && fbAlbum.isEmpty()) {
        qDebug() << "[CdMetaFetcher] no fallback info, giving up";
        return res;  // 手掛かりなし
    }

    CoverArt ca;
    iTunesAlbumInfo info = ca.searchITunesFull(fbArtist, fbAlbum);

    if (info.isValid()) {
        res.found      = true;
        res.albumTitle = info.album;
        res.artist     = info.artist;
        res.year       = info.year;

        // トラック名：iTunes トラック数とCD実トラック数が合う場合のみ採用
        if (info.tracks.size() == disc.tracks.size()) {
            res.trackNames = info.tracks;
        } else {
            qWarning() << "[CdMetaFetcher] iTunes track count mismatch:"
                       << info.tracks.size() << "vs" << disc.tracks.size();
            for (int i = 0; i < disc.tracks.size(); ++i)
                res.trackNames << QString("Track %1").arg(i + 1, 2, 10, QChar('0'));
        }

        // アルバムアート
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
