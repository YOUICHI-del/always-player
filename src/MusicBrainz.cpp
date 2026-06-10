#include "MusicBrainz.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QUrl>
#include <QDebug>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// ============================================================
//  共有 HTTP GET（リダイレクト追跡あり）
// ============================================================
QByteArray MusicBrainz::httpGet(const QString &urlStr)
{
    QUrl qurl(urlStr);
    QString host     = qurl.host();
    QString fullPath = qurl.path();
    if (!qurl.query().isEmpty())
        fullPath += "?" + qurl.query();
    if (fullPath.isEmpty()) fullPath = "/";

    qDebug() << "[HTTP] host=" << host << "path=" << fullPath;

    HINTERNET hSession = WinHttpOpen(
        L"AlwaysCDRipper/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { qWarning() << "[HTTP] WinHttpOpen failed"; return {}; }

    INTERNET_PORT port = (qurl.scheme() == "https")
        ? INTERNET_DEFAULT_HTTPS_PORT
        : INTERNET_DEFAULT_HTTP_PORT;

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        reinterpret_cast<LPCWSTR>(host.utf16()),
        port, 0);
    if (!hConnect) {
        qWarning() << "[HTTP] WinHttpConnect failed";
        WinHttpCloseHandle(hSession);
        return {};
    }

    DWORD flags = (qurl.scheme() == "https") ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET",
        reinterpret_cast<LPCWSTR>(fullPath.utf16()),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        qWarning() << "[HTTP] WinHttpOpenRequest failed";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // MusicBrainz 必須 User-Agent
    WinHttpAddRequestHeaders(hRequest,
        L"User-Agent: AlwaysCDRipper/1.0 ( https://github.com/YOUICHI-del/always-player )",
        (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    // リダイレクト自動追跡（307対応）
    DWORD redirPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirPolicy, sizeof(redirPolicy));

    BOOL ok = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);

    if (!ok) {
        DWORD err = GetLastError();
        qWarning() << "[HTTP] request failed, err=" << err;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    qDebug() << "[HTTP] status=" << statusCode;

    QByteArray result;
    if (ok && statusCode == 200) {
        DWORD size = 0;
        do {
            size = 0;
            WinHttpQueryDataAvailable(hRequest, &size);
            if (size == 0) break;
            QByteArray buf(static_cast<int>(size), '\0');
            DWORD downloaded = 0;
            WinHttpReadData(hRequest, buf.data(), size, &downloaded);
            result.append(buf.left(static_cast<int>(downloaded)));
        } while (size > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

// ============================================================
//  TOC 文字列生成（fuzzy lookup 用）
// ============================================================
static QString calcToc(const DiscInfo &disc)
{
    int first       = disc.tracks.first().number;
    int last        = disc.tracks.last().number;
    quint32 leadout = static_cast<quint32>(disc.totalSectors + 150);

    QStringList parts;
    parts << QString::number(first)
          << QString::number(last)
          << QString::number(leadout);
    for (const auto &t : disc.tracks)
        parts << QString::number(static_cast<quint32>(t.startSector + 150));

    QString toc = parts.join(" ");
    qDebug() << "[MB] TOC:" << toc;
    return toc;
}

// ============================================================
//  DiscID 計算
// ============================================================
QString MusicBrainz::calcDiscId(const DiscInfo &disc)
{
    if (disc.tracks.isEmpty()) return {};

    int first       = disc.tracks.first().number;
    int last        = disc.tracks.last().number;
    quint32 leadout = static_cast<quint32>(disc.totalSectors + 150);

    qDebug() << "[MB] DiscID calc: first=" << first
             << "last=" << last << "leadout=" << leadout;
    for (int i = 0; i < disc.tracks.size(); ++i)
        qDebug() << "  track" << disc.tracks[i].number
                 << "offset=" << (disc.tracks[i].startSector + 150);

    QByteArray input;
    input += QString("%1").arg(first,   2, 16, QChar('0')).toUpper().toLatin1();
    input += QString("%1").arg(last,    2, 16, QChar('0')).toUpper().toLatin1();
    input += QString("%1").arg(leadout, 8, 16, QChar('0')).toUpper().toLatin1();
    for (int i = 0; i < 99; ++i) {
        quint32 offset = 0;
        if (i < disc.tracks.size())
            offset = static_cast<quint32>(disc.tracks[i].startSector + 150);
        input += QString("%1").arg(offset, 8, 16, QChar('0')).toUpper().toLatin1();
    }

    QByteArray sha1 = QCryptographicHash::hash(input, QCryptographicHash::Sha1);
    QString b64 = sha1.toBase64();
    b64.replace('+', '.');
    b64.replace('/', '_');
    b64.replace('=', '-');

    qDebug() << "[MB] DiscID:" << b64;
    return b64;
}

// ============================================================
//  JSON → MbRelease パース（共通化）
// ============================================================
static MbRelease parseRelease(const QByteArray &data)
{
    MbRelease result;
    if (data.isEmpty()) return result;

    QJsonDocument doc  = QJsonDocument::fromJson(data);
    QJsonObject   root = doc.object();

    if (root.contains("error")) {
        qWarning() << "[MB] error:" << root.value("error").toString();
        return result;
    }

    QJsonArray releases = root.value("releases").toArray();
    if (releases.isEmpty()) {
        qWarning() << "[MB] no releases found";
        return result;
    }

    QJsonObject rel  = releases[0].toObject();
    result.title     = rel.value("title").toString();
    result.date      = rel.value("date").toString().left(4);
    result.mbid      = rel.value("id").toString();

    QJsonArray credits = rel.value("artist-credit").toArray();
    if (!credits.isEmpty())
        result.artist = credits[0].toObject()
            .value("artist").toObject()
            .value("name").toString();

    qDebug() << "[MB] found:" << result.title << "/" << result.artist
             << "mbid=" << result.mbid;

    QJsonArray media = rel.value("media").toArray();
    result.totalDiscs = media.size();
    for (const QJsonValue &mv : media) {
        QJsonObject mObj = mv.toObject();
        MbDisc d;
        d.position = mObj.value("position").toInt();

        QJsonArray tracks = mObj.value("tracks").toArray();
        for (const QJsonValue &tv : tracks) {
            MbTrack t;
            t.number     = tv.toObject().value("number").toString().toInt();
            t.title      = tv.toObject().value("title").toString();
            t.discNumber = d.position;
            d.tracks.append(t);
        }
        result.discs.append(d);

        if (d.position == 1)
            result.tracks = d.tracks;
    }

    return result;
}

// ============================================================
//  MusicBrainz lookup -- DiscID 完全一致のみ
//  TOC fuzzy lookup は別CDが誤ヒットするため廃止
// ============================================================
MbRelease MusicBrainz::lookup(const DiscInfo &disc)
{
    MbRelease result;
    if (disc.tracks.isEmpty()) return result;

    QString discId = calcDiscId(disc);
    if (discId.isEmpty()) return result;

    QString url = QString(
        "https://musicbrainz.org/ws/2/discid/%1"
        "?inc=recordings+artists&fmt=json&cdstubs=no").arg(discId);
    qDebug() << "[MB] DiscID URL:" << url;
    QByteArray data = httpGet(url);
    qDebug() << "[MB] DiscID response:" << data.left(300);

    result = parseRelease(data);
    if (!result.title.isEmpty())
        qDebug() << "[MB] DiscID hit:" << result.title;
    else
        qWarning() << "[MB] DiscID miss -> iTunes fallback will handle";

    return result;
}

// ============================================================
//  アルバムアート取得（Cover Art Archive）
//  戻り値：JPEG/PNG バイナリ。失敗時は空
// ============================================================
QByteArray MusicBrainz::fetchCoverArt(const QString &mbid)
{
    if (mbid.isEmpty()) {
        qWarning() << "[CAA] mbid is empty";
        return {};
    }

    // Cover Art Archive は mbid/front を GET すると画像 URL へリダイレクトされる
    // httpGet はリダイレクト追跡済みなのでそのまま使える
    QString url = QString(
        "https://coverartarchive.org/release/%1/front").arg(mbid);
    qDebug() << "[CAA] fetching:" << url;

    QByteArray data = httpGet(url);
    qDebug() << "[CAA] received bytes:" << data.size();

    // 画像データの簡易バリデーション（JPEG or PNG）
    bool isJpeg = data.startsWith("\xFF\xD8\xFF");
    bool isPng  = data.startsWith("\x89PNG");
    if (!isJpeg && !isPng) {
        qWarning() << "[CAA] unexpected data (not JPEG/PNG), size=" << data.size();
        // デバッグ用：先頭を表示
        qDebug() << "[CAA] head:" << data.left(100);
        return {};
    }

    qDebug() << "[CAA] OK, format=" << (isJpeg ? "JPEG" : "PNG");
    return data;
}
