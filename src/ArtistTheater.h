#pragma once
// ─────────────────────────────────────────────────────────
// ArtistTheater
//
// ★ v10: アーティスト情報の「シアター画面」。ブラウザを使わず、Always Playerの
//   メイン画面の上に暗い背景で重ねて、アーティストの写真・ひとこと紹介・
//   Wikipediaの本文を表示する（音楽は鳴り続ける）。
//
// ・記事の特定：MusicBrainz（アーティスト→WikidataまたはWikipediaのリンク）→
//   Wikidata（各言語版の記事名）。見つからなければWikipediaの検索で探す。
// ・表示：Wikipedia REST API の要約（写真・ひとこと紹介）と、本文のテキスト
//   （脚注・参考文献・外部リンクなどの章は除く）。
// ・言語：画面の言語（日本語／英語）の版を優先し、無ければもう一方の版。
// ・ライセンス：Wikipediaの文章はCC BY-SA 4.0。出典（記事名とリンク）を表示する。
//   出典をクリックしたときだけブラウザが開く。
// ・一度調べたアーティストは、起動中はメモリに覚えておく（再表示は即時）。
// ─────────────────────────────────────────────────────────
#include <QWidget>
#include <QHash>
#include <QByteArray>
#include <QStringList>

class QLabel;
class QTextBrowser;
class QPushButton;
class QHBoxLayout;

struct ArtistInfoData {
    bool       ok = false;
    QString    name;          // 記事名
    QString    description;   // ひとこと紹介（例：「日本の歌手」）
    QString    bodyHtml;      // 本文（HTML化済み）
    QString    pageUrl;       // 記事のURL（出典）
    QString    lang;          // "ja" / "en"
    QByteArray image;         // 写真（無ければ空）
    QString    imageSource;   // 写真の出典（"Wikimedia Commons" / "Deezer"）
};

class ArtistTheater : public QWidget {
    Q_OBJECT
public:
    explicit ArtistTheater(QWidget *host);
    // v10: リモコン(スマホ)からも同じ方法で調べるため公開。ネット通信するので別スレッドで呼ぶこと
    static ArtistInfoData fetch(const QString &artist, bool english);
    // artists：表示するアーティスト（複数なら上に切替ボタンを並べる）
    void showArtists(const QStringList &artists, bool english);

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void keyPressEvent(QKeyEvent *ev) override;
    void paintEvent(QPaintEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;

private:
    void load(const QString &artist);
    void apply(const QString &artist, const ArtistInfoData &d);
    void setPhoto(const QByteArray &data);

    QWidget      *m_host = nullptr;
    QWidget      *m_card = nullptr;
    QWidget      *m_chipBox = nullptr;
    QHBoxLayout  *m_chipLayout = nullptr;
    QLabel       *m_photo = nullptr;
    QLabel       *m_name = nullptr;
    QLabel       *m_desc = nullptr;
    QLabel       *m_credit = nullptr;
    QLabel       *m_status = nullptr;
    QTextBrowser *m_body = nullptr;
    QPushButton  *m_close = nullptr;

    QHash<QString, ArtistInfoData> m_cache;   // キー：言語 + アーティスト名
    bool    m_english = false;
    QString m_current;
    int     m_serial = 0;
};
