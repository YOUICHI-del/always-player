#include "ArtistTheater.h"
#include <QLabel>
#include <QTextBrowser>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPainter>
#include <QLinearGradient>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QDesktopServices>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QPointer>
#include <QScrollBar>
#include <QResizeEvent>
#include <windows.h>
#include <winhttp.h>

namespace {

// ── HTTPS GET（WinHTTP）。200以外は空を返す。パスとクエリは符号化済みのまま送る。
QByteArray httpGet(const QString &urlStr)
{
    const QUrl url(urlStr);
    const QString host = url.host();
    QString path = url.path(QUrl::FullyEncoded);
    if (url.hasQuery()) path += "?" + url.query(QUrl::FullyEncoded);

    HINTERNET hSession = WinHttpOpen(L"AlwaysPlayer/10.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};
    WinHttpSetTimeouts(hSession, 5000, 5000, 8000, 8000);
    HINTERNET hConnect = WinHttpConnect(hSession, reinterpret_cast<LPCWSTR>(host.utf16()),
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", reinterpret_cast<LPCWSTR>(path.utf16()),
                                        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }
    // MusicBrainz・Wikimediaは、アプリ名と連絡先を名乗ることを求めている
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

// "https://ja.wikipedia.org/wiki/中森明菜" → lang="ja", title="中森明菜"
bool parseWikipediaUrl(const QString &u, QString &lang, QString &title)
{
    static const QRegularExpression re(R"(^https?://([a-z\-]+)\.wikipedia\.org/wiki/(.+)$)");
    const auto m = re.match(u);
    if (!m.hasMatch()) return false;
    lang  = m.captured(1);
    title = QUrl::fromPercentEncoding(m.captured(2).toUtf8()).replace('_', ' ');
    return true;
}

// MusicBrainz → Wikidata/Wikipedia で、各言語版の記事名を探す（lang→title）
QHash<QString, QString> findTitlesViaMusicBrainz(const QString &artist)
{
    QHash<QString, QString> titles;
    const QByteArray s = httpGet("https://musicbrainz.org/ws/2/artist/?query=artist:%22"
                                 + enc(artist) + "%22&fmt=json&limit=1");
    const QJsonArray arr = QJsonDocument::fromJson(s).object().value("artists").toArray();
    if (arr.isEmpty()) return titles;
    const QJsonObject a = arr.first().toObject();
    if (a.value("score").toInt() < 90) return titles;   // 別人の可能性が高い
    const QString mbid = a.value("id").toString();
    if (mbid.isEmpty()) return titles;
    Sleep(1100);   // MusicBrainzのルール：1秒に1回まで

    const QByteArray d = httpGet("https://musicbrainz.org/ws/2/artist/" + mbid + "?inc=url-rels&fmt=json");
    QString qid;
    for (const QJsonValue &rv : QJsonDocument::fromJson(d).object().value("relations").toArray()) {
        const QJsonObject r = rv.toObject();
        const QString type = r.value("type").toString();
        const QString res  = r.value("url").toObject().value("resource").toString();
        if (type == "wikidata") {
            qid = res.section('/', -1);
        } else if (type == "wikipedia") {
            QString lang, title;
            if (parseWikipediaUrl(res, lang, title)) titles.insert(lang, title);
        }
    }
    // 近年のMusicBrainzはWikipediaではなくWikidataへのリンクが基本。Wikidataから各言語版の記事名を得る
    if (!qid.isEmpty()) {
        const QByteArray w = httpGet("https://www.wikidata.org/w/api.php?action=wbgetentities&ids=" + qid
                                     + "&props=sitelinks&sitefilter=jawiki%7Cenwiki&format=json");
        const QJsonObject links = QJsonDocument::fromJson(w).object().value("entities").toObject()
                                      .value(qid).toObject().value("sitelinks").toObject();
        const QString ja = links.value("jawiki").toObject().value("title").toString();
        const QString en = links.value("enwiki").toObject().value("title").toString();
        if (!ja.isEmpty()) titles.insert("ja", ja);
        if (!en.isEmpty()) titles.insert("en", en);
    }
    return titles;
}

// 名前の比較用：全角→半角、大文字小文字・空白・中黒・補足の括弧を無視する
//   "ロバート・マクドナルド (ピアニスト)" → "ロバートマクドナルド"
QString normName(QString n)
{
    n = n.normalized(QString::NormalizationForm_KC);
    n.remove(QRegularExpression(R"(\s*\([^)]*\))"));
    n.remove(QRegularExpression(QStringLiteral("[\\s・･·=＝.\\-‐]")));
    return n.toCaseFolded();
}

// 記事が音楽家・バンドについてのものか（説明文と冒頭の要約で判断）
bool looksLikeMusic(const QJsonObject &sum)
{
    static const QStringList kWords = {
        "音楽", "歌手", "歌い手", "シンガー", "ミュージシャン", "アーティスト", "バンド", "ユニット",
        "グループ", "アイドル", "作曲", "作詞", "編曲", "演奏", "奏者", "ピアニスト", "ヴァイオリニスト",
        "バイオリニスト", "チェリスト", "ギタリスト", "ベーシスト", "ドラマー", "指揮者", "声楽",
        "ソプラノ", "メゾソプラノ", "アルト", "テノール", "バリトン", "バス歌手", "管弦楽団", "交響楽団",
        "オーケストラ", "楽団", "合唱", "ラッパー", "DJ", "声優", "プロデューサー", "レコード",
        "musician", "singer", "vocalist", "songwriter", "composer", "pianist", "violinist",
        "cellist", "violist", "guitarist", "bassist", "drummer", "conductor", "soprano",
        "mezzo", "tenor", "baritone", "orchestra", "ensemble", "quartet", "band", "duo",
        "trio", "rapper", "record producer", "music", "idol", "choir", "organist", "harpsichordist"
    };
    const QString text = sum.value("description").toString() + " "
                       + sum.value("extract").toString().left(400);
    for (const QString &w : kWords)
        if (text.contains(w, Qt::CaseInsensitive)) return true;
    return false;
}

QJsonObject restSummary(const QString &lang, const QString &title)
{
    return QJsonDocument::fromJson(httpGet("https://" + lang + ".wikipedia.org/api/rest_v1/page/summary/"
                                           + enc(QString(title).replace(' ', '_')))).object();
}

// ★ v10: Wikipediaの検索で記事名を探す（MusicBrainzで見つからなかったとき）。
//   検索の1件目をそのまま使うと、「ロバート・マクドナルド」→「マクドナルド」（ファストフード店）
//   のような誤りが起きる。記事名がアーティスト名と一致し、かつ音楽家の記事であるものだけを使う。
QString searchTitle(const QString &lang, const QString &artist)
{
    const QString want = normName(artist);
    if (want.isEmpty()) return {};
    const QByteArray s = httpGet("https://" + lang + ".wikipedia.org/w/api.php?action=query&list=search&srsearch="
                                 + enc(artist) + "&srlimit=10&format=json&formatversion=2");
    const QJsonArray arr = QJsonDocument::fromJson(s).object().value("query").toObject()
                               .value("search").toArray();
    int checked = 0;
    for (const QJsonValue &v : arr) {
        const QString title = v.toObject().value("title").toString();
        if (normName(title) != want) continue;          // 名前が違う記事は使わない
        if (++checked > 3) break;
        const QJsonObject sum = restSummary(lang, title);
        if (sum.value("type").toString() == QLatin1String("disambiguation")) continue;   // 曖昧さ回避
        if (looksLikeMusic(sum)) return title;
    }
    return {};
}

// Wikipedia REST API の要約から写真を取る（大きめのサムネイル。無ければ空）
QByteArray wikiImage(const QJsonObject &sum)
{
    const QString thumb = sum.value("thumbnail").toObject().value("source").toString();
    QString img = thumb;
    if (!img.isEmpty()) img.replace(QRegularExpression(R"(/(\d+)px-)"), "/640px-");
    if (img.isEmpty()) img = sum.value("originalimage").toObject().value("source").toString();
    if (img.isEmpty()) return {};
    QByteArray data = httpGet(img);
    if (data.isEmpty() && !thumb.isEmpty()) data = httpGet(thumb);   // 640pxが用意されていない画像
    return data;
}

// ★ v10: Deezerのアーティスト写真（登録不要の公開API）。日本の歌手などWikipediaに
//   写真が無いことが多いため、その代わりに使う。別人を避けるため、名前が一致したときだけ。
//   names：同じ人とみなす名前（元のアーティスト名、記事名、英語版の記事名など）。
//   日本のアーティストはDeezerではローマ字表記（例："Akina Nakamori"）のことがあるため。
QByteArray deezerImage(const QString &query, const QStringList &names)
{
    const QJsonArray arr = QJsonDocument::fromJson(
        httpGet("https://api.deezer.com/search/artist?q=" + enc(query) + "&limit=5"))
        .object().value("data").toArray();
    QStringList wants;
    for (QString n : names) {
        n.remove(QRegularExpression(R"(\s*\([^)]*\))"));   // "(singer)" などの補足を除く
        n = n.simplified().toCaseFolded();
        if (!n.isEmpty()) wants << n;
    }
    for (const QJsonValue &v : arr) {
        const QJsonObject a = v.toObject();
        if (!wants.contains(a.value("name").toString().simplified().toCaseFolded())) continue;
        const QString url = a.value("picture_xl").toString();
        // Deezerは写真の無いアーティストに共通の「空の人型」画像を返すので、それは使わない
        if (url.isEmpty() || url.contains(QLatin1String("/artist//"))) continue;
        return httpGet(url);
    }
    return {};
}

// 本文（プレーンテキスト、"== 見出し =="付き）をHTMLへ。脚注などの章以降は載せない。
QString bodyToHtml(const QString &text)
{
    static const QStringList kStop = {
        "脚注", "注釈", "出典", "参考文献", "関連項目", "外部リンク", "関連文献",
        "References", "Notes", "Footnotes", "See also", "External links", "Further reading",
        "Bibliography", "Sources", "Citations"
    };
    static const QRegularExpression head(R"(^(=+)\s*(.*?)\s*\1\s*$)");
    QString html;
    const QStringList lines = text.split('\n');
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        const auto m = head.match(line);
        if (m.hasMatch()) {
            const QString h = m.captured(2);
            if (kStop.contains(h, Qt::CaseInsensitive)) break;
            const int level = m.captured(1).size();
            html += QString("<p style='margin-top:18px; margin-bottom:4px; font-weight:bold; font-size:%1pt; color:#8ecbff;'>%2</p>")
                        .arg(level <= 2 ? 13 : 11).arg(h.toHtmlEscaped());
        } else {
            html += "<p style='margin-top:0; margin-bottom:10px; line-height:150%;'>" + line.toHtmlEscaped() + "</p>";
        }
    }
    return html;
}

} // namespace

ArtistInfoData ArtistTheater::fetch(const QString &artist, bool english)
{
    ArtistInfoData d;
    const QString pref  = english ? "en" : "ja";
    const QString other = english ? "ja" : "en";

    // ① 記事名を特定（画面の言語の版を優先）
    const QHash<QString, QString> titles = findTitlesViaMusicBrainz(artist);
    QString lang, title;
    if (titles.contains(pref))       { lang = pref;  title = titles.value(pref); }
    else if (titles.contains(other)) { lang = other; title = titles.value(other); }
    else {
        title = searchTitle(pref, artist);
        lang = pref;
        if (title.isEmpty()) { title = searchTitle(other, artist); lang = other; }
    }
    if (title.isEmpty()) return d;

    // ② 要約（写真・ひとこと紹介・記事URL）
    const QString restTitle = enc(QString(title).replace(' ', '_'));
    const QJsonObject sum = QJsonDocument::fromJson(
        httpGet("https://" + lang + ".wikipedia.org/api/rest_v1/page/summary/" + restTitle)).object();
    if (sum.isEmpty()) return d;
    d.name        = sum.value("title").toString(title);
    d.description = sum.value("description").toString();
    d.pageUrl     = sum.value("content_urls").toObject().value("desktop").toObject().value("page").toString();
    d.lang        = lang;
    const QString extractShort = sum.value("extract").toString();

    // ③ 本文（プレーンテキスト）
    const QJsonArray pages = QJsonDocument::fromJson(httpGet(
        "https://" + lang + ".wikipedia.org/w/api.php?action=query&prop=extracts&explaintext=1"
        "&exsectionformat=wiki&format=json&formatversion=2&titles=" + enc(title)))
        .object().value("query").toObject().value("pages").toArray();
    const QString full = pages.isEmpty() ? QString() : pages.first().toObject().value("extract").toString();
    d.bodyHtml = bodyToHtml(full.isEmpty() ? extractShort : full);

    // ④ 写真：この記事 → もう一方の言語版の記事 → Deezer の順に探す
    d.image = wikiImage(sum);
    if (!d.image.isEmpty()) d.imageSource = QStringLiteral("Wikimedia Commons");
    if (d.image.isEmpty()) {
        const QString otherLang = (lang == "ja") ? "en" : "ja";
        const QString otherTitle = titles.value(otherLang);
        if (!otherTitle.isEmpty()) {
            const QJsonObject sum2 = QJsonDocument::fromJson(httpGet(
                "https://" + otherLang + ".wikipedia.org/api/rest_v1/page/summary/"
                + enc(QString(otherTitle).replace(' ', '_')))).object();
            d.image = wikiImage(sum2);
            if (!d.image.isEmpty()) d.imageSource = QStringLiteral("Wikimedia Commons");
        }
    }
    if (d.image.isEmpty()) {
        const QString enTitle = titles.value("en");
        const QStringList names = { artist, d.name, enTitle };
        if (!enTitle.isEmpty())
            d.image = deezerImage(QString(enTitle).remove(QRegularExpression(R"(\s*\([^)]*\))")), names);
        if (d.image.isEmpty()) d.image = deezerImage(artist, names);
        if (!d.image.isEmpty()) d.imageSource = QStringLiteral("Deezer");
    }
    d.ok = !d.bodyHtml.isEmpty();
    return d;
}

ArtistTheater::ArtistTheater(QWidget *host)
    : QWidget(host), m_host(host)
{
    setAttribute(Qt::WA_NoSystemBackground, false);
    setFocusPolicy(Qt::StrongFocus);
    host->installEventFilter(this);

    // 中央のカード
    m_card = new QWidget(this);
    m_card->setObjectName("artistTheaterCard");
    m_card->setStyleSheet(
        "#artistTheaterCard { background: rgba(14,18,26,235); border: 1px solid #22344a; border-radius: 10px; }"
        "QLabel { color: #d8e2ee; background: transparent; }"
        "QTextBrowser { background: transparent; border: none; color: #c9d4e0; font-size: 11pt; }"
        "QPushButton#theaterChip { color: #9fc7ec; background: #121c28; border: 1px solid #2a4460;"
        "  border-radius: 12px; padding: 4px 12px; }"
        "QPushButton#theaterChip:checked { color: #ffffff; background: #1f4f7f; border-color: #4f94d4; }"
        "QPushButton#theaterClose { color: #9fb3c8; background: transparent; border: none; font-size: 16pt; }"
        "QPushButton#theaterClose:hover { color: #ffffff; }");

    auto *outer = new QVBoxLayout(m_card);
    outer->setContentsMargins(28, 18, 28, 16);
    outer->setSpacing(10);

    auto *top = new QHBoxLayout();
    m_chipBox = new QWidget(m_card);
    m_chipBox->setAttribute(Qt::WA_TranslucentBackground);
    m_chipBox->setStyleSheet("background: transparent;");
    m_chipLayout = new QHBoxLayout(m_chipBox);
    m_chipLayout->setContentsMargins(0, 0, 0, 0);
    m_chipLayout->setSpacing(6);
    top->addWidget(m_chipBox, 1);
    m_close = new QPushButton(QStringLiteral("✕"), m_card);
    m_close->setObjectName("theaterClose");
    m_close->setCursor(Qt::PointingHandCursor);
    m_close->setToolTip(QObject::tr("閉じる"));
    connect(m_close, &QPushButton::clicked, this, &QWidget::hide);
    top->addWidget(m_close, 0, Qt::AlignTop);
    outer->addLayout(top);

    auto *main = new QHBoxLayout();
    main->setSpacing(28);
    m_photo = new QLabel(m_card);
    m_photo->setFixedWidth(340);
    m_photo->setStyleSheet("background: transparent;");
    m_photo->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    main->addWidget(m_photo, 0, Qt::AlignTop);

    auto *right = new QVBoxLayout();
    right->setSpacing(6);
    m_name = new QLabel(m_card);
    m_name->setStyleSheet("font-size: 22pt; font-weight: bold; color: #ffffff;");
    m_name->setWordWrap(true);
    m_desc = new QLabel(m_card);
    m_desc->setStyleSheet("font-size: 11pt; color: #8fa6bd;");
    m_desc->setWordWrap(true);
    m_status = new QLabel(m_card);
    m_status->setStyleSheet("font-size: 11pt; color: #8fa6bd;");
    m_body = new QTextBrowser(m_card);
    m_body->setOpenLinks(false);
    m_body->setFrameShape(QFrame::NoFrame);
    right->addWidget(m_name);
    right->addWidget(m_desc);
    right->addWidget(m_status);
    right->addWidget(m_body, 1);
    main->addLayout(right, 1);
    outer->addLayout(main, 1);

    m_credit = new QLabel(m_card);
    m_credit->setStyleSheet("font-size: 8pt; color: #6d8298;");
    m_credit->setTextFormat(Qt::RichText);
    m_credit->setOpenExternalLinks(false);
    connect(m_credit, &QLabel::linkActivated, this, [](const QString &link) {
        QDesktopServices::openUrl(QUrl(link));   // 出典をクリックしたときだけブラウザを開く
    });
    outer->addWidget(m_credit);

    hide();
}

void ArtistTheater::showArtists(const QStringList &artists, bool english)
{
    if (artists.isEmpty()) return;
    m_english = english;

    // アーティストの切替ボタン（1人ならボタンは出さない）
    while (QLayoutItem *it = m_chipLayout->takeAt(0)) {
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    if (artists.size() > 1) {
        for (const QString &a : artists) {
            auto *chip = new QPushButton(a, m_chipBox);
            chip->setObjectName("theaterChip");
            chip->setCheckable(true);
            chip->setAutoExclusive(true);
            chip->setCursor(Qt::PointingHandCursor);
            chip->setChecked(a == artists.first());
            connect(chip, &QPushButton::clicked, this, [this, a] { load(a); });
            m_chipLayout->addWidget(chip);
        }
    }
    m_chipLayout->addStretch(1);
    m_chipBox->setVisible(artists.size() > 1);   // 1人のときは切替ボタンの欄を出さない

    setGeometry(m_host->rect());
    raise();
    show();
    setFocus();
    load(artists.first());
}

void ArtistTheater::load(const QString &artist)
{
    m_current = artist;
    const int serial = ++m_serial;
    const QString key = (m_english ? "en:" : "ja:") + artist;

    if (m_cache.contains(key)) { apply(artist, m_cache.value(key)); return; }

    m_name->setText(artist);
    m_desc->clear();
    m_desc->setVisible(true);
    m_body->clear();
    m_photo->clear();
    m_photo->setVisible(true);
    m_credit->clear();
    m_status->setText(QObject::tr("読み込み中…"));

    auto *watcher = new QFutureWatcher<ArtistInfoData>(this);
    QPointer<ArtistTheater> self(this);
    connect(watcher, &QFutureWatcher<ArtistInfoData>::finished, this, [self, watcher, artist, key, serial] {
        const ArtistInfoData d = watcher->result();
        watcher->deleteLater();
        if (!self) return;
        if (d.ok) self->m_cache.insert(key, d);
        if (serial == self->m_serial) self->apply(artist, d);   // 途中で別のアーティストに切り替えていたら捨てる
    });
    const bool english = m_english;
    watcher->setFuture(QtConcurrent::run([artist, english] { return fetch(artist, english); }));
}

void ArtistTheater::apply(const QString &artist, const ArtistInfoData &d)
{
    if (!d.ok) {
        m_name->setText(artist);
        m_desc->clear();
        m_body->clear();
        m_photo->clear();
        m_credit->clear();
        m_status->setText(QObject::tr("このアーティストの情報は見つかりませんでした。"));
        return;
    }
    m_status->clear();
    m_name->setText(d.name);
    m_desc->setText(d.description);
    m_desc->setVisible(!d.description.isEmpty());
    m_body->setHtml(d.bodyHtml);
    m_body->verticalScrollBar()->setValue(0);   // 本文は先頭から
    setPhoto(d.image);
    const QString link = QString("<a href='%1' style='color:#6d8298;'>%2</a>")
        .arg(d.pageUrl.toHtmlEscaped(), QObject::tr("Wikipedia「%1」").arg(d.name.toHtmlEscaped()));
    const QString img = d.imageSource.isEmpty() ? QString() : QObject::tr("画像：%1").arg(d.imageSource);
    m_credit->setText(QObject::tr("出典：%1").arg(QObject::tr("%1（CC BY-SA 4.0）　%2").arg(link, img)));
}

void ArtistTheater::setPhoto(const QByteArray &data)
{
    QPixmap pm;
    if (data.isEmpty() || !pm.loadFromData(data)) {
        m_photo->clear();
        m_photo->setVisible(false);   // 写真が無ければ左の欄をなくし、本文を広く
        return;
    }
    m_photo->setVisible(true);
    m_photo->setPixmap(pm.scaled(340, 460, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

bool ArtistTheater::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == m_host && ev->type() == QEvent::Resize && isVisible())
        setGeometry(m_host->rect());
    return QWidget::eventFilter(obj, ev);
}

void ArtistTheater::keyPressEvent(QKeyEvent *ev)
{
    if (ev->key() == Qt::Key_Escape) { hide(); return; }
    QWidget::keyPressEvent(ev);
}

void ArtistTheater::mousePressEvent(QMouseEvent *ev)
{
    // カードの外（暗い部分）をクリックしたら閉じる
    if (!m_card->geometry().contains(ev->pos())) { hide(); return; }
    QWidget::mousePressEvent(ev);
}

void ArtistTheater::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QLinearGradient g(0, 0, 0, height());
    g.setColorAt(0.0, QColor(4, 6, 10, 225));
    g.setColorAt(1.0, QColor(10, 16, 26, 240));
    p.fillRect(rect(), g);
}

void ArtistTheater::resizeEvent(QResizeEvent *ev)
{
    // カードはウィンドウの 90% × 86% の大きさで中央に置く
    const int w = qMin(width() - 40, qMax(640, width() * 90 / 100));
    const int h = qMin(height() - 40, qMax(420, height() * 86 / 100));
    m_card->setGeometry((width() - w) / 2, (height() - h) / 2, w, h);
    QWidget::resizeEvent(ev);
}
