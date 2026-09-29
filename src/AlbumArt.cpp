#include "AlbumArt.h"
#include "IniFileLock.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <taglib/tag.h>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/tbytevectorstream.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/wavfile.h>
#include <taglib/aifffile.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4coverart.h>
#include <taglib/vorbisfile.h>
#include <taglib/opusfile.h>
#include <taglib/xiphcomment.h>

#include <windows.h>
#include <winhttp.h>
#include <memory>
#include <functional>

namespace {

QByteArray qba(const TagLib::ByteVector &bv) { return QByteArray(bv.data(), static_cast<int>(bv.size())); }
QString qs(const TagLib::String &s) { return QString::fromStdWString(s.toWString()).trimmed(); }

// ── ID3v2 の APIC から表紙を選ぶ（Front Cover → 説明に"front" → 最初の1枚）
QByteArray pickApic(TagLib::ID3v2::Tag *tag)
{
    if (!tag) return {};
    const auto &frames = tag->frameListMap()["APIC"];
    TagLib::ID3v2::AttachedPictureFrame *best = nullptr;
    for (auto *fr : frames) {
        auto *apic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame *>(fr);
        if (!apic || apic->picture().isEmpty()) continue;
        if (apic->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) return qba(apic->picture());
        if (!best || qs(apic->description()).contains(QLatin1String("front"), Qt::CaseInsensitive))
            best = apic;
    }
    return best ? qba(best->picture()) : QByteArray();
}

// ── FLAC形式の画像リスト（FLAC・OGG・Opus 共通）から表紙を選ぶ
QByteArray pickFlacPicture(const TagLib::List<TagLib::FLAC::Picture *> &list)
{
    TagLib::FLAC::Picture *best = nullptr;
    for (auto *p : list) {
        if (!p || p->data().isEmpty()) continue;
        if (p->type() == TagLib::FLAC::Picture::FrontCover) return qba(p->data());
        if (!best) best = p;
    }
    return best ? qba(best->data()) : QByteArray();
}

// ── DSF・DFF は ID3v2 タグを独自の場所に置く。その部分だけを取り出す。
//   DSF：先頭の "DSD " チャンクの 20バイト目（8バイト、リトルエンディアン）がタグの位置。
//   DFF：FRM8 の中の "ID3 " チャンク（サイズは8バイト、ビッグエンディアン）。
QByteArray dsdId3Bytes(const QString &path, const QString &ext)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const qint64 fileSize = f.size();
    const qint64 kMaxTag = 32LL * 1024 * 1024;
    if (ext == QLatin1String("dsf")) {
        const QByteArray h = f.read(28);
        if (h.size() < 28 || !h.startsWith("DSD ")) return {};
        quint64 off = 0;
        for (int i = 0; i < 8; ++i) off |= quint64(uchar(h[20 + i])) << (8 * i);
        if (off == 0 || qint64(off) >= fileSize) return {};
        if (!f.seek(qint64(off))) return {};
        return f.read(qMin(kMaxTag, fileSize - qint64(off)));
    }
    if (ext == QLatin1String("dff")) {
        const QByteArray h = f.read(16);
        if (h.size() < 16 || !h.startsWith("FRM8") || h.mid(12, 4) != "DSD ") return {};
        qint64 pos = 16;
        while (pos + 12 <= fileSize) {
            if (!f.seek(pos)) break;
            const QByteArray ch = f.read(12);
            if (ch.size() < 12) break;
            quint64 sz = 0;
            for (int i = 0; i < 8; ++i) sz = (sz << 8) | uchar(ch[4 + i]);
            if (ch.startsWith("ID3 ")) return f.read(qint64(qMin<quint64>(sz, quint64(kMaxTag))));
            pos += 12 + qint64(sz) + qint64(sz & 1);   // チャンクは偶数バイトにそろえてある
            if (sz > quint64(fileSize)) break;
        }
    }
    return {};
}

