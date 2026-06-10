#include "GnuDb.h"
#include <QDebug>
#include <QUrl>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

QByteArray GnuDb::httpGet(const QString &urlStr)
{
    QUrl qurl(urlStr);
    QString host     = qurl.host();
    QString fullPath = qurl.path();
    if (!qurl.query().isEmpty())
        fullPath += "?" + qurl.query();
    if (fullPath.isEmpty()) fullPath = "/";

    HINTERNET hSession = WinHttpOpen(
        L"AlwaysCDRipper/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    INTERNET_PORT port = (qurl.scheme() == "https")
        ? INTERNET_DEFAULT_HTTPS_PORT
        : INTERNET_DEFAULT_HTTP_PORT;

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        reinterpret_cast<LPCWSTR>(host.utf16()),
        port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }

    DWORD flags = (qurl.scheme() == "https") ? WINHTTP_FLAG_SECURE : 0;
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

    WinHttpAddRequestHeaders(hRequest,
        L"User-Agent: AlwaysCDRipper/1.0 ( https://github.com/YOUICHI-del/always-player )",
        (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

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

QString GnuDb::calcDiscId(const DiscInfo &disc)
{
    if (disc.tracks.isEmpty()) return {};

    quint32 n = 0;
    for (const auto &t : disc.tracks) {
        quint32 s = static_cast<quint32>(t.startSector) / 75;
        while (s > 0) {
            n += s % 10;
            s /= 10;
        }
    }

    quint32 totalSecs = static_cast<quint32>(disc.totalSectors) / 75;
    quint32 id = ((n % 255) << 24)
               | (totalSecs << 8)
               | static_cast<quint32>(disc.tracks.size());

    return QString("%1").arg(id, 8, 16, QChar('0'));
}

GnuDbResult GnuDb::parseEntry(const QString &entry)
{
    GnuDbResult result;
    QMap<int, QString> trackTitles;

    QStringList lines = entry.split('\n');
    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith('#')) continue;

        if (line.startsWith("DTITLE=")) {
            QString dtitle = line.mid(7).trimmed();
            int sep = dtitle.indexOf(" / ");
            if (sep >= 0) {
                result.artist = dtitle.left(sep).trimmed();
                result.album  = dtitle.mid(sep + 3).trimmed();
            } else {
                result.album = dtitle;
            }
        } else if (line.startsWith("DYEAR=")) {
            result.year = line.mid(6).trimmed();
        } else if (line.startsWith("DGENRE=")) {
            result.genre = line.mid(7).trimmed();
        } else if (line.startsWith("TTITLE")) {
            int eq = line.indexOf('=');
            if (eq > 6) {
                bool ok2;
                int num = line.mid(6, eq - 6).toInt(&ok2);
                if (ok2) {
                    QString title = line.mid(eq + 1).trimmed();
                    if (trackTitles.contains(num))
                        trackTitles[num] += title;
                    else
                        trackTitles[num] = title;
                }
            }
        }
    }

    QList<int> keys = trackTitles.keys();
    std::sort(keys.begin(), keys.end());
    for (int k : keys) {
        GnuDbTrack t;
        t.number = k + 1;
        t.title  = trackTitles[k];
        result.tracks.append(t);
    }

    qDebug() << "[GnuDB] parsed:" << result.artist << "/" << result.album
             << "year=" << result.year
             << "tracks=" << result.tracks.size();
    return result;
}

// cddb read ヘルパー
static QByteArray gnudbRead(const QString &category, const QString &discId)
{
    QString cmd = QString("cddb+read+%1+%2").arg(category, discId);
    QString url = QString(
        "http://gnudb.gnudb.org/~cddb/cddb.cgi"
        "?cmd=%1"
        "&hello=user+localhost+AlwaysCDRipper+1.0"
        "&proto=6").arg(cmd);
    qDebug() << "[GnuDB] read URL:" << url;
    return GnuDb::httpGet(url);
}

// TTITLEの数を数える
static int countTtitles(const QByteArray &data)
{
    int count = 0;
    QString str = QString::fromUtf8(data);
    for (const QString &line : str.split('\n')) {
        if (line.trimmed().startsWith("TTITLE"))
            count++;
    }
    return count;
}

GnuDbResult GnuDb::lookup(const DiscInfo &disc)
{
    GnuDbResult result;
    if (disc.tracks.isEmpty()) return result;

    QString discId    = calcDiscId(disc);
    int     ntrks     = disc.tracks.size();
    quint32 totalSecs = static_cast<quint32>(disc.totalSectors) / 75;

    qDebug() << "[GnuDB] DiscID:" << discId
             << "tracks:" << ntrks
             << "secs:" << totalSecs;

    QStringList offsets;
    for (const auto &t : disc.tracks)
        offsets << QString::number(static_cast<quint32>(t.startSector));

    // Step1: cddb query
    QString queryCmd = QString("cddb+query+%1+%2+%3+%4")
        .arg(discId).arg(ntrks).arg(offsets.join("+")).arg(totalSecs);

    QString queryUrl = QString(
        "http://gnudb.gnudb.org/~cddb/cddb.cgi"
        "?cmd=%1"
        "&hello=user+localhost+AlwaysCDRipper+1.0"
        "&proto=6").arg(queryCmd);

    qDebug() << "[GnuDB] query URL:" << queryUrl;
    QByteArray queryData = httpGet(queryUrl);
    if (queryData.isEmpty()) {
        qWarning() << "[GnuDB] query request failed";
        return result;
    }

    QString queryResp = QString::fromUtf8(queryData);
    qDebug() << "[GnuDB] query response:" << queryResp.left(200);

    QStringList respLines = queryResp.split('\n');
    if (respLines.isEmpty()) return result;

    QString firstLine = respLines[0].trimmed();
    int code = firstLine.left(3).toInt();

    QString category, foundDiscId;

    if (code == 200) {
        // 完全一致1件
        QStringList parts = firstLine.split(' ');
        if (parts.size() >= 3) {
            category    = parts[1];
            foundDiscId = parts[2];
        }

    } else if (code == 210 || code == 211) {
        // 複数件 → トラック数一致を優先
        struct Candidate { QString cat; QString id; };
        QVector<Candidate> candidates;
        for (int i = 1; i < respLines.size(); ++i) {
            QString line = respLines[i].trimmed();
            if (line == ".") break;
            QStringList parts = line.split(' ');
            if (parts.size() >= 2)
                candidates.append({parts[0], parts[1]});
        }
        qDebug() << "[GnuDB] candidates:" << candidates.size();

        QByteArray matchedData;
        for (const Candidate &c : candidates) {
            QByteArray rd = gnudbRead(c.cat, c.id);
            if (rd.isEmpty()) continue;
            int tc = countTtitles(rd);
            qDebug() << "[GnuDB] candidate" << c.cat << c.id
                     << "tracks=" << tc << "(need" << ntrks << ")";
            if (tc == ntrks) {
                category    = c.cat;
                foundDiscId = c.id;
                matchedData = rd;
                qDebug() << "[GnuDB] track count matched!";
                break;
            }
        }

        // 一致なければ先頭を使用
        if (category.isEmpty() && !candidates.isEmpty()) {
            category    = candidates[0].cat;
            foundDiscId = candidates[0].id;
            qDebug() << "[GnuDB] no track count match, using first candidate";
        }

        // マッチング時は既読データを再利用
        if (!matchedData.isEmpty()) {
            QString readResp = QString::fromUtf8(matchedData);
            int firstNl = readResp.indexOf('\n');
            if (firstNl >= 0) readResp = readResp.mid(firstNl + 1);
            result = parseEntry(readResp);
            return result;
        }

    } else {
        qWarning() << "[GnuDB] no match (code=" << code << ")";
        return result;
    }

    if (category.isEmpty() || foundDiscId.isEmpty()) return result;
    qDebug() << "[GnuDB] found: category=" << category << "discid=" << foundDiscId;

    // Step2: cddb read
    QByteArray readData = gnudbRead(category, foundDiscId);
    if (readData.isEmpty()) {
        qWarning() << "[GnuDB] read request failed";
        return result;
    }

    QString readResp = QString::fromUtf8(readData);
    int firstNl = readResp.indexOf('\n');
    if (firstNl >= 0) readResp = readResp.mid(firstNl + 1);

    result = parseEntry(readResp);
    return result;
}
