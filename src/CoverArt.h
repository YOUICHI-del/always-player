#pragma once
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QByteArray>

// iTunes から取得できるアルバム情報
struct iTunesAlbumInfo {
    QString     artist;
    QString     album;
    QString     year;
    QString     artUrl;    // アートワークURL（600x600）
    QStringList tracks;    // トラック名リスト（曲順）
    bool isValid() const { return !album.isEmpty(); }
};

class CoverArt
{
public:
    void setDiscogsToken(const QString &token) { discogsToken = token; }
    QString getDiscogsToken() const { return discogsToken; }

    // メイン：アーティスト名＋アルバム名からカバーアートを取得
    QPixmap fetch(const QString &artist, const QString &album);

    // ★ 新規：iTunes からアルバム情報（曲名含む）を一括取得
    iTunesAlbumInfo searchITunesFull(const QString &artist, const QString &album);

    // 個別検索（後方互換）
    QString searchITunes(const QString &artist, const QString &album);
    QString searchDiscogs(const QString &artist, const QString &album);
    QString searchMusicBrainzReleaseId(const QString &artist, const QString &album);
    QString musicBrainzCoverUrl(const QString &mbid);

private:
    QString discogsToken;
    QByteArray httpGet(const QString &url);
};