// ID3v2 のバイト列を読むため、MP3 ファイルとして開く（タグだけのメモリ上のファイル）
struct Id3Holder {
    TagLib::ByteVector bytes;
    std::unique_ptr<TagLib::ByteVectorStream> stream;
    std::unique_ptr<TagLib::MPEG::File> file;
    TagLib::ID3v2::Tag *tag() const { return file ? file->ID3v2Tag() : nullptr; }
};
bool openId3(const QByteArray &raw, Id3Holder &h)
{
    if (raw.size() < 10 || !raw.startsWith("ID3")) return false;
    h.bytes = TagLib::ByteVector(raw.constData(), static_cast<unsigned int>(raw.size()));
    h.stream.reset(new TagLib::ByteVectorStream(h.bytes));
#if TAGLIB_MAJOR_VERSION >= 2
    h.file.reset(new TagLib::MPEG::File(h.stream.get(), false));
#else
    h.file.reset(new TagLib::MPEG::File(h.stream.get(), TagLib::ID3v2::FrameFactory::instance(), false));
#endif
    return h.tag() != nullptr;
}

// ── 名前の比較用：全角→半角、大文字小文字・空白・記号・補足の括弧を無視する
//   "Abbey Road (Remastered 2019)" → "abbeyroad"
QString norm(QString n)
{
    n = n.normalized(QString::NormalizationForm_KC).toCaseFolded();
    static const QRegularExpression bracket(R"(\s*[\(\[【〔][^\)\]】〕]*[\)\]】〕])");
    n.remove(bracket);
    static const QRegularExpression punct(QStringLiteral(R"([\s\p{P}\p{S}]+)"));
    n.remove(punct);
    return n;
}

// アーティスト名の一致：どちらかがもう一方を含めばよい（"The Beatles" と "Beatles" など）
bool sameArtist(const QString &want, const QString &got)
{
    const QString a = norm(want), b = norm(got);
    if (a.isEmpty() || b.isEmpty()) return false;
    return a == b || (qMin(a.size(), b.size()) >= 2 && (a.contains(b) || b.contains(a)));
}

// アルバム名の一致：完全一致、または補足（Deluxe Edition など）の違いだけ
bool sameAlbum(const QString &want, const QString &got)
{
    const QString a = norm(want), b = norm(got);
    if (a.isEmpty() || b.isEmpty()) return false;
    if (a == b) return true;
    // "～ - Single" や "～ Deluxe Edition" のような、版の違いを表す付け足しだけは許す
    //   （"Best" と "Best of ○○" のような別のアルバムは一致させない）
    const QString &shorter = a.size() < b.size() ? a : b;
    const QString &longer  = a.size() < b.size() ? b : a;
    if (shorter.size() < 2 || !longer.startsWith(shorter)) return false;
    static const QRegularExpression edition(
        R"(^(single|ep|deluxe|edition|remaster|remastered|expanded|anniversary|bonus|version|special|complete|limited|stereo|mono|hires|hiresversion|\d+bit|\d+khz|盤|版|完全|限定|通常|初回|リマスター|デラックス|ハイレゾ|\d+周年|記念)+.*$)");
    return edition.match(longer.mid(shorter.size())).hasMatch();
}

// ── HTTPS GET（WinHTTP）。200以外は空。リダイレクトは WinHTTP が自動で追う。
QByteArray httpGet(const QString &urlStr)
{
    const QUrl url(urlStr);
    const QString host = url.host();
    QString path = url.path(QUrl::FullyEncoded);
    if (url.hasQuery()) path += "?" + url.query(QUrl::FullyEncoded);

    HINTERNET hSession = WinHttpOpen(L"AlwaysPlayer/10.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};
    WinHttpSetTimeouts(hSession, 5000, 5000, 8000, 10000);
    HINTERNET hConnect = WinHttpConnect(hSession, reinterpret_cast<LPCWSTR>(host.utf16()),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", reinterpret_cast<LPCWSTR>(path.utf16()),
                                        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }
    WinHttpAddRequestHeaders(hReq,
        L"User-Agent: AlwaysPlayer/10.0 ( https://github.com/YOUICHI-del/always-player )",
        static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD);

    QByteArray data;
    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            DWORD size = 0;
            do {
                if (!WinHttpQueryDataAvailable(hReq, &size) || size == 0) break;
                QByteArray buf(static_cast<int>(size), 0);
                DWORD got = 0;
                if (!WinHttpReadData(hReq, buf.data(), size, &got)) break;
                data.append(buf.constData(), static_cast<int>(got));
            } while (size > 0 && data.size() < 20 * 1024 * 1024);
        }
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return data;
}

