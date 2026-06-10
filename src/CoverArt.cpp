#include "CoverArt.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QDebug>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// ============================================================
//  httpGet  （既存のまま・変更なし）
// ============================================================
QByteArray CoverArt::httpGet(const QString &urlStr)
{
    QUrl url(urlStr);
    QString host = url.host();
    QString fullPath = urlStr.mid(urlStr.indexOf(host) + host.length());
    if (fullPath.isEmpty()) fullPath = "/";

    HINTERNET hSession = WinHttpOpen(
        L"AlwaysCDRipper/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        reinterpret_cast<LPCWSTR>(host.utf16()),
        url.scheme() == "https" ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT,
        0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }

    DWORD flags = url.scheme() == "https" ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET",
        reinterpret_cast<LPCWSTR>(fullPath.utf16()),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // MusicBrainz / Discogs は User-Agent 必須
    WinHttpAddRequestHeaders(hRequest,
        L"User-Agent: AlwaysCDRipper/1.0 ( https://example.com )",
        (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    // リダイレクト自動追跡
    DWORD option = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
                     &option, sizeof(option));

    BOOL ok = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);

    QByteArray result;
    if (ok) {
        DWORD size = 0;
        do {
            size = 0;
            WinHttpQueryDataAvailable(hRequest, &size);
            if (size == 0) break;
            QByteArray buf(size, '\0');
            DWORD downloaded = 0;
            WinHttpReadData(hRequest, buf.data(), size, &downloaded);
            result.append(buf.left(downloaded));
        } while (size > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

// ============================================================
//  MusicBrainz  : アーティスト＋アルバム名 → MBID
// ============================================================
QString CoverArt::searchMusicBrainzReleaseId(const QString &artist, const QString &album)
{
    // Lucene形式クエリ
    QString query = QString("artist:\"%1\" AND release:\"%2\"")
                        .arg(artist, album);
    QString encoded = QString(QUrl::toPercentEncoding(query));
    QString url = QString("https://musicbrainz.org/ws/2/release/"
                          "?query=%1&fmt=json&limit=5").arg(encoded);

    QByteArray data = httpGet(url);
    if (data.isEmpty()) {
        qWarning() << "CoverArt[MB]: request failed";
        return {};
    }

    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonArray releases = doc.object().value("releases").toArray();
    if (releases.isEmpty()) {
        qWarning() << "CoverArt[MB]: no releases found";
        return {};
    }

    // スコア最高の先頭を採用
    QString mbid = releases[0].toObject().value("id").toString();
    qDebug() << "CoverArt[MB]: MBID =" << mbid;
    return mbid;
}

// ============================================================
//  MusicBrainz  : MBID → Cover Art Archive URL
// ============================================================
QString CoverArt::musicBrainzCoverUrl(const QString &mbid)
{
    if (mbid.isEmpty()) return {};

    // 500pxサムネイル。リダイレクト先が実際の画像URL
    return QString("https://coverartarchive.org/release/%1/front-500").arg(mbid);
}

// ============================================================
//  Discogs  : アーティスト＋アルバム名 → カバー画像URL
//  事前に setDiscogsToken() でトークンをセットしておくこと
// ============================================================
QString CoverArt::searchDiscogs(const QString &artist, const QString &album)
{
    if (discogsToken.isEmpty()) {
        qDebug() << "CoverArt[Discogs]: token not set, skip";
        return {};
    }

    QString q = QString("%1 %2").arg(artist, album);
    QString encoded = QString(QUrl::toPercentEncoding(q));
    QString url = QString("https://api.discogs.com/database/search"
                          "?q=%1&type=release&per_page=5&token=%2")
                          .arg(encoded, discogsToken);

    QByteArray data = httpGet(url);
    if (data.isEmpty()) {
        qWarning() << "CoverArt[Discogs]: request failed";
        return {};
    }

    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonArray results = doc.object().value("results").toArray();
    if (results.isEmpty()) {
        qWarning() << "CoverArt[Discogs]: no results";
        return {};
    }

    QJsonObject r0 = results[0].toObject();
    // cover_image が大きい画像、thumb は小さいサムネイル
    QString img = r0.value("cover_image").toString();
    if (img.isEmpty())
        img = r0.value("thumb").toString();

    qDebug() << "CoverArt[Discogs]: url =" << img;
    return img;
}

// ============================================================
//  iTunes  : アーティスト＋アルバム名 → アルバム情報＋曲名リスト一括取得
//  日本盤・演歌・J-POP に強い。country=JP で日本語データ優先。
// ============================================================
iTunesAlbumInfo CoverArt::searchITunesFull(const QString &artist, const QString &album)
{
    iTunesAlbumInfo info;

    // ── Step1: アルバム検索（collectionId を取得）──────────────
    QString term = QString(QUrl::toPercentEncoding(artist + " " + album))
                   .replace("%20", "+");
    QString albumUrl = QString(
        "https://itunes.apple.com/search"
        "?term=%1&entity=album&limit=10&country=JP&lang=ja_jp").arg(term);

    qDebug() << "[iTunes] album search:" << albumUrl;
    QByteArray albumData = httpGet(albumUrl);
    if (albumData.isEmpty()) {
        qWarning() << "[iTunes] album search failed";
        return info;
    }

    QJsonDocument albumDoc = QJsonDocument::fromJson(albumData);
    QJsonArray albumResults = albumDoc.object().value("results").toArray();
    qDebug() << "[iTunes] album results:" << albumResults.size();
    if (albumResults.isEmpty()) return info;

    // アルバム名の類似度でベスト候補を選ぶ
    QJsonObject bestAlbum;
    for (const QJsonValue &rv : albumResults) {
        QJsonObject obj = rv.toObject();
        QString col = obj.value("collectionName").toString();
        if (col.contains(album, Qt::CaseInsensitive) ||
            album.contains(col, Qt::CaseInsensitive)) {
            bestAlbum = obj;
            break;
        }
    }
    if (bestAlbum.isEmpty())
        bestAlbum = albumResults[0].toObject();

    qlonglong collectionId = static_cast<qlonglong>(
        bestAlbum.value("collectionId").toDouble());
    info.artist  = bestAlbum.value("artistName").toString();
    info.album   = bestAlbum.value("collectionName").toString();
    info.year    = bestAlbum.value("releaseDate").toString().left(4);
    info.artUrl  = bestAlbum.value("artworkUrl100").toString()
                       .replace("100x100bb", "600x600bb");

    qDebug() << "[iTunes] best album:" << info.album
             << "artist:" << info.artist
             << "year:" << info.year
             << "collectionId:" << collectionId;

    // ── Step2: トラックリスト取得（collectionId で lookup）────────
    if (collectionId > 0) {
        QString trackUrl = QString(
            "https://itunes.apple.com/lookup"
            "?id=%1&entity=song&country=JP&lang=ja_jp")
            .arg(collectionId);

        qDebug() << "[iTunes] track lookup:" << trackUrl;
        QByteArray trackData = httpGet(trackUrl);
        if (!trackData.isEmpty()) {
            QJsonDocument trackDoc = QJsonDocument::fromJson(trackData);
            QJsonArray trackResults = trackDoc.object().value("results").toArray();

            // 先頭はアルバム自体なのでスキップ、2番目以降がトラック
            QMap<int, QString> trackMap;
            for (const QJsonValue &tv : trackResults) {
                QJsonObject t = tv.toObject();
                if (t.value("wrapperType").toString() != "track") continue;
                int    num   = static_cast<int>(t.value("trackNumber").toDouble());
                QString name = t.value("trackName").toString();
                if (num > 0 && !name.isEmpty())
                    trackMap[num] = name;
            }

            // 曲順に並べる
            QList<int> keys = trackMap.keys();
            std::sort(keys.begin(), keys.end());
            for (int k : keys)
                info.tracks << trackMap[k];

            qDebug() << "[iTunes] tracks:" << info.tracks.size();
        }
    }

    return info;
}

// ============================================================
//  iTunes  : アーティスト＋アルバム名 → アートワークURL（後方互換）
// ============================================================
QString CoverArt::searchITunes(const QString &artist, const QString &album)
{
    QString term = QString(QUrl::toPercentEncoding(artist + " " + album))
                   .replace("%20", "+");
    QString url = QString("https://itunes.apple.com/search"
                          "?term=%1&entity=album&limit=10&country=JP").arg(term);

    QByteArray data = httpGet(url);
    if (data.isEmpty()) {
        qWarning() << "CoverArt[iTunes]: request failed";
        return {};
    }

    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonArray results = doc.object().value("results").toArray();
    qDebug() << "CoverArt[iTunes]: results count" << results.size();

    QString artUrl;
    for (const QJsonValue &rv : results) {
        QJsonObject obj = rv.toObject();
        QString colName = obj.value("collectionName").toString();
        QString url100  = obj.value("artworkUrl100").toString();
        if (!url100.isEmpty() &&
            (colName.contains(album, Qt::CaseInsensitive) ||
             album.contains(colName, Qt::CaseInsensitive))) {
            artUrl = url100;
            break;
        }
    }
    if (artUrl.isEmpty() && !results.isEmpty())
        artUrl = results[0].toObject().value("artworkUrl100").toString();

    if (!artUrl.isEmpty())
        artUrl.replace("100x100bb", "600x600bb");

    return artUrl;
}

// ============================================================
//  fetch()  : MusicBrainz → Discogs → iTunes の三段検索
// ============================================================
QPixmap CoverArt::fetch(const QString &artist, const QString &album)
{
    qDebug() << "CoverArt::fetch" << artist << album;

    QString artUrl;

    // --- 1. MusicBrainz + Cover Art Archive ---
    QString mbid = searchMusicBrainzReleaseId(artist, album);
    if (!mbid.isEmpty()) {
        artUrl = musicBrainzCoverUrl(mbid);
        // 画像が実際に存在するか確認（Cover Art Archiveはリダイレクト先が空の場合あり）
        QByteArray probe = httpGet(artUrl);
        if (probe.isEmpty()) {
            qWarning() << "CoverArt[MB]: image not available for MBID" << mbid;
            artUrl.clear();
        } else {
            // probe がそのまま画像データの場合（リダイレクト追跡済み）
            QPixmap pix;
            if (pix.loadFromData(probe)) {
                qDebug() << "CoverArt[MB]: success" << pix.size();
                return pix;
            }
            artUrl.clear();
        }
    }

    // --- 2. Discogs ---
    if (artUrl.isEmpty()) {
        artUrl = searchDiscogs(artist, album);
    }

    // --- 3. iTunes（最終保険）---
    if (artUrl.isEmpty()) {
        artUrl = searchITunes(artist, album);
    }

    if (artUrl.isEmpty()) {
        qWarning() << "CoverArt: not found in any source";
        return {};
    }

    qDebug() << "CoverArt: downloading" << artUrl;
    QByteArray imgData = httpGet(artUrl);
    if (imgData.isEmpty()) {
        qWarning() << "CoverArt: image download failed";
        return {};
    }

    QPixmap pix;
    pix.loadFromData(imgData);
    qDebug() << "CoverArt: success" << pix.size();
    return pix;
}
