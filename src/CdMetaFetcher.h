#pragma once
#include <QObject>
#include <QPixmap>
#include <QStringList>
#include "CdDrive.h"

// CD挿入時にバックグラウンドでメタデータ・アルバムアートを取得し、
// 結果をシグナルで通知する。
// 呼び出し方:
//   auto *f = new CdMetaFetcher(this);
//   connect(f, &CdMetaFetcher::metaReady, this, &MainWindow::onCdMetaReady);
//   f->fetchAsync(m_discInfo);
class CdMetaFetcher : public QObject
{
    Q_OBJECT
public:
    struct Result {
        QString     albumTitle;
        QString     artist;
        QString     year;
        QStringList trackNames;   // index 0 = Track 1
        QPixmap     coverArt;
        bool        found = false;
    };

    explicit CdMetaFetcher(QObject *parent = nullptr);

    // TOC情報を渡してバックグラウンド検索開始
    // 結果は metaReady() シグナルで返る（UIスレッドへ）
    void fetchAsync(const DiscInfo &disc);

signals:
    void metaReady(CdMetaFetcher::Result result);

private:
    // ワーカー（QtConcurrent::run から呼ばれる）
    static Result doFetch(DiscInfo disc);
};