QString enc(const QString &s) { return QString::fromUtf8(QUrl::toPercentEncoding(s)); }

bool isImage(const QByteArray &d)
{
    return d.startsWith("\xFF\xD8\xFF") || d.startsWith("\x89PNG") || d.startsWith("GIF8")
        || (d.startsWith("RIFF") && d.mid(8, 4) == "WEBP") || d.startsWith("BM");
}

// MusicBrainz のルール：1秒に1回まで（アプリ全体で間隔をあける）
void musicBrainzWait()
{
    static QMutex m;
    static QElapsedTimer last;
    QMutexLocker lock(&m);
    if (last.isValid() && last.elapsed() < 1100) QThread::msleep(quint64(1100 - last.elapsed()));
    last.start();
}

// iTunes Search API（登録不要）。日本の作品が多い JP ストアを先に、無ければ US ストア。
QByteArray fromITunes(const AlbumArt::Tags &t)
{
    for (const char *country : {"JP", "US"}) {
        const QByteArray s = httpGet("https://itunes.apple.com/search?term=" + enc(t.artist + " " + t.album)
                                     + "&entity=album&limit=25&country=" + QLatin1String(country));
        const QJsonArray arr = QJsonDocument::fromJson(s).object().value("results").toArray();
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            if (!sameAlbum(t.album, o.value("collectionName").toString())) continue;
            if (!sameArtist(t.artist, o.value("artistName").toString())) continue;
            QString url = o.value("artworkUrl100").toString();
            if (url.isEmpty()) continue;
            QString big = url;
            big.replace(QLatin1String("100x100bb"), QLatin1String("1000x1000bb"));
            QByteArray img = httpGet(big);
            if (!isImage(img)) img = httpGet(url.replace(QLatin1String("100x100bb"), QLatin1String("600x600bb")));
            if (isImage(img)) return img;
        }
    }
    return {};
}

// MusicBrainz でアルバム（リリースグループ）を探し、Cover Art Archive の表紙を取る
QByteArray fromMusicBrainz(const AlbumArt::Tags &t)
{
    const QString q = QStringLiteral("releasegroup:\"%1\" AND artist:\"%2\"")
                          .arg(QString(t.album).remove('"'), QString(t.artist).remove('"'));
    musicBrainzWait();
    const QByteArray s = httpGet("https://musicbrainz.org/ws/2/release-group/?query=" + enc(q)
                                 + "&fmt=json&limit=5");
    const QJsonArray arr = QJsonDocument::fromJson(s).object().value("release-groups").toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        if (o.value("score").toInt() < 90) continue;
        if (!sameAlbum(t.album, o.value("title").toString())) continue;
        QString credit;
        for (const QJsonValue &c : o.value("artist-credit").toArray())
            credit += c.toObject().value("name").toString() + c.toObject().value("joinphrase").toString();
        if (!sameArtist(t.artist, credit)) continue;
        const QString id = o.value("id").toString();
        if (id.isEmpty()) continue;
        const QByteArray img = httpGet("https://coverartarchive.org/release-group/" + id + "/front-500");
        if (isImage(img)) return img;
    }
    return {};
}

QString cacheDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base + QStringLiteral("/covers");
}

QString cacheBase(const AlbumArt::Tags &t)
{
    if (t.album.isEmpty()) return {};
    const QByteArray key = (norm(t.artist) + QLatin1Char('|') + norm(t.album)).toUtf8();
    return cacheDir() + QLatin1Char('/')
         + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());
}

} // namespace

namespace AlbumArt {

QByteArray embedded(const QString &audioFile)
{
    if (audioFile.isEmpty() || audioFile.startsWith(QLatin1String("cdda://"))) return {};
    const QString ext = QFileInfo(audioFile).suffix().toLower();
    const std::wstring w = audioFile.toStdWString();
    const wchar_t *p = w.c_str();

    if (ext == "mp3") {
        TagLib::MPEG::File f(p);
        return pickApic(f.ID3v2Tag());
    }
    if (ext == "wav") {
        TagLib::RIFF::WAV::File f(p);
        return pickApic(f.ID3v2Tag());
    }
    if (ext == "aif" || ext == "aiff" || ext == "aifc") {
        TagLib::RIFF::AIFF::File f(p);
        return pickApic(f.tag());
    }
    if (ext == "flac") {
        TagLib::FLAC::File f(p);
        QByteArray d = pickFlacPicture(f.pictureList());
        if (d.isEmpty()) d = pickApic(f.ID3v2Tag());
        return d;
    }
    if (ext == "m4a" || ext == "mp4" || ext == "aac" || ext == "alac") {
        TagLib::MP4::File f(p);
        if (f.tag() && f.tag()->contains("covr")) {
            const auto covers = f.tag()->item("covr").toCoverArtList();
            for (const auto &c : covers)
                if (!c.data().isEmpty()) return qba(c.data());
        }
        return {};
    }
    if (ext == "ogg" || ext == "oga") {
        TagLib::Ogg::Vorbis::File f(p);
        return f.tag() ? pickFlacPicture(f.tag()->pictureList()) : QByteArray();
    }
    if (ext == "opus") {
        TagLib::Ogg::Opus::File f(p);
        return f.tag() ? pickFlacPicture(f.tag()->pictureList()) : QByteArray();
    }
    if (ext == "dsf" || ext == "dff") {
        Id3Holder h;
        if (openId3(dsdId3Bytes(audioFile, ext), h)) return pickApic(h.tag());
    }
    return {};
}

QString folderImage(const QString &dirPath)
{
    if (dirPath.isEmpty()) return {};
    QDir dir(dirPath);
    // よく使われる名前を優先（Windows は大文字小文字を区別しないので小文字だけでよい）
    static const QStringList names = {
        "cover", "folder", "front", "artwork", "albumart", "album", "jacket", "thumb", "image"
    };
    static const QStringList exts = { "jpg", "jpeg", "png", "webp", "bmp" };
    for (const QString &n : names)
        for (const QString &e : exts) {
            const QString path = dir.filePath(n + '.' + e);
            if (QFileInfo::exists(path)) return path;
        }
    // 決まった名前が無ければ、フォルダ内の最初の画像（"back" などの裏面は後回し）
    const QFileInfoList list = dir.entryInfoList(
        {"*.jpg", "*.jpeg", "*.png", "*.webp", "*.bmp", "*.gif", "*.tif", "*.tiff"},
        QDir::Files, QDir::Name | QDir::IgnoreCase);
    static const QRegularExpression backish(R"(back|rear|inlay|cd\d*|disc|obi|booklet)",
                                            QRegularExpression::CaseInsensitiveOption);
    for (const QFileInfo &fi : list)
        if (!backish.match(fi.completeBaseName()).hasMatch()) return fi.absoluteFilePath();
    return list.isEmpty() ? QString() : list.first().absoluteFilePath();
}

QString firstAudioIn(const QString &dirPath)
{
    static const QStringList audio = {
        "*.mp3", "*.flac", "*.m4a", "*.mp4", "*.aac", "*.wav", "*.aif", "*.aiff",
        "*.ogg", "*.opus", "*.dsf", "*.dff"
    };
    std::function<QString(const QString &, int)> find = [&](const QString &d, int depth) -> QString {
        if (depth > 2) return {};
        QDir dir(d);
        const QFileInfoList files = dir.entryInfoList(audio, QDir::Files, QDir::Name | QDir::IgnoreCase);
        if (!files.isEmpty()) return files.first().absoluteFilePath();
        for (const QFileInfo &sub : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString f = find(sub.absoluteFilePath(), depth + 1);
            if (!f.isEmpty()) return f;
        }
        return {};
    };
    return find(dirPath, 0);
}

Tags readTags(const QString &audioFile)
{
    Tags t;
    if (audioFile.isEmpty() || audioFile.startsWith(QLatin1String("cdda://"))) return t;
    const QString ext = QFileInfo(audioFile).suffix().toLower();
    if (ext == "dsf" || ext == "dff") {
        Id3Holder h;
        if (openId3(dsdId3Bytes(audioFile, ext), h)) {
            TagLib::ID3v2::Tag *tag = h.tag();
            t.album  = qs(tag->album());
            const auto tpe2 = tag->frameListMap()["TPE2"];
            if (!tpe2.isEmpty()) t.artist = qs(tpe2.front()->toString());
            if (t.artist.isEmpty()) t.artist = qs(tag->artist());
        }
        return t;
    }
    TagLib::FileRef f(audioFile.toStdWString().c_str());
    if (f.isNull() || !f.tag()) return t;
    t.album = qs(f.tag()->album());
    const TagLib::PropertyMap props = f.file()->properties();
    const auto it = props.find("ALBUMARTIST");
    if (it != props.end() && !it->second.isEmpty()) t.artist = qs(it->second.front());
    if (t.artist.isEmpty()) t.artist = qs(f.tag()->artist());
    return t;
}

QString cachedFor(const Tags &t)
{
    const QString base = cacheBase(t);
    if (base.isEmpty()) return {};
    for (const char *e : {".jpg", ".png", ".webp", ".gif", ".bmp"}) {
        const QString path = base + QLatin1String(e);
        if (QFileInfo::exists(path)) return path;
    }
    return {};
}

QString cachedFor(const QString &audioFile)
{
    return cachedFor(readTags(audioFile));
}

QString extensionFor(const QByteArray &d)
{
    if (d.startsWith("\x89PNG")) return QStringLiteral(".png");
    if (d.startsWith("GIF8")) return QStringLiteral(".gif");
    if (d.startsWith("RIFF") && d.mid(8, 4) == "WEBP") return QStringLiteral(".webp");
    if (d.startsWith("BM")) return QStringLiteral(".bmp");
    return QStringLiteral(".jpg");
}

QString saveToCache(const Tags &t, const QByteArray &image)
{
    const QString base = cacheBase(t);
    if (base.isEmpty() || !isImage(image)) return {};
    QDir().mkpath(cacheDir());
    const QString path = base + extensionFor(image);
    const QString tmp = path + QStringLiteral(".part");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly)) return {};
    const bool ok = f.write(image) == image.size();
    f.close();
    if (!ok) { QFile::remove(tmp); return {}; }
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) { QFile::remove(tmp); return {}; }
    return path;
}

QByteArray fetchOnline(const Tags &t)
{
    if (t.album.isEmpty() || t.artist.isEmpty()) return {};   // 名前が分からないと、別のアルバムを拾うおそれ
    QByteArray img = fromITunes(t);
    if (img.isEmpty()) img = fromMusicBrainz(t);
    return img;
}

bool onlineEnabled()
{
    return iniReadValue(QStringLiteral("ui"), QStringLiteral("cover_online")) != QLatin1String("0");
}

void setOnlineEnabled(bool on)
{
    iniWriteValue(QStringLiteral("ui"), QStringLiteral("cover_online"),
                  on ? QStringLiteral("1") : QStringLiteral("0"));
}

} // namespace AlbumArt
