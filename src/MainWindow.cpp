#include "MainWindow.h"
#include <QPainter>
#include "CdMetaFetcher.h"
#include <mpv/client.h>
#include <QRadioButton>
#include <QCheckBox>
#include <QGroupBox>
#include <QTextStream>
#include <QActionGroup>
#include <QMenu>
#include <QAction>
#include <QApplication>
#include <QThread>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QCloseEvent>
#include <QPixmap>
#include <QIcon>
#include <QMessageBox>
#include <QDialog>
#include <QRadioButton>
#include <QButtonGroup>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QProcess>
#include <QDialog>
#include <QDebug>
#include <QScrollArea>
#include <QDesktopServices>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <windows.h>
#include <mmsystem.h>  // MCI CD再生
#include <shellapi.h>
#include <powrprof.h>
#include <winreg.h>
#include <winhttp.h>
#include <QFrame>
#include <algorithm>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>

static QString jp(const char *utf8) { return QString::fromUtf8(utf8); }

// Wikipedia フォールバック URL（英語版をGoogle翻訳経由で検索）
static QString fallbackUrl(const QString &artist)
{
    QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(artist));
    return QString(
        "https://en-m-wikipedia-org.translate.goog/w/index.php"
        "?search=%1&_x_tr_sl=en&_x_tr_tl=ja&_x_tr_hl=ja").arg(encoded);
}

// WinHTTPでGETリクエスト
static QByteArray winHttpGet(const QString &urlStr)
{
    QUrl qurl(urlStr);
    QString host = qurl.host();
    QString path = qurl.path() + "?" + qurl.query();

    HINTERNET hSession = WinHttpOpen(L"AlwaysPlayer/5.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    HINTERNET hConnect = WinHttpConnect(hSession,
        reinterpret_cast<LPCWSTR>(host.utf16()),
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }

    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET",
        reinterpret_cast<LPCWSTR>(path.utf16()),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    WinHttpAddRequestHeaders(hReq,
        L"User-Agent: AlwaysPlayer/5.0 ( https://github.com/YOUICHI-del/always-player )",
        (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS,
        0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);

    QByteArray data;
    if (ok) {
        DWORD size = 0;
        do {
            WinHttpQueryDataAvailable(hReq, &size);
            if (!size) break;
            QByteArray buf(static_cast<int>(size), 0);
            DWORD downloaded = 0;
            WinHttpReadData(hReq, buf.data(), size, &downloaded);
            data.append(buf.left(static_cast<int>(downloaded)));
        } while (size > 0);
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return data;
}

// アーティスト名からWikipedia URLを取得（MusicBrainz経由）
static QString searchWikipediaUrl(const QString &artist)
{
    // ① MusicBrainzでアーティスト検索
    QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(artist));
    QString searchUrl = QString(
        "https://musicbrainz.org/ws/2/artist/"
        "?query=artist:%1&fmt=json&limit=1").arg(encoded);

    QByteArray data = winHttpGet(searchUrl);
    if (data.isEmpty()) return fallbackUrl(artist);

    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonArray artists = doc.object().value("artists").toArray();
    if (artists.isEmpty()) return fallbackUrl(artist);

    QString mbid = artists[0].toObject().value("id").toString();
    if (mbid.isEmpty()) return fallbackUrl(artist);

    // ② MBIDでWikipedia URLを取得
    QString detailUrl = QString(
        "https://musicbrainz.org/ws/2/artist/%1"
        "?inc=url-rels&fmt=json").arg(mbid);

    QByteArray data2 = winHttpGet(detailUrl);
    if (data2.isEmpty()) return fallbackUrl(artist);

    QJsonDocument doc2 = QJsonDocument::fromJson(data2);
    QJsonArray relations = doc2.object().value("relations").toArray();

    QString jaUrl, enUrl;
    for (const QJsonValue &rel : relations) {
        QJsonObject r = rel.toObject();
        if (r.value("type").toString() != "wikipedia") continue;
        QString wUrl = r.value("url").toObject().value("resource").toString();
        if (wUrl.contains("ja.wikipedia.org"))
            jaUrl = wUrl;
        else if (wUrl.contains("en.wikipedia.org"))
            enUrl = wUrl;
    }

    // ③ 日本語版優先、なければ英語版をGoogle翻訳経由
    if (!jaUrl.isEmpty()) return jaUrl;
    if (!enUrl.isEmpty()) {
        QString page = enUrl;
        page.replace("https://en.wikipedia.org", "https://en-m-wikipedia-org.translate.goog");
        page += (page.contains("?") ? "&" : "?");
        page += "_x_tr_sl=en&_x_tr_tl=ja&_x_tr_hl=ja";
        return page;
    }

    // ④ 最終フォールバック
    QString encodedArtist = QString::fromUtf8(QUrl::toPercentEncoding(artist));
    return QString(
        "https://en-m-wikipedia-org.translate.goog/w/index.php"
        "?search=%1&_x_tr_sl=en&_x_tr_tl=ja&_x_tr_hl=ja").arg(encodedArtist);
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle("Always Player  v6.0.0");
    setWindowIcon(QIcon(":/icons/Always.ico"));
    setMinimumSize(900, 700);

    m_player = new Player(this);
    m_player->init();
    setupUI();
    applyStyle();
    setupTray();

    connect(m_player, &Player::trackChanged,    this, &MainWindow::onTrackChanged);
    connect(m_player, &Player::playbackStarted, this, &MainWindow::onPlaybackStarted);
    connect(m_player, &Player::playbackStopped, this, &MainWindow::onPlaybackStopped);
    connect(m_player, &Player::playbackPaused,  this, &MainWindow::onPlaybackPaused);

    m_infoTimer = new QTimer(this);
    connect(m_infoTimer, &QTimer::timeout, [this]{

        // ★ MCI CD再生中の処理
        if (m_isCdMode && m_mciOpen) {
            // VUメーターを擬似的に振動させる（一時停止中は止める）
            m_vuMeter->setPlaying(!m_cdPaused);

            // MCI からミリ秒形式で位置・長さを取得
            mciSendStringW(L"set cd time format milliseconds", nullptr, 0, nullptr);

            wchar_t posBuf[64] = {}, lenBuf[64] = {}, startBuf[64] = {};
            mciSendStringW(L"status cd position", posBuf, 64, nullptr);

            // 現在トラックの長さと開始位置
            QString lenCmd   = QString("status cd length track %1").arg(m_cdCurrentTrack + 1);
            QString startCmd = QString("status cd position track %1").arg(m_cdCurrentTrack + 1);
            mciSendStringW(reinterpret_cast<LPCWSTR>(lenCmd.utf16()),   lenBuf,   64, nullptr);
            mciSendStringW(reinterpret_cast<LPCWSTR>(startCmd.utf16()), startBuf, 64, nullptr);

            // 再生フォーマットをtmsfに戻す
            mciSendStringW(L"set cd time format tmsf", nullptr, 0, nullptr);

            double posMs      = QString::fromWCharArray(posBuf).trimmed().toDouble();
            double lenMs      = QString::fromWCharArray(lenBuf).trimmed().toDouble();
            double startMs    = QString::fromWCharArray(startBuf).trimmed().toDouble();
            double relPosMs   = posMs - startMs;
            if (relPosMs < 0) relPosMs = 0;

            if (!m_seekDragging && lenMs > 0) {
                m_seekSlider->setValue(static_cast<int>(relPosMs / lenMs * 1000));
                auto fmt = [](double ms) -> QString {
                    int sec = static_cast<int>(ms / 1000.0);
                    int mi = sec / 60, s = sec % 60;
                    return QString("%1:%2").arg(mi).arg(s, 2, 10, QChar('0'));
                };
                m_timeLabel->setText(fmt(relPosMs) + " / " + fmt(lenMs));
            }

            // ③ 現在再生中トラックをpositionから逆算してUI同期
            {
                // posMs（絶対位置）から現在トラックを特定
                int detectedTrack = m_cdCurrentTrack;
                for (int i = 0; i < m_discInfo.tracks.size(); ++i) {
                    double tStart = (m_discInfo.tracks[i].startSector + 150) / 75.0 * 1000.0;
                    double tEnd   = (i + 1 < m_discInfo.tracks.size())
                        ? (m_discInfo.tracks[i+1].startSector + 150) / 75.0 * 1000.0
                        : (m_discInfo.totalSectors + 150) / 75.0 * 1000.0;
                    if (posMs >= tStart && posMs < tEnd) {
                        detectedTrack = i;
                        break;
                    }
                }
                if (detectedTrack != m_cdCurrentTrack && detectedTrack >= 0 && detectedTrack < m_cdTrackCount) {
                    m_cdCurrentTrack = detectedTrack;
                    m_playlist->setCurrentRow(detectedTrack);
                    QString tname = m_playlist->item(detectedTrack)
                        ? m_playlist->item(detectedTrack)->text()
                        : QString("Track %1").arg(detectedTrack + 1);
                    setWindowTitle(jp("Always Player  v6.0.0  -  CD  -  ") + tname);
                }
            }
            return;
        }

        if (m_player->isPlaying()) {
            m_infoLabel->setText(m_player->getInfo(currentMode()));

            // シークスライダー＆時間表示の更新（ドラッグ中は止める）
            if (!m_seekDragging) {
                double pos = 0.0, duration = 0.0;
                mpv_get_property(m_player->mpvHandle(), "time-pos",  MPV_FORMAT_DOUBLE, &pos);
                mpv_get_property(m_player->mpvHandle(), "duration",  MPV_FORMAT_DOUBLE, &duration);
                if (duration > 0) {
                    m_seekSlider->setValue(static_cast<int>(pos / duration * 1000));
                    auto fmt = [](double sec) -> QString {
                        int m = static_cast<int>(sec) / 60;
                        int s = static_cast<int>(sec) % 60;
                        return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
                    };
                    m_timeLabel->setText(fmt(pos) + " / " + fmt(duration));
                }
            }
        }
    });
    m_infoTimer->start(500);  // MCI用は500msで十分

    loadFavorites();

    QString last = m_player->loadLastFolder();
    if (!last.isEmpty() && QDir(last).exists())
        loadFolder(last, false);  // 起動時は自動再生しない
}

MainWindow::~MainWindow() {}

void MainWindow::setupUI()
{
    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    QVBoxLayout *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── toolbar
    QWidget *toolbar = new QWidget();
    toolbar->setObjectName("toolbar");
    toolbar->setFixedHeight(52);
    QHBoxLayout *tbL = new QHBoxLayout(toolbar);
    tbL->setContentsMargins(12, 8, 12, 8);
    tbL->setSpacing(8);

    QPushButton *folderBtn = new QPushButton(jp("\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e\xe3\x81\x97\xe3\x81\xa6\xe5\x86\x8d\xe7\x94\x9f"));
    folderBtn->setObjectName("toolBtn");
    connect(folderBtn, &QPushButton::clicked, this, &MainWindow::onSelectFolder);

    m_albumBrowseBtn = new QPushButton(jp("\xe3\x82\xa2\xe3\x83\xab\xe3\x83\x90\xe3\x83\xa0\xe3\x82\x92\xe8\xa1\xa8\xe7\xa4\xba"));
    m_albumBrowseBtn->setObjectName("toolBtn");
    connect(m_albumBrowseBtn, &QPushButton::clicked, this, &MainWindow::onBrowseAlbums);

    QPushButton *fileBtn = new QPushButton(jp("\xe4\xb8\x80\xe6\x9b\xb2\xe5\x86\x8d\xe7\x94\x9f"));
    fileBtn->setObjectName("toolBtn");
    fileBtn->setFixedWidth(70);
    connect(fileBtn, &QPushButton::clicked, this, &MainWindow::onSelectFile);

    m_infoLabel = new QLabel("---");
    m_infoLabel->setObjectName("infoLabel");
    m_infoLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QPushButton *trayBtn = new QPushButton(jp("\xe2\x96\xbc \xe3\x83\x88\xe3\x83\xac\xe3\x82\xa4"));
    trayBtn->setObjectName("toolBtn");
    trayBtn->setFixedWidth(72);
    connect(trayBtn, &QPushButton::clicked, this, &QWidget::hide);

    QPushButton *exitBtn = new QPushButton("EXIT");
    exitBtn->setObjectName("exitBtn");
    exitBtn->setFixedWidth(52);
    connect(exitBtn, &QPushButton::clicked, [this]{
        m_player->stop();
        saveFavorites();
        QApplication::quit();
    });

    // シャッフルボタン
    m_shuffleBtn = new QPushButton(QString::fromUtf8("\xe2\x87\x8c Shuffle"));
    m_shuffleBtn->setObjectName("toolBtn");
    m_shuffleBtn->setCheckable(true);
    connect(m_shuffleBtn, &QPushButton::clicked, this, [this]{
        QMenu *menu = new QMenu(this);
        menu->addAction(QString::fromUtf8("OFF"), [this]{
            m_player->setShuffle(Player::ShuffleMode::None);
            m_shuffleBtn->setChecked(false);
            m_shuffleBtn->setText(QString::fromUtf8("\xe2\x87\x8c Shuffle"));
        });
        menu->addAction(QString::fromUtf8("\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80\xe5\x86\x85"), [this]{
            m_player->setShuffle(Player::ShuffleMode::Folder);
            m_shuffleBtn->setChecked(true);
            m_shuffleBtn->setText(QString::fromUtf8("\xe2\x87\x8c Folder"));
        });
        menu->addAction(QString::fromUtf8("\xe3\x81\x8a\xe6\xb0\x97\xe3\x81\xab\xe5\x85\xa5\xe3\x82\x8a"), [this]{
            QStringList paths;
            for (auto it = m_favorites.begin(); it != m_favorites.end(); ++it) paths << it.key();
            m_player->setFavoritePaths(paths);
            m_player->setShuffle(Player::ShuffleMode::Favorites);
            m_shuffleBtn->setChecked(true);
            m_shuffleBtn->setText(QString::fromUtf8("\xe2\x87\x8c Fav"));
            if (!paths.isEmpty()) { loadFolder(paths.first(), false); m_player->play(0); }
        });
        menu->exec(QCursor::pos());
    });

    // リピートボタン
    m_repeatBtn = new QPushButton(QString::fromUtf8("\xe2\x86\xa9 Repeat"));
    m_repeatBtn->setObjectName("toolBtn");
    m_repeatBtn->setCheckable(true);
    connect(m_repeatBtn, &QPushButton::clicked, this, [this]{
        QMenu *menu = new QMenu(this);
        menu->addAction("OFF", [this]{
            m_player->setRepeat(Player::RepeatMode::None);
            m_repeatBtn->setChecked(false);
            m_repeatBtn->setText(QString::fromUtf8("\xe2\x86\xa9 Repeat"));
        });
        menu->addAction(QString::fromUtf8("1\xe6\x9b\xb2"), [this]{
            m_player->setRepeat(Player::RepeatMode::One);
            m_repeatBtn->setChecked(true);
            m_repeatBtn->setText(QString::fromUtf8("\xe2\x86\xa9 1\xe6\x9b\xb2"));
        });
        menu->addAction(QString::fromUtf8("\xe5\x85\xa8\xe6\x9b\xb2"), [this]{
            m_player->setRepeat(Player::RepeatMode::All);
            m_repeatBtn->setChecked(true);
            m_repeatBtn->setText(QString::fromUtf8("\xe2\x86\xa9 All"));
        });
        menu->exec(QCursor::pos());
    });

    // タイマーボタン
    m_sleepBtn = new QPushButton("Timer");
    m_sleepBtn->setObjectName("toolBtn");
    connect(m_sleepBtn, &QPushButton::clicked, this, &MainWindow::onSleepTimer);

    tbL->addWidget(m_albumBrowseBtn);
    tbL->addWidget(folderBtn);
    tbL->addWidget(fileBtn);
    tbL->addWidget(m_shuffleBtn);
    tbL->addWidget(m_repeatBtn);
    tbL->addWidget(m_sleepBtn);

    m_artistInfoBtn = new QPushButton("アーティスト情報");
    m_artistInfoBtn->setObjectName("toolBtn");
    m_artistInfoBtn->setEnabled(false);
    connect(m_artistInfoBtn, &QPushButton::clicked, this, [this]() {
        stopIfCd();  // ★ CD再生中なら停止
        if (m_currentArtist.isEmpty()) return;

        // カンマ・セミコロン・スラッシュ・コロンで複数アーティストに分割
        // (vn)(pf)などの楽器表記を除去してから検索
        auto cleanArtist = [](const QString &s) -> QString {
            QString result = s;
            result.remove(QRegularExpression("\\([^)]*\\)"));  // (...)を除去
            return result.trimmed();
        };

        QStringList artists;
        if (m_currentArtist.contains(',') || m_currentArtist.contains(';') ||
            m_currentArtist.contains('/') || m_currentArtist.contains(':')) {
            QStringList parts = m_currentArtist.split(
                QRegularExpression("[,;/:]"), Qt::SkipEmptyParts);
            for (const QString &p : parts) {
                QString cleaned = cleanArtist(p);
                if (!cleaned.isEmpty()) artists << cleaned;
            }
        } else {
            QString cleaned = cleanArtist(m_currentArtist);
            artists << (cleaned.isEmpty() ? m_currentArtist : cleaned);
        }

        // 全アーティストを1スレッドでまとめて検索してURLリストを返す
        auto *watcher = new QFutureWatcher<QStringList>(this);
        connect(watcher, &QFutureWatcher<QStringList>::finished, this,
                [this, watcher]() {
            QStringList urls = watcher->result();
            watcher->deleteLater();
            for (const QString &url : urls) {
                if (!url.isEmpty())
                    QDesktopServices::openUrl(QUrl(url));
            }
        });

        watcher->setFuture(QtConcurrent::run([artists]() -> QStringList {
            QStringList urls;
            for (const QString &artist : artists) {
                urls << searchWikipediaUrl(artist);
            }
            return urls;
        }));
    });

    tbL->addWidget(m_artistInfoBtn);

    // ── ビットパーフェクトドロップダウン
    m_bitPerfectBtn = new QPushButton("BitPerfect ▼");
    m_bitPerfectBtn->setObjectName("toolBtn");
    m_bitPerfectBtn->setToolTip("ビットパーフェクト出力（排他モード）");

    QMenu *bpMenu = new QMenu(this);
    bpMenu->setObjectName("bitPerfectMenu");

    // OFF
    m_bpActOff = bpMenu->addAction(jp("OFF\xef\xbc\x88\xe5\x85\xb1\xe6\x9c\x89\xe3\x83\xa2\xe3\x83\xbc\xe3\x83\x89\xef\xbc\x89"));
    m_bpActOff->setCheckable(true);
    m_bpActOff->setChecked(true);
    bpMenu->addSeparator();

    // 16種類
    struct BpItem { QString label; int rate; int bits; };
    QList<BpItem> items = {
        {"44100 Hz / 16bit",  44100,  16},
        {"44100 Hz / 24bit",  44100,  24},
        {"48000 Hz / 16bit",  48000,  16},
        {"48000 Hz / 24bit",  48000,  24},
        {"88200 Hz / 16bit",  88200,  16},
        {"88200 Hz / 24bit",  88200,  24},
        {"96000 Hz / 16bit",  96000,  16},
        {"96000 Hz / 24bit",  96000,  24},
        {"176400 Hz / 16bit", 176400, 16},
        {"176400 Hz / 24bit", 176400, 24},
        {"192000 Hz / 16bit", 192000, 16},
        {"192000 Hz / 24bit", 192000, 24},
        {"352800 Hz / 16bit", 352800, 16},
        {"352800 Hz / 24bit", 352800, 24},
        {"384000 Hz / 16bit", 384000, 16},
        {"384000 Hz / 24bit", 384000, 24},
    };

    QActionGroup *bpGroup = new QActionGroup(this);
    bpGroup->addAction(m_bpActOff);
    for (const auto &item : items) {
        QAction *act = bpMenu->addAction(item.label);
        act->setCheckable(true);
        act->setData(QVariantList{item.rate, item.bits});
        bpGroup->addAction(act);
    }
    bpGroup->setExclusive(true);

    connect(bpMenu, &QMenu::triggered, this, [this](QAction *act) {
        if (act == m_bpActOff) {
            // UI更新は即座に（GUIスレッド）
            m_bitPerfectBtn->setText("BitPerfect ▼");
            qDebug() << "[BitPerfect] OFF";
            // mpvへの設定変更は別スレッドで（WASAPIデバイス再取得を伴うため）
            QtConcurrent::run([this]{
                mpv_set_property_string(m_player->mpvHandle(), "audio-exclusive", "no");
            });
        } else {
            QVariantList data = act->data().toList();
            int rate = data[0].toInt();
            int bits = data[1].toInt();
            // UI更新は即座に（GUIスレッド）
            m_bitPerfectBtn->setText(
                QString("BitPerfect %1Hz/%2 ▼").arg(rate/1000).arg(bits));
            qDebug() << "[BitPerfect] rate=" << rate << "bits=" << bits;
            // mpvへの設定変更は別スレッドで（WASAPIデバイス再取得を伴うため）
            QtConcurrent::run([this, rate]{
                mpv_set_property_string(m_player->mpvHandle(), "audio-exclusive", "yes");
                mpv_set_property_string(m_player->mpvHandle(), "audio-samplerate",
                    QString::number(rate).toUtf8().constData());
            });
        }
    });

    m_bitPerfectBtn->setMenu(bpMenu);
    tbL->addWidget(m_bitPerfectBtn);

    tbL->addStretch();
    tbL->addWidget(m_infoLabel);

    QPushButton *settingsBtn = new QPushButton("Settings");
    settingsBtn->setObjectName("toolBtn");
    settingsBtn->setFixedWidth(70);
    connect(settingsBtn, &QPushButton::clicked, this, &MainWindow::showSettings);
    tbL->addWidget(settingsBtn);

    tbL->addWidget(trayBtn);
    tbL->addWidget(exitBtn);
    root->addWidget(toolbar);

    // ── ページスタック（メインUI ↔ アルバムブラウザ）
    QStackedWidget *pageStack = new QStackedWidget();
    pageStack->setObjectName("pageStack");

    // ── content（メインUI）
    m_mainContent = new QWidget();
    QVBoxLayout *cl = new QVBoxLayout(m_mainContent);
    cl->setContentsMargins(20, 14, 20, 10);
    cl->setSpacing(10);

    // ── display area
    m_displayWrap = new QWidget();
    m_displayWrap->setObjectName("displayWrap");
    m_displayWrap->setFixedHeight(220);
    QVBoxLayout *dispL = new QVBoxLayout(m_displayWrap);
    dispL->setContentsMargins(0, 0, 0, 0);

    m_stack = new QStackedWidget();
    m_vuMeter = new VUMeter();
    m_stack->addWidget(m_vuMeter);   // index 0

    m_jacket = new QLabel();
    m_jacket->setObjectName("jacketLabel");
    m_jacket->setAlignment(Qt::AlignCenter);
    m_jacket->setText("No artwork");
    m_stack->addWidget(m_jacket);    // index 1

    m_stack->setCurrentIndex(0);
    dispL->addWidget(m_stack);
    cl->addWidget(m_displayWrap);
    m_displayWrap->installEventFilter(this);

    // ── track info
    m_title = new QLabel("--- Please load a folder ---");
    m_title->setObjectName("trackTitle");
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setWordWrap(true);
    cl->addWidget(m_title);

    m_subTitle = new QLabel();
    m_subTitle->setObjectName("trackSub");
    m_subTitle->setAlignment(Qt::AlignCenter);
    cl->addWidget(m_subTitle);

    // ── mode buttons
    QGridLayout *modeGrid = new QGridLayout();
    modeGrid->setSpacing(6);

    const QStringList modeKeys   = {"pure","hires4","dsd8","loudness"};
    const QStringList modeLabels = {
        jp("\xe3\x83\x94\xe3\x83\xa5\xe3\x82\xa2"),
        jp("\xe3\x83\x8f\xe3\x82\xa4\xe3\x83\xac\xe3\x82\xbe x4"),
        jp("\xe7\x96\x91\xe4\xbc\xbc") + "DSD x8",
        jp("\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\x89\xe3\x83\x8d\xe3\x82\xb9"),
    };
    for (int i = 0; i < 4; i++) {
        QPushButton *btn = new QPushButton(modeLabels[i]);
        btn->setObjectName("modeBtn");
        btn->setCheckable(true);
        btn->setChecked(modeKeys[i] == "dsd8");
        m_modeBtns[modeKeys[i]] = btn;
        connect(btn, &QPushButton::clicked, [this, key=modeKeys[i]]{
            stopIfCd();  // ★ CD再生中なら停止
            // ハイレゾ音源はピュアモードのみ
            bool hiRes = (m_player->cachedSr() > 48000);
            QString actualKey = (key != "pure" && hiRes) ? "pure" : key;
            for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it)
                it.value()->setChecked(it.key() == actualKey);
            m_player->setMode(actualKey, m_hp1On, m_hp2On, m_soundField);
            updateModeDesc(actualKey);
            m_infoLabel->setText(m_player->getInfo(currentMode()));
        });
        modeGrid->addWidget(btn, 0, i);
    }
    cl->addLayout(modeGrid);

    // ── HP buttons
    QHBoxLayout *hpRow = new QHBoxLayout();
    hpRow->setSpacing(6);
    m_hp1Btn = new QPushButton(jp("HP1  \xe5\xbc\xb1\xef\xbc\x88\xe8\x87\xaa\xe7\x84\xb6\xe3\x81\xaa\xe5\xba\x83\xe3\x81\x8c\xe3\x82\x8a\xef\xbc\x89"));
    m_hp1Btn->setObjectName("modeBtn");
    m_hp1Btn->setCheckable(true);
    QPushButton *hp1Btn = m_hp1Btn;
    m_hp2Btn = new QPushButton(jp("HP2  \xe5\xbc\xb7\xef\xbc\x88\xe5\x89\x8d\xe6\x96\xb9\xe5\xae\x9a\xe4\xbd\x8d\xef\xbc\x89"));
    m_hp2Btn->setObjectName("modeBtn");
    m_hp2Btn->setCheckable(true);
    QPushButton *hp2Btn = m_hp2Btn;
    connect(hp1Btn, &QPushButton::clicked, [this, hp1Btn, hp2Btn](bool checked){
        stopIfCd();  // ★ CD再生中なら停止
        m_hp1On = checked;
        if (checked) { m_hp2On = false; hp2Btn->setChecked(false); }
        m_player->setMode(currentMode(), m_hp1On, m_hp2On, m_soundField);
    });
    connect(hp2Btn, &QPushButton::clicked, [this, hp1Btn, hp2Btn](bool checked){
        stopIfCd();  // ★ CD再生中なら停止
        m_hp2On = checked;
        if (checked) { m_hp1On = false; hp1Btn->setChecked(false); }
        m_player->setMode(currentMode(), m_hp1On, m_hp2On, m_soundField);
    });
    hpRow->addWidget(m_hp1Btn);
    hpRow->addWidget(m_hp2Btn);
    cl->addLayout(hpRow);

    m_modeDesc = new QLabel(jp("8\xe5\x80\x8d\xe3\x82\xa2\xe3\x83\x83\xe3\x83\x97\xe3\x82\xb5\xe3\x83\xb3\xe3\x83\x97\xe3\x83\xaa\xe3\x83\xb3\xe3\x82\xb0 / \xe3\x83\x8e\xe3\x82\xa4\xe3\x82\xba\xe3\x82\xb7\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\x94\xe3\x83\xb3\xe3\x82\xb0 / \xe7\x96\x91\xe4\xbc\xbc""DSD"));
    m_modeDesc->setObjectName("modeDesc");
    m_modeDesc->setAlignment(Qt::AlignCenter);
    cl->addWidget(m_modeDesc);

    // ── control buttons
    QHBoxLayout *ctrlRow = new QHBoxLayout();
    ctrlRow->setSpacing(16);
    ctrlRow->setAlignment(Qt::AlignCenter);

    auto makeCtrlBtn = [](const QString &iconPath, int size) -> QPushButton* {
        QPushButton *btn = new QPushButton();
        btn->setObjectName("ctrlBtn");
        btn->setFixedSize(size, size);
        btn->setFlat(true);
        QPixmap pix(iconPath);
        if (!pix.isNull()) {
            btn->setIcon(QIcon(pix));
            btn->setIconSize(QSize(size, size));
        }
        return btn;
    };

    m_prevBtn  = makeCtrlBtn(":/buttons/back.png",  60);
    m_pauseBtn = makeCtrlBtn(":/buttons/pause.png", 60);
    m_playBtn  = makeCtrlBtn(":/buttons/start.png", 80);
    m_stopBtn  = makeCtrlBtn(":/buttons/stop.png",  60);
    m_nextBtn  = makeCtrlBtn(":/buttons/skip.png",  60);

    connect(m_prevBtn, &QPushButton::clicked, [this]{
        if (m_isCdMode) {
            int prev = m_cdCurrentTrack - 1;
            if (prev >= 0) startCdTrackStream(prev);
        } else {
            m_player->prev();
        }
    });
    connect(m_pauseBtn, &QPushButton::clicked, [this]{
        if (m_isCdMode) {
            if (!m_mciOpen) {
                return;
            }
            if (m_cdPaused) {
                mciSendStringW(L"set cd time format milliseconds", nullptr, 0, nullptr);
                MCIERROR r = mciSendStringW(L"resume cd", nullptr, 0, nullptr);
                mciSendStringW(L"set cd time format tmsf", nullptr, 0, nullptr);
                if (r == 0) m_cdPaused = false;
            } else {
                mciSendStringW(L"set cd time format milliseconds", nullptr, 0, nullptr);
                MCIERROR r = mciSendStringW(L"pause cd", nullptr, 0, nullptr);
                mciSendStringW(L"set cd time format tmsf", nullptr, 0, nullptr);
                if (r == 0) m_cdPaused = true;
            }
            return;
        }
        if (m_player->isPaused())
            m_player->resume();
        else
            m_player->pause();
    });
    connect(m_playBtn,  &QPushButton::clicked, [this]{
        if (m_isCdMode) {
            if (m_cdPaused) {
                // 一時停止中 → MCI resume
                mciSendStringW(L"resume cd", nullptr, 0, nullptr);
                m_cdPaused = false;
            } else if (m_mciPlaying) {
                // 再生中は何もしない
            } else {
                // 停止中 → 1曲目から再生
                startCdStreamMode();
            }
            return;
        } else if (m_player->isPaused()) {
            m_player->resume();
        } else if (m_player->isPlaying()) {
            // 再生中は何もしない
        } else {
            m_player->play(0);  // 停止後は1曲目から
        }
    });
    connect(m_stopBtn, &QPushButton::clicked, [this]{
        if (m_isCdMode) {
            stopCdStream();
            m_cdPaused = false;
            m_seekSlider->setValue(0);
            m_timeLabel->setText("0:00 / 0:00");
            m_cdCurrentTrack = 0;
            m_playlist->setCurrentRow(-1);
        } else {
            m_player->stop();
            m_seekSlider->setValue(0);
            m_timeLabel->setText("0:00 / 0:00");
            m_playlist->setCurrentRow(-1);
        }
    });
    connect(m_nextBtn, &QPushButton::clicked, [this]{
        if (m_isCdMode) {
            int next = m_cdCurrentTrack + 1;
            if (next < m_cdTrackCount) startCdTrackStream(next);
        } else {
            m_player->next();
        }
    });

    ctrlRow->addWidget(m_prevBtn);
    ctrlRow->addWidget(m_pauseBtn);
    ctrlRow->addWidget(m_playBtn);
    ctrlRow->addWidget(m_stopBtn);
    ctrlRow->addWidget(m_nextBtn);
    cl->addLayout(ctrlRow);

    // ── volume
    QHBoxLayout *volRow = new QHBoxLayout();
    QLabel *volIcon = new QLabel(jp("\xf0\x9f\x94\x8a"));
    volIcon->setFixedWidth(22);
    m_volSlider = new QSlider(Qt::Horizontal);
    m_volSlider->setRange(0, 100);
    m_volSlider->setValue(100);
    QLabel *volVal = new QLabel("100");
    volVal->setFixedWidth(28);
    volVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(m_volSlider, &QSlider::valueChanged, [this, volVal](int v){
        m_player->setVolume(v);
        volVal->setText(QString::number(v));
    });
    volRow->addWidget(volIcon);
    volRow->addWidget(m_volSlider);
    volRow->addWidget(volVal);
    cl->addLayout(volRow);

    // ── seek slider & time
    QHBoxLayout *seekRow = new QHBoxLayout();
    seekRow->setSpacing(6);

    m_seekSlider = new QSlider(Qt::Horizontal);
    m_seekSlider->setRange(0, 1000);
    m_seekSlider->setValue(0);
    m_seekSlider->setObjectName("seekSlider");

    m_timeLabel = new QLabel("0:00 / 0:00");
    m_timeLabel->setObjectName("timeLabel");
    m_timeLabel->setFixedWidth(90);
    m_timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    connect(m_seekSlider, &QSlider::sliderPressed,  [this]{ m_seekDragging = true; });
    connect(m_seekSlider, &QSlider::sliderReleased, [this]{
        m_seekDragging = false;
        if (m_isCdMode && m_mciOpen) {
            // MCI シーク：現在トラックの開始位置＋スライダー比率でシーク
            mciSendStringW(L"set cd time format milliseconds", nullptr, 0, nullptr);
            wchar_t lenBuf[64] = {}, startBuf[64] = {};
            QString lenCmd   = QString("status cd length track %1").arg(m_cdCurrentTrack + 1);
            QString startCmd = QString("status cd position track %1").arg(m_cdCurrentTrack + 1);
            mciSendStringW(reinterpret_cast<LPCWSTR>(lenCmd.utf16()),   lenBuf,   64, nullptr);
            mciSendStringW(reinterpret_cast<LPCWSTR>(startCmd.utf16()), startBuf, 64, nullptr);
            double lenMs   = QString::fromWCharArray(lenBuf).trimmed().toDouble();
            double startMs = QString::fromWCharArray(startBuf).trimmed().toDouble();
            if (lenMs > 0) {
                double seekMs = startMs + lenMs * m_seekSlider->value() / 1000.0;
                // MCIはseekではなくplay from で位置指定再生
                QString playCmd = QString("play cd from %1").arg(static_cast<long long>(seekMs));
                mciSendStringW(reinterpret_cast<LPCWSTR>(playCmd.utf16()), nullptr, 0, nullptr);
                m_cdPaused = false;
            }
            mciSendStringW(L"set cd time format tmsf", nullptr, 0, nullptr);
            return;
        }
        double duration = 0.0;
        mpv_get_property(m_player->mpvHandle(), "duration", MPV_FORMAT_DOUBLE, &duration);
        if (duration > 0) {
            double pos = duration * m_seekSlider->value() / 1000.0;
            mpv_set_property(m_player->mpvHandle(), "time-pos", MPV_FORMAT_DOUBLE, &pos);
        }
    });

    seekRow->addWidget(m_seekSlider);
    seekRow->addWidget(m_timeLabel);
    cl->addLayout(seekRow);

    // ── search & favorites
    QHBoxLayout *searchRow = new QHBoxLayout();
    m_searchBox = new QLineEdit();
    m_searchBox->setPlaceholderText(jp("\xe6\xa4\x9c\xe7\xb4\xa2..."));
    m_searchBox->setObjectName("searchBox");
    connect(m_searchBox, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);

    m_starBtn = new QPushButton(jp("\xe2\x98\x85"));
    m_starBtn->setObjectName("favBtn");
    m_starBtn->setFixedSize(32, 32);
    connect(m_starBtn, &QPushButton::clicked, this, &MainWindow::onFavoriteClicked);

    m_favBtn = new QPushButton(jp("\xe2\x98\x85 \xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88"));
    m_favBtn->setObjectName("favBtn");
    m_favBtn->setFixedHeight(32);
    connect(m_favBtn, &QPushButton::clicked, this, &MainWindow::onShowFavorites);

    searchRow->addWidget(m_searchBox);
    searchRow->addWidget(m_starBtn);
    searchRow->addWidget(m_favBtn);
    cl->addLayout(searchRow);

    // ── playlist
    m_playlist = new QListWidget();
    m_playlist->setObjectName("playlist");
    m_playlist->setFlow(QListWidget::LeftToRight);
    m_playlist->setWrapping(true);
    m_playlist->setResizeMode(QListWidget::Adjust);
    m_playlist->setSpacing(2);
    m_playlist->setMaximumHeight(150);
    connect(m_playlist, &QListWidget::itemDoubleClicked, [this](QListWidgetItem *item){
        int idx = item->data(Qt::UserRole).toInt();
        if (m_isCdMode) startCdTrackStream(idx);
        else m_player->play(idx);
    });
    cl->addWidget(m_playlist);

    m_statusBar = new QLabel(" ");
    m_statusBar->setObjectName("statusBar");
    m_statusBar->setFixedHeight(22);
    cl->addWidget(m_statusBar);

    // アルバムブラウザ（後でsetupAlbumBrowserで構築）
    m_albumBrowser = new QWidget();
    m_albumBrowser->setObjectName("albumBrowser");

    pageStack->addWidget(m_mainContent);   // index 0
    pageStack->addWidget(m_albumBrowser);  // index 1
    pageStack->setCurrentIndex(0);
    root->addWidget(pageStack);
}

void MainWindow::applyStyle()
{
    qApp->setStyle("Fusion");
    qApp->setStyleSheet(R"(
        QWidget { background:#060606; color:#dcdcdc; font-family:'Meiryo'; font-size:11px; }
        QWidget#toolbar { background:#0a0a0a; border-bottom:1px solid #111f2e; }
        QPushButton#toolBtn {
            background:transparent; border:1px solid #1a3a5a; border-radius:6px;
            color:#3a8fe8; font-size:11px; min-height:32px; max-height:32px; padding:0 10px;
        }
        QPushButton#toolBtn:hover { background:rgba(58,143,232,0.12); border-color:#6ab4ff; color:#6ab4ff; }
        QPushButton#exitBtn {
            background:transparent; border:1px solid #1a3a5a; border-radius:6px;
            color:#3a8fe8; font-size:11px; min-height:32px; max-height:32px; padding:0 10px;
        }
        QPushButton#exitBtn:hover { background:rgba(255,140,0,0.15); border-color:#ff8c00; color:#ff8c00; }
        QPushButton#modeBtn {
            background:#0d0d0d; border:1px solid #1a2a3a; border-radius:6px; color:#607080; padding:6px 10px;
        }
        QPushButton#modeBtn:checked { background:#0a1e30; border-color:#3a8fe8; color:#3a8fe8; }
        QPushButton#modeBtn:hover:!checked { border-color:#2a4a6a; color:#8ab4d8; }
        QPushButton#ctrlBtn { background:transparent; border:none; border-radius:50%; }
        QPushButton#ctrlBtn:hover { background:rgba(58,143,232,0.10); }
        QPushButton#ctrlBtn:pressed { background:rgba(58,143,232,0.20); }
        QPushButton#ctrlBtn:disabled { opacity:0.3; }
        QSlider::groove:horizontal { height:3px; background:#181818; border-radius:2px; }
        QSlider::handle:horizontal { background:#3a8fe8; width:14px; height:14px; margin:-6px 0; border-radius:7px; }
        QSlider::sub-page:horizontal { background:#3a8fe8; border-radius:2px; }
        QSlider#seekSlider::groove:horizontal { height:3px; background:#222222; border-radius:2px; }
        QSlider#seekSlider::sub-page:horizontal { background:#555555; border-radius:2px; }
        QSlider#seekSlider::handle:horizontal { width:8px; height:8px; margin:-3px 0; background:#888888; border-radius:4px; }
        QLabel#timeLabel { color:#7a9fc0; font-size:11px; font-family:'Consolas'; }
        QLineEdit#searchBox { background:#0a0a0a; border:1px solid #1a2a3a; border-radius:6px; color:#aaaaaa; padding:4px 8px; height:28px; }
        QLineEdit#searchBox:focus { border-color:#3a8fe8; }
        QPushButton#favBtn { background:#0d0d0d; border:1px solid #1a2a3a; border-radius:6px; color:#607080; padding:0 8px; }
        QPushButton#favBtn:hover { background:rgba(255,140,0,0.15); border-color:#ff8c00; color:#ff8c00; }
        QListWidget#playlist { background:#080808; border:1px solid #111f2e; border-radius:6px; }
        QListWidget#playlist::item { background:#0a0a0a; border:1px solid #111820; border-radius:2px; color:#8090a0; padding:2px 6px; margin:1px; }
        QListWidget#playlist::item:selected { background:#0a1e30; border-color:#3a8fe8; color:#3a8fe8; }
        QListWidget#playlist::item:hover { background:#0d1a28; color:#aabbc8; }
        QLabel#trackTitle { color:#e8e8e8; font-size:14px; font-weight:bold; }
        QLabel#trackSub { color:#607080; font-size:10px; }
        QLabel#modeDesc { color:#405060; font-size:9px; }
        QLabel#infoLabel { color:#3a6080; font-size:10px; font-family:'Consolas'; }
        QLabel#statusBar { color:#304050; font-size:9px; padding-left:4px; }
        QLabel#jacketLabel { color:#141414; font-size:11px; }
        QWidget#displayWrap { background:#020202; border:1px solid #0d1a28; border-radius:10px; }
        QScrollBar:vertical { width:6px; background:#080808; }
        QScrollBar::handle:vertical { background:#1a3a5a; border-radius:3px; }
        QWidget#albumBrowser { background:#060606; }
        QWidget#albumHeader { background:#0a0a0a; border-bottom:1px solid #111f2e; }
        QWidget#albumSearchBar { background:#080808; border-bottom:1px solid #0d1a28; }
        QLineEdit#albumSearchBox {
            background:#0a0a0a; border:1px solid #1a2a3a; border-radius:6px;
            color:#aaaaaa; padding:4px 8px; height:26px; font-size:11px;
        }
        QLineEdit#albumSearchBox:focus { border-color:#3a8fe8; }
        QLabel#albumHeaderLabel { color:#607080; font-size:10px; }
        QLabel#albumPathLabel { color:#2a6898; font-size:10px; }
        QPushButton#albumCloseBtn {
            background:transparent; border:1px solid #1a2a3a; border-radius:4px;
            color:#607080; font-size:10px; padding:2px 8px;
        }
        QPushButton#albumCloseBtn:hover { border-color:#3a8fe8; color:#3a8fe8; }
        QLabel#genreLabel { color:#304050; font-size:9px; }
        QWidget#albumCard {
            background:#0d0d0d; border:1px solid #1a2a3a; border-radius:6px;
        }
        QWidget#albumCard:hover { background:#111820; border-color:#2a4a6a; }
        QLabel#albumArtLabel { border-radius:4px; background:#080808; }
        QLabel#albumNameLabel { color:#c8c8c8; font-size:11px; font-weight:bold; }
        QLabel#albumSubLabel  { color:#506070; font-size:9px; }
        QWidget#albumFooter { background:#080808; border-top:1px solid #0d1a28; }
        QLabel#albumFooterLabel { color:#304050; font-size:9px; }
        QScrollArea { border:none; background:#060606; }
    )");
}

void MainWindow::setupTray()
{
    m_tray = new TrayManager(this);
    connect(m_tray, &TrayManager::showRequested, this, [this]{ show(); raise(); activateWindow(); });
    connect(m_tray, &TrayManager::folderRequested, this, &MainWindow::onSelectFolder);
    connect(m_tray, &TrayManager::quitRequested, this, [this]{
        m_player->stop();
        saveFavorites();
        QApplication::quit();
    });
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    MSG *msg = static_cast<MSG*>(message);
    if (msg->message == WM_DEVICECHANGE) {
        if (msg->wParam == 0x8004 && m_isCdMode) {
            // CD取り出し
            qDebug() << "[CD] Device removed";
            stopCdStream();
            clearCdState();
            m_title->setText("CD");
            m_subTitle->setText(jp("\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xb9\xe3\x82\xaf\xe3\x81\x8c\xe3\x81\x82\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93"));
            m_statusBar->setText(jp(">> CD\xe3\x82\x92\xe6\x8c\xbf\xe5\x85\xa5\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84"));
        }
        if (msg->wParam == 0x8000) {
            // CD挿入 → 2秒後に自動認識
            QTimer::singleShot(2000, this, [this](){
                for (char c = 'D'; c <= 'Z'; ++c) {
                    QString drive = QString("%1:").arg(c);
                    if (GetDriveTypeA((drive + "\\").toLocal8Bit().constData()) == DRIVE_CDROM) {
                        CdDrive tmp;
                        if (tmp.open(drive)) {
                            DiscInfo di = tmp.readToc();
                            tmp.close();
                            if (!di.tracks.isEmpty()) {
                                stopCdStream();
                                clearCdState();
                                playCd(drive);
                                return;
                            }
                        }
                    }
                }
            });
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveFavorites(); 
    event->ignore();
    hide();
    m_tray->showMessage("Always Player", jp("\xe3\x83\x90\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xb0\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x89\xe3\x81\xa7\xe5\x86\x8d\xe7\x94\x9f\xe4\xb8\xad\xe3\x81\xa7\xe3\x81\x99"));
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    // アルバムカードのクリック
    if (event->type() == QEvent::MouseButtonPress) {
        QWidget *w = qobject_cast<QWidget*>(obj);
        if (w && w->objectName() == "albumCard") {
            QString path = w->property("albumPath").toString();
            if (!path.isEmpty()) {
                m_player->stop();
                turnOffBitPerfect();
                loadFolder(path, false);
                QStackedWidget *ps = qobject_cast<QStackedWidget*>(m_mainContent->parentWidget());
                if (ps) ps->setCurrentIndex(0);
            }
            return true;
        }
    }
    // ジャケット↔VUメーター切り替え
    if (obj == m_displayWrap && event->type() == QEvent::MouseButtonPress) {
        if (m_hasArtwork) {
            m_showVU = !m_showVU;
            m_stack->setCurrentIndex(m_showVU ? 0 : 1);
            m_vuMeter->setPlaying(m_showVU && m_player->isPlaying());
        }
        return true;
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::onSelectFolder()
{
    m_player->stop();
    turnOffBitPerfect();
    m_player->setCachedInfo(0, 0, 0);
    m_hp1On = false;
    m_hp2On = false;
    m_infoLabel->setText("---");
    m_title->clear();
    m_subTitle->clear();
    for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it) {
        it.value()->setEnabled(true);
        it.value()->setChecked(it.key() == "dsd8");
    }

    QString path = QFileDialog::getExistingDirectory(
        this, jp("\xe9\x9f\xb3\xe6\xa5\xbd\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e"),
        m_currentFolder, QFileDialog::ShowDirsOnly);

    if (path.isEmpty()) return;

    // ★ CDドライブが選択されたら playCd() へ
    QString driveLetter = path.left(2);  // 例: "D:"
    if (GetDriveTypeA((driveLetter + "\\").toLocal8Bit().constData()) == DRIVE_CDROM) {
        playCd(driveLetter);
        return;
    }

    // 通常フォルダ
    loadFolder(path, false);
}

// ─────────────────────────────────────────────
// CD Stream Mode
// ─────────────────────────────────────────────

// ★ UIキャッシュを完全クリア
void MainWindow::clearCdState()
{
    m_player->stop();
    m_isCdMode       = false;
    m_cdCurrentTrack = 0;
    m_playlist->clear();
    m_allItems.clear();
    m_allIndices.clear();
    m_title->setText("");
    m_subTitle->setText("");
    m_statusBar->setText("");
    m_seekSlider->setValue(0);
    m_timeLabel->setText("0:00 / 0:00");
    m_hasArtwork = false;
    m_showVU     = true;
    m_stack->setCurrentIndex(0);

    // ★ モードボタンを全て有効化に戻す
    for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it)
        it.value()->setEnabled(true);
}

// ★ CDドライブ選択時：TOC読み込みとプレイリスト表示
void MainWindow::playCd(const QString &drive)
{
    // 既存のCD再生ストリームのみ停止（UIリセットはしない）
    stopCdStream();
    m_cdPaused = false;
    m_mciPlaying = false;

    CdDrive tmp;
    if (!tmp.open(drive)) {
        qDebug() << "[playCd] drive open failed";
        return;
    }
    m_discInfo = tmp.readToc();
    tmp.close();

    if (m_discInfo.tracks.isEmpty()) {
        qDebug() << "[playCd] no tracks";
        return;
    }

    m_cdDrive      = drive;
    m_isCdMode     = true;
    m_cdTrackCount = m_discInfo.tracks.size();
    m_cdCurrentTrack = 0;

    // ★ UIキャッシュを完全クリアしてから構築
    m_player->stop();
    m_playlist->clear();
    m_allItems.clear();
    m_allIndices.clear();

    for (int i = 0; i < m_cdTrackCount; i++) {
        const TrackInfo &t = m_discInfo.tracks[i];
        QString name = t.title.isEmpty()
            ? QString("Track %1").arg(t.number, 2, 10, QChar('0'))
            : t.title;
        m_allItems   << name;
        m_allIndices << i;
        QListWidgetItem *item = new QListWidgetItem(name);
        item->setData(Qt::UserRole, i);
        m_playlist->addItem(item);
    }
    m_playlist->setCurrentRow(0);

    QString sub = QString("%1 Tracks").arg(m_cdTrackCount);
    if (!m_discInfo.albumTitle.isEmpty()) sub += "   " + m_discInfo.albumTitle;

    m_title->setText("CD  -  Press Play");
    m_subTitle->setText(sub);
    m_hasArtwork = false;
    m_showVU     = true;
    m_stack->setCurrentIndex(0);

    if (m_artistInfoBtn) m_artistInfoBtn->setEnabled(false);
    setWindowTitle(jp("Always Player  v6.0.0  -  CD"));
    // ★ バックグラウンドでメタデータ取得開始（再生前はpauseBtn無効）
    // pauseBtn は常に有効（setEnabledによる色変化を避ける）
    m_statusBar->setText(jp("\xe6\xa4\x9c\xe7\xb4\xa2\xe4\xb8\xad... MusicBrainz / iTunes"));
    if (!m_cdMetaFetcher) {
        m_cdMetaFetcher = new CdMetaFetcher(this);
        connect(m_cdMetaFetcher, &CdMetaFetcher::metaReady,
                this, &MainWindow::onCdMetaReady);
    }
    m_cdMetaFetcher->fetchAsync(m_discInfo);

    // ★ CD再生時はピュアのみ有効・他は無効化
    for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it) {
        it.value()->setChecked(it.key() == "pure");
        it.value()->setEnabled(it.key() == "pure");
    }
    // pure以外のモードボタンにDSP処理不可を追加表示
    {
        QMap<QString,QString> cdLabels;
        cdLabels["hires4"] = jp("\xe3\x83\x8f\xe3\x82\xa4\xe3\x83\xac\xe3\x82\xbe  " "\xef\xbc\x88" "DSP" "\xe5\x87\xa6\xe7\x90\x86\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
        cdLabels["dsd8"]   = jp("\xe7\x96\x91\xe4\xbc\xbc" "DSD  " "\xef\xbc\x88" "DSP" "\xe5\x87\xa6\xe7\x90\x86\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
        cdLabels["loudness"] = jp("\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\x89\xe3\x83\x8d\xe3\x82\xb9  " "\xef\xbc\x88" "DSP" "\xe5\x87\xa6\xe7\x90\x86\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
        for (auto it = cdLabels.begin(); it != cdLabels.end(); ++it)
            if (m_modeBtns.contains(it.key()))
                m_modeBtns[it.key()]->setText(it.value());
    }
    // HP1/HP2をDSP処理不可表示に変更
    if (m_hp1Btn) {
        m_hp1Btn->setText(jp("HP1  \xe5\xbc\xb1\xef\xbc\x88" "DSP" "\xe5\x87\xa6\xe7\x90\x86\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89"));
        m_hp1Btn->setEnabled(false);
        m_hp1Btn->setCheckable(false);
    }
    if (m_hp2Btn) {
        m_hp2Btn->setText(jp("HP2  \xe5\xbc\xb7\xef\xbc\x88" "DSP" "\xe5\x87\xa6\xe7\x90\x86\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89"));
        m_hp2Btn->setEnabled(false);
        m_hp2Btn->setCheckable(false);
    }
    updateModeDesc("pure");
    if (m_infoLabel) m_infoLabel->setText("PCM_S16LE  |  1411 kbps  |  44.1 kHz  /  16bit");
}

// ★ ストリームを停止してリソースを解放
// ★ CD再生中なら安全に停止する（全ボタン共通の入口）
void MainWindow::stopIfCd()
{
    if (!m_isCdMode) return;  // CD再生中でなければ何もしない

    // 1. タイマー停止（コールバックを止める）
    if (m_cdTrackTimer) {
        m_cdTrackTimer->stop();
        delete m_cdTrackTimer;
        m_cdTrackTimer = nullptr;
    }

    // 2. MCI停止
    if (m_mciOpen) {
        mciSendStringW(L"stop cd", nullptr, 0, nullptr);
        mciSendStringW(L"close cd", nullptr, 0, nullptr);
        m_mciOpen = false;
        m_mciPlaying = false;
        m_cdPaused = false;
        // pauseBtn setEnabled削除
    }

    // 3. VUメーター停止
    if (m_vuMeter) m_vuMeter->setPlaying(false);

    // 4. CD状態フラグをリセット
    m_isCdMode       = false;
    m_cdCurrentTrack = 0;

    // 5. UIをリセット
    m_seekSlider->setValue(0);
    m_timeLabel->setText("0:00 / 0:00");

    // 6. モードボタンを全て有効化
    for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it)
        it.value()->setEnabled(true);
    // モードボタンのテキストを元に戻す
    {
        QMap<QString,QString> origLabels;
        origLabels["hires4"]   = jp("\xe3\x83\x8f\xe3\x82\xa4\xe3\x83\xac\xe3\x82\xbe x4");
        origLabels["dsd8"]     = jp("\xe7\x96\x91\xe4\xbc\xbc") + "DSD x8";
        origLabels["loudness"] = jp("\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\x89\xe3\x83\x8d\xe3\x82\xb9");
        for (auto it = origLabels.begin(); it != origLabels.end(); ++it)
            if (m_modeBtns.contains(it.key()))
                m_modeBtns[it.key()]->setText(it.value());
    }
    // HP1/HP2を元のテキストに復元
    if (m_hp1Btn) {
        m_hp1Btn->setText(jp("HP1  \xe5\xbc\xb1\xef\xbc\x88\xe8\x87\xaa\xe7\x84\xb6\xe3\x81\xaa\xe5\xba\x83\xe3\x81\x8c\xe3\x82\x8a\xef\xbc\x89"));
        m_hp1Btn->setEnabled(true);
        m_hp1Btn->setCheckable(true);
    }
    if (m_hp2Btn) {
        m_hp2Btn->setText(jp("HP2  \xe5\xbc\xb7\xef\xbc\x88\xe5\x89\x8d\xe6\x96\xb9\xe5\xae\x9a\xe4\xbd\x8d\xef\xbc\x89"));
        m_hp2Btn->setEnabled(true);
        m_hp2Btn->setCheckable(true);
    }
}

void MainWindow::stopCdStream()
{
    // MCIを停止・クローズ
    if (m_mciOpen) {
        mciSendStringW(L"stop cd", nullptr, 0, nullptr);
        mciSendStringW(L"close cd", nullptr, 0, nullptr);
        m_mciOpen = false;
    }
    if (m_cdTrackTimer) {
        m_cdTrackTimer->stop();
        delete m_cdTrackTimer;
        m_cdTrackTimer = nullptr;
    }
    // CdStreamWriterも念のため停止
    if (m_cdWriter) {
        m_cdWriter->stop();
        m_cdWriter->wait(1000);
        m_cdWriter.reset();
    }
    if (m_cdReader) {
        m_cdReader->stopReading();
        m_cdReader->wait(1000);
        m_cdReader.reset();
    }
    if (m_cdBuffer) m_cdBuffer.reset();
}

// ★ MCI方式CD再生開始（再生ボタン押下時）
void MainWindow::startCdStreamMode()
{
    if (!m_isCdMode) return;
    startCdTrackStream(m_cdCurrentTrack);
}

// ★ MCI方式：指定トラックを即時再生
void MainWindow::startCdTrackStream(int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= m_cdTrackCount) return;

    m_cdCurrentTrack = trackIndex;
    m_cdPaused = false;   // ★ 新トラック開始時は一時停止解除

    // 前の再生を停止
    stopCdStream();

    // ★ MCI でCDを開く（時間フォーマットをトラックに設定）
    QString openCmd = QString("open %1 type cdaudio alias cd").arg(m_cdDrive);
    MCIERROR err = mciSendStringW(
        reinterpret_cast<LPCWSTR>(openCmd.utf16()),
        nullptr, 0, nullptr);

    if (err != 0) {
        wchar_t errMsg[256];
        mciGetErrorStringW(err, errMsg, 256);
        qDebug() << "[MCI] open failed:" << QString::fromWCharArray(errMsg);
        m_statusBar->setText(">> CD open failed");
        return;
    }

    m_mciOpen = true;

    // ★ 時間フォーマットをTMSF（Track-Minute-Second-Frame）に設定
    mciSendStringW(L"set cd time format tmsf", nullptr, 0, nullptr);

    // ★ 少し待ってからplay（先頭欠け防止）
    QTimer::singleShot(2000, this, [this, trackIndex](){
        if (!m_mciOpen) return;

        // TMSF形式: track:minute:second:frame → "play cd from 01:00:00:00"
        QString fromStr = QString("%1:00:00:00").arg(trackIndex + 1, 2, 10, QChar('0'));
        QString playCmd = QString("play cd from %1").arg(fromStr);
        MCIERROR err2 = mciSendStringW(
            reinterpret_cast<LPCWSTR>(playCmd.utf16()),
            nullptr, 0, nullptr);

        if (err2 != 0) {
            wchar_t errMsg[256];
            mciGetErrorStringW(err2, errMsg, 256);
            qDebug() << "[MCI] play failed:" << QString::fromWCharArray(errMsg);
            mciSendStringW(L"close cd", nullptr, 0, nullptr);
            m_mciOpen = false;
            m_statusBar->setText(">> CD play failed");
            return;
        }

        m_mciPlaying = true;   // ★ 実際にplay開始
        // pauseBtn setEnabled削除
        qDebug() << "[MCI] playing Track" << (trackIndex + 1);
        if (m_infoLabel) m_infoLabel->setText("PCM_S16LE  |  1411 kbps  |  44.1 kHz  /  16bit");

        // タイマー起動（play直後4秒間は誤検知防止のためスキップ）
        QTimer::singleShot(4000, this, [this](){
            if (!m_mciOpen || !m_mciPlaying) return;
            if (m_cdTrackTimer) {
                m_cdTrackTimer->stop();
                m_cdTrackTimer->deleteLater();
                m_cdTrackTimer = nullptr;
            }
            m_cdTrackTimer = new QTimer(this);
            m_cdTrackTimer->setInterval(500);
            connect(m_cdTrackTimer, &QTimer::timeout, this, [this](){
                if (!m_mciOpen) { m_cdTrackTimer->stop(); return; }
            wchar_t statusBuf[64] = {};
            mciSendStringW(L"status cd mode", statusBuf, 64, nullptr);
            QString mode = QString::fromWCharArray(statusBuf).trimmed().toLower();
            // pause中は stopped が返るので終了判定しない
            if (m_cdPaused) return;
            if (mode == "stopped" || mode == "not ready" || mode.isEmpty()) {
                m_cdTrackTimer->stop();
                qDebug() << "[MCI] Track" << (m_cdCurrentTrack + 1) << "finished";
                mciSendStringW(L"close cd", nullptr, 0, nullptr);
                m_mciOpen = false;
                int next = m_cdCurrentTrack + 1;
                if (next < m_cdTrackCount) {
                    QTimer::singleShot(200, this, [this, next](){
                        startCdTrackStream(next);
                    });
                } else {
                    m_vuMeter->setPlaying(false);
                    m_seekSlider->setValue(0);
                    m_timeLabel->setText("0:00 / 0:00");
                    m_statusBar->setText(">> CD 再生完了");
                    setWindowTitle(jp("Always Player  v6.0.0  -  CD"));
                }
            }
        });
            m_cdTrackTimer->start();
        });
    });

    // UI更新
    QString name = (!m_discInfo.tracks.isEmpty() &&
                    trackIndex < m_discInfo.tracks.size() &&
                    !m_discInfo.tracks[trackIndex].title.isEmpty())
        ? m_discInfo.tracks[trackIndex].title
        : QString("Track %1").arg(trackIndex + 1, 2, 10, QChar('0'));

    m_title->setText(name);
    m_subTitle->setText(QString("%1  /  %2").arg(trackIndex + 1).arg(m_cdTrackCount));
    m_playlist->setCurrentRow(trackIndex);
    m_statusBar->setText(">> " + name);
    setWindowTitle(jp("Always Player  v6.0.0  -  CD  -  ") + name);


}

void MainWindow::onSelectFile()
{
    m_player->stop();     // 停止ボタンと同じ処理でWASAPIを解放
    turnOffBitPerfect();
    // ── 前の状態をリセット ──
    m_player->setCachedInfo(0, 0, 0);
    // m_soundFieldは維持（Settings設定を引き継ぐ）
    m_hp1On = false;
    m_hp2On = false;
    m_infoLabel->setText("---");
    for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it) {
        it.value()->setEnabled(true);
        it.value()->setChecked(it.key() == "dsd8");
    }
    QString path = QFileDialog::getOpenFileName(
        this, jp("\xe9\x9f\xb3\xe6\xa5\xbd\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e"),
        m_currentFolder,
        "Audio Files (*.mp3 *.flac *.wav *.aac *.m4a *.ogg *.opus *.dsf *.dff *.aiff *.wv)");
    if (path.isEmpty()) return;
    m_player->loadFile(path);
    // TagLibで情報取得→cachedInfo更新→infoLabel即時表示
    {
        int br = 0, sr = 0, bits = 0;
        QString ext = QFileInfo(path).suffix().toLower();
        if (ext == "flac") {
            TagLib::FLAC::File tf(path.toStdWString().c_str());
            if (tf.isValid() && tf.audioProperties()) {
                br = tf.audioProperties()->bitrate();
                sr = tf.audioProperties()->sampleRate();
                bits = tf.audioProperties()->bitsPerSample();
            }
        } else if (ext == "m4a" || ext == "mp4" || ext == "aac") {
            TagLib::MP4::File tf(path.toStdWString().c_str());
            if (tf.isValid() && tf.audioProperties()) {
                br = tf.audioProperties()->bitrate();
                sr = tf.audioProperties()->sampleRate();
                bits = tf.audioProperties()->bitsPerSample();
            }
        } else {
            TagLib::FileRef ref(path.toStdWString().c_str());
            if (!ref.isNull() && ref.audioProperties()) {
                br = ref.audioProperties()->bitrate();
                sr = ref.audioProperties()->sampleRate();
            }
        }
        m_player->setCachedInfo(br, sr, bits);
        bool hiRes = (sr > 48000);
        QString autoMode = hiRes ? "pure" : "dsd8";
        for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it) {
            it.value()->setChecked(it.key() == autoMode);
            it.value()->setEnabled(!hiRes || it.key() == "pure");
        }
        updateModeDesc(autoMode);
        m_player->setModeQuiet(autoMode);
        m_infoLabel->setText(m_player->getInfo(currentMode()));
    }
}

void MainWindow::loadFolder(const QString &path, bool autoPlay)
{
    m_isCdMode = false;  // ★ フォルダ読み込み時はCDモードを解除
    m_currentFolder = path;
    m_player->loadFolder(path);
    m_playlist->clear();
    m_allItems.clear();
    m_allIndices.clear();
    for (int i = 0; i < m_player->total(); i++) {
        QString name = m_player->fileAt(i);
        m_allItems << name;
        m_allIndices << i;
        QListWidgetItem *item = new QListWidgetItem(name);
        item->setData(Qt::UserRole, i);
        m_playlist->addItem(item);
    }
    // ── フォルダ選択直後：1曲目をTagLibで読んでUI即時更新 ──
    if (m_player->total() > 0) {
        QString fp = m_player->filePathAt(0);
        int br = 0, sr = 0, bits = 0;
        QString title, artist;
        if (!fp.isEmpty()) {
            QString ext = QFileInfo(fp).suffix().toLower();
            if (ext == "flac") {
                TagLib::FLAC::File tf(fp.toStdWString().c_str());
                if (tf.isValid()) {
                    if (tf.tag()) {
                        title  = QString::fromStdString(tf.tag()->title().to8Bit(true));
                        artist = QString::fromStdString(tf.tag()->artist().to8Bit(true));
                    }
                    if (tf.audioProperties()) {
                        br   = tf.audioProperties()->bitrate();
                        sr   = tf.audioProperties()->sampleRate();
                        bits = tf.audioProperties()->bitsPerSample();
                    }
                }
            } else if (ext == "m4a" || ext == "mp4" || ext == "aac") {
                TagLib::MP4::File tf(fp.toStdWString().c_str());
                if (tf.isValid()) {
                    if (tf.tag()) {
                        title  = QString::fromStdString(tf.tag()->title().to8Bit(true));
                        artist = QString::fromStdString(tf.tag()->artist().to8Bit(true));
                    }
                    if (tf.audioProperties()) {
                        br   = tf.audioProperties()->bitrate();
                        sr   = tf.audioProperties()->sampleRate();
                        bits = tf.audioProperties()->bitsPerSample();
                    }
                }
            } else {
                TagLib::FileRef ref(fp.toStdWString().c_str());
                if (!ref.isNull()) {
                    if (ref.tag()) {
                        title  = QString::fromStdString(ref.tag()->title().to8Bit(true));
                        artist = QString::fromStdString(ref.tag()->artist().to8Bit(true));
                    }
                    if (ref.audioProperties()) {
                        br = ref.audioProperties()->bitrate();
                        sr = ref.audioProperties()->sampleRate();
                    }
                }
            }
        }
        // キャッシュ更新
        m_player->setCachedInfo(br, sr, bits);
        // ハイレゾ判定→モードボタン更新
        bool hiRes = (sr > 48000);
        QString autoMode = hiRes ? "pure" : "dsd8";
        for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it) {
            it.value()->setChecked(it.key() == autoMode);
            it.value()->setEnabled(!hiRes || it.key() == "pure");
        }
        updateModeDesc(autoMode);
        m_player->setModeQuiet(autoMode);
        // infoLabel即時表示
        m_infoLabel->setText(m_player->getInfo(currentMode()));
        // タグ表示
        QString firstFileName = m_player->fileAt(0);
        m_title->setText(title.isEmpty() ? QFileInfo(fp).completeBaseName() : title);
        QString sub = QString("1  /  %1").arg(m_player->total());
        if (!artist.isEmpty()) sub += "   " + artist;
        m_subTitle->setText(sub);
        m_currentArtist = artist;
        if (m_artistInfoBtn) m_artistInfoBtn->setEnabled(!artist.isEmpty());
        m_playlist->setCurrentRow(0);
        setWindowTitle(QString::fromUtf8("Always Player  v6.0.0  -  ") + QFileInfo(fp).fileName());
        m_statusBar->setText(">> " + (title.isEmpty() ? QFileInfo(fp).completeBaseName() : title));
    }

    // ── アルバムアートを即時更新 ──
    QPixmap art = findAlbumArt(path, 200);
    if (!art.isNull()) {
        m_jacket->setPixmap(art);
        m_jacket->setText("");
        m_hasArtwork = true;
        m_showVU = false;
        m_stack->setCurrentIndex(1);
    } else {
        m_jacket->setPixmap(QPixmap());
        m_jacket->setText("No artwork");
        m_hasArtwork = false;
        m_showVU = true;
        m_stack->setCurrentIndex(0);
    }

    if (autoPlay)
        QTimer::singleShot(100, this, [this]{ m_player->play(0); });
}

void MainWindow::updateJacket()
{
    QString cover = m_player->getCoverArt();
    if (!cover.isEmpty()) {
        QPixmap pix(cover);
        if (!pix.isNull()) {
            m_jacket->setPixmap(pix.scaled(200, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            m_jacket->setText("");
            m_hasArtwork = true;
            m_showVU = false;
            m_stack->setCurrentIndex(1);
            m_vuMeter->setPlaying(false);
            return;
        }
    }
    m_jacket->setPixmap(QPixmap());
    m_jacket->setText("No artwork");
    m_hasArtwork = false;
    m_showVU = true;
    m_stack->setCurrentIndex(0);
}

void MainWindow::onTrackChanged(int index, const QString &filename,
                                const QString &title, const QString &artist)
{
    // ★ CD再生中はStreamModeで管理するのでスキップ
    if (m_isCdMode) return;

    m_title->setText(title.isEmpty() ? filename.section('.', 0, -2) : title);
    QString sub = QString("%1  /  %2").arg(index + 1).arg(m_player->total());
    if (!artist.isEmpty()) sub += "   " + artist;
    m_subTitle->setText(sub);
    m_currentArtist = artist;
    if (m_artistInfoBtn) m_artistInfoBtn->setEnabled(!artist.isEmpty());
    m_playlist->setCurrentRow(index);
    setWindowTitle(QString::fromUtf8("Always Player  v6.0.0  -  ") + filename);
    m_statusBar->setText(">> " + (title.isEmpty() ? filename.section('.', 0, -2) : title));

    // ハイレゾ自動モード切り替え＋disabled制御＋infoLabel更新
    {
        QString fp = m_player->currentFilePath();
        if (!fp.isEmpty()) {
            // TagLibでsr/bits取得
            int br = 0, sr = 0, bits = 0;
            QString ext = QFileInfo(fp).suffix().toLower();
            if (ext == "flac") {
                TagLib::FLAC::File tf(fp.toStdWString().c_str());
                if (tf.isValid() && tf.audioProperties()) {
                    br   = tf.audioProperties()->bitrate();
                    sr   = tf.audioProperties()->sampleRate();
                    bits = tf.audioProperties()->bitsPerSample();
                }
            } else if (ext == "m4a" || ext == "mp4" || ext == "aac") {
                TagLib::MP4::File tf(fp.toStdWString().c_str());
                if (tf.isValid() && tf.audioProperties()) {
                    br   = tf.audioProperties()->bitrate();
                    sr   = tf.audioProperties()->sampleRate();
                    bits = tf.audioProperties()->bitsPerSample();
                }
            } else {
                TagLib::FileRef ref(fp.toStdWString().c_str());
                if (!ref.isNull() && ref.audioProperties()) {
                    br = ref.audioProperties()->bitrate();
                    sr = ref.audioProperties()->sampleRate();
                }
            }
            // sr/bitsのみ更新、brはplay()のQtConcurrentで設定済みの値を維持
            m_player->setCachedInfo(m_player->cachedBr(), sr, bits);

            // ハイレゾ判定
            bool hiRes = (sr > 48000);
            QString autoMode = hiRes ? "pure" : "dsd8";

            // ボタン状態更新
            for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it) {
                it.value()->setChecked(it.key() == autoMode);
                it.value()->setEnabled(!hiRes || it.key() == "pure");
            }
            updateModeDesc(autoMode);

            // モードを実際に切り替え（DSP処理も反映）
            m_player->setMode(autoMode, m_hp1On, m_hp2On, m_soundField);

            // infoLabel即時更新
            m_infoLabel->setText(m_player->getInfo(currentMode()));
        }
    }
    // 少し遅延してアルバムアートを更新（mpvのファイルオープンと競合回避）
    QTimer::singleShot(300, this, [this]() { updateJacket(); });
}

void MainWindow::onPlaybackStarted() { m_vuMeter->setPlaying(true); }
void MainWindow::onPlaybackPaused()  { m_vuMeter->setPlaying(false); }
void MainWindow::onPlaybackStopped()
{
    m_vuMeter->setPlaying(false);
    m_statusBar->setText(" ");
    if (!m_hasArtwork) { m_stack->setCurrentIndex(0); m_showVU = true; }
}

void MainWindow::onSearchChanged(const QString &text)
{
    m_playlist->clear();
    for (int i = 0; i < m_allItems.size(); i++) {
        if (text.isEmpty() || m_allItems[i].contains(text, Qt::CaseInsensitive)) {
            QListWidgetItem *item = new QListWidgetItem(m_allItems[i]);
            item->setData(Qt::UserRole, m_allIndices[i]);
            m_playlist->addItem(item);
        }
    }
}

void MainWindow::onFavoriteClicked()
{
    if (m_currentFolder.isEmpty()) return;
    QString name = QFileInfo(m_currentFolder).fileName();
    if (m_favorites.contains(m_currentFolder))
        m_favorites.remove(m_currentFolder);
    else
        m_favorites[m_currentFolder] = name;
    saveFavorites();
}

void MainWindow::onShowFavorites()
{
    stopIfCd();  // ★ CD再生中なら停止
    if (m_favorites.isEmpty()) {
        QMessageBox::information(this,
            jp("\xe3\x81\x8a\xe6\xb0\x97\xe3\x81\xab\xe5\x85\xa5\xe3\x82\x8a"),
            jp("\xe3\x81\x8a\xe6\xb0\x97\xe3\x81\xab\xe5\x85\xa5\xe3\x82\x8a\xe3\x81\xaf\xe3\x81\x82\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93"));
        return;
    }
    QDialog *dlg = new QDialog(this);
    dlg->setWindowTitle(jp("\xe3\x81\x8a\xe6\xb0\x97\xe3\x81\xab\xe5\x85\xa5\xe3\x82\x8a\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80"));
    dlg->setMinimumWidth(400);
    QVBoxLayout *vl = new QVBoxLayout(dlg);
    QListWidget *lw = new QListWidget();
    for (auto it = m_favorites.begin(); it != m_favorites.end(); ++it) {
        QListWidgetItem *item = new QListWidgetItem(it.value());
        item->setData(Qt::UserRole, it.key());
        lw->addItem(item);
    }
    vl->addWidget(lw);
    QHBoxLayout *hl = new QHBoxLayout();
    QPushButton *openBtn  = new QPushButton(jp("\xe9\x96\x8b\xe3\x81\x84\xe3\x81\xa6\xe5\x86\x8d\xe7\x94\x9f"));
    QPushButton *delBtn   = new QPushButton(jp("\xe5\x89\x8a\xe9\x99\xa4"));
    QPushButton *closeBtn = new QPushButton(jp("\xe9\x96\x89\xe3\x81\x98\xe3\x82\x8b"));
    hl->addWidget(openBtn); hl->addWidget(delBtn); hl->addStretch(); hl->addWidget(closeBtn);
    vl->addLayout(hl);
    connect(openBtn, &QPushButton::clicked, [this, lw, dlg]{
        auto *item = lw->currentItem();
        if (!item) return;
        stopIfCd();  // ★ CD再生中なら停止
        m_player->stop();
        turnOffBitPerfect();
        loadFolder(item->data(Qt::UserRole).toString(), false);
        dlg->accept();
    });
    connect(delBtn, &QPushButton::clicked, [this, lw]{
        auto *item = lw->currentItem();
        if (!item) return;
        m_favorites.remove(item->data(Qt::UserRole).toString());
        delete item;
        saveFavorites();
    });
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::accept);
    dlg->exec();
}

void MainWindow::saveFavorites()
{
    QString ini = QDir::homePath() + "/AlwaysPlayer.ini";
    QString tmp = QDir::homePath() + "/AlwaysPlayer.ini.tmp";
    QString bak = QDir::homePath() + "/AlwaysPlayer.ini.bak";
    QStringList lines;
    {
        QFile rf(ini);
        if (rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&rf);
            in.setEncoding(QStringConverter::Utf8);
            bool inFav = false;
            while (!in.atEnd()) {
                QString line = in.readLine();
                if (line.trimmed() == "[favorites]") { inFav = true; continue; }
                if (line.startsWith("[") && inFav) inFav = false;
                if (!inFav) lines << line;
            }
        }
    }
    {
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&f);
        out.setEncoding(QStringConverter::Utf8);
        for (const auto &l : lines) out << l << "\n";
        // sound_field 保存
        out << "[sound_field]\n";
        out << "value=" << m_soundField << "\n";
        out << "[favorites]\n";
        for (auto it = m_favorites.begin(); it != m_favorites.end(); ++it)
            out << it.key() << "|" << it.value() << "\n";
        out.flush(); f.flush();
    }
    QFile::remove(bak);
    QFile::copy(ini, bak);
    QFile::remove(ini);
    QFile::rename(tmp, ini);
}

void MainWindow::loadFavorites()
{
    m_favorites.clear();
    QString ini = QDir::homePath() + "/AlwaysPlayer.ini";
    QFile f(ini);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    bool inFav = false;
    bool inSound = false;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line == "[sound_field]") { inSound = true; inFav = false; continue; }
        if (line == "[favorites]") { inFav = true; inSound = false; continue; }
        if (line.startsWith("[")) { inFav = false; inSound = false; continue; }
        if (inSound && line.startsWith("value="))
            m_soundField = line.mid(6);
        if (inFav && line.contains("|")) {
            int sep = line.indexOf("|");
            m_favorites[line.left(sep)] = line.mid(sep + 1);
        }
    }
    // 読み込んだ音場効果をPlayerに反映
    if (m_player && !m_soundField.isEmpty())
        m_player->setMode("dsd8", false, false, m_soundField);
}

QString MainWindow::currentMode() const
{
    for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it)
        if (it.value()->isChecked()) return it.key();
    return "dsd8";
}

void MainWindow::onSleepTimer()
{
    stopCdStream(); clearCdState();  // ★ CD停止
    QMenu *menu = new QMenu(this);
    auto addItem = [&](const QString &label, int minutes) {
        menu->addAction(label, [this, minutes] {
            if (m_sleepTimer) { m_sleepTimer->stop(); delete m_sleepTimer; m_sleepTimer = nullptr; }
            m_sleepSeconds = minutes * 60;
            m_sleepTimer = new QTimer(this);
            connect(m_sleepTimer, &QTimer::timeout, this, [this] {
                m_sleepSeconds--;
                int m = m_sleepSeconds / 60, s = m_sleepSeconds % 60;
                m_sleepBtn->setText(QString("%1:%2")
                    .arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')));
                if (m_sleepSeconds <= 0) {
                    m_sleepTimer->stop();
                    m_sleepBtn->setText(QString::fromUtf8("\xf0\x9f\x8c\x99 Timer"));
                    m_player->stop();
                    QMessageBox msgBox(this);
                    msgBox.setWindowTitle(QString::fromUtf8("\xe3\x81\x8a\xe4\xbc\x91\xe3\x81\xbf\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc"));
                    msgBox.setText(QString::fromUtf8("\xe3\x82\xbf\xe3\x82\xa4\xe3\x83\x9e\xe3\x83\xbc\xe7\xb5\x82\xe4\xba\x86"));
                    QPushButton *sdBtn  = msgBox.addButton(QString::fromUtf8("\xe3\x82\xb7\xe3\x83\xa3\xe3\x83\x83\xe3\x83\x88\xe3\x83\x80\xe3\x82\xa6\xe3\x83\xb3"), QMessageBox::AcceptRole);
                    QPushButton *slBtn  = msgBox.addButton(QString::fromUtf8("\xe3\x82\xb9\xe3\x83\xaa\xe3\x83\xbc\xe3\x83\x97"), QMessageBox::AcceptRole);
                    msgBox.addButton(QString::fromUtf8("\xe4\xbd\x95\xe3\x82\x82\xe3\x81\x97\xe3\x81\xaa\xe3\x81\x84"), QMessageBox::RejectRole);
                    msgBox.exec();
                    if (msgBox.clickedButton() == sdBtn)
                        QProcess::startDetached("shutdown", {"/s", "/t", "30"});
                    else if (msgBox.clickedButton() == slBtn)
                        QProcess::startDetached("rundll32", {"powrprof.dll,SetSuspendState", "0,1,0"});
                }
            });
            m_sleepTimer->start(1000);
            m_sleepBtn->setText(QString("%1:00").arg(minutes, 2, 10, QChar('0')));
        });
    };
    addItem(QString::fromUtf8("15\xe5\x88\x86"), 15);
    addItem(QString::fromUtf8("30\xe5\x88\x86"), 30);
    addItem(QString::fromUtf8("60\xe5\x88\x86"), 60);
    addItem(QString::fromUtf8("90\xe5\x88\x86"), 90);
    menu->addSeparator();
    menu->addAction("OFF", [this] {
        if (m_sleepTimer) { m_sleepTimer->stop(); delete m_sleepTimer; m_sleepTimer = nullptr; }
        m_sleepBtn->setText(QString::fromUtf8("\xf0\x9f\x8c\x99 Timer"));
    });
    menu->exec(QCursor::pos());
}

// ── 検索用テキスト正規化（小文字化・全角半角統一・カタカナ→ひらがな）
QString MainWindow::normalizeForSearch(const QString &text) const
{
    QString s = text.toLower();

    // 全角英数字・記号 → 半角
    QString result;
    result.reserve(s.size());
    for (QChar c : s) {
        ushort u = c.unicode();
        // 全角英数字 ！～ (FF01-FF5E) → 半角 (21-7E)
        if (u >= 0xFF01 && u <= 0xFF5E)
            result += QChar(u - 0xFEE0);
        // 全角スペース → 半角スペース
        else if (u == 0x3000)
            result += ' ';
        // カタカナ → ひらがな (ァ-ン: 30A1-30F6 → 3041-3096)
        else if (u >= 0x30A1 && u <= 0x30F6)
            result += QChar(u - 0x60);
        else
            result += c;
    }
    return result;
}

// ── アルバムカードをフィルタリング
void MainWindow::filterAlbumCards(const QString &query)
{
    QString norm = normalizeForSearch(query.trimmed());
    QStringList tokens = norm.split(' ', Qt::SkipEmptyParts);

    // グリッドからいったん全カードを取り外す
    for (auto &info : m_albumCards)
        m_albumGridLayout->removeWidget(info.card);

    // マッチするカードだけ2列グリッドに再配置
    int col = 0, row = 0;
    int shown = 0;
    for (auto &info : m_albumCards) {
        bool match = tokens.isEmpty();
        if (!match) {
            match = true;
            for (const QString &token : tokens) {
                if (!info.searchKey.contains(token)) { match = false; break; }
            }
        }
        if (match) {
            info.card->setVisible(true);
            m_albumGridLayout->addWidget(info.card, row, col);
            col++;
            if (col >= 2) { col = 0; row++; }
            shown++;
        } else {
            info.card->setVisible(false);
        }
    }

    if (tokens.isEmpty())
        m_albumFooterLabel->setText(
            QString("%1 %2").arg(m_albumCards.size())
                .arg(jp("\xe3\x82\xa2\xe3\x83\xab\xe3\x83\x90\xe3\x83\xa0 \xe2\x80\x94 \xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x83\xe3\x82\xaf\xe3\x81\xa7\xe8\xaa\xad\xe3\x81\xbf\xe8\xbe\xbc\xe3\x82\x93\xe3\x81\xa7\xe5\x86\x8d\xe7\x94\x9f")));
    else
        m_albumFooterLabel->setText(
            QString("%1 / %2 %3").arg(shown).arg(m_albumCards.size())
                .arg(jp("\xe3\x82\xa2\xe3\x83\xab\xe3\x83\x90\xe3\x83\xa0")));
}

// ── 現在の電源プランGUIDを取得
static QString getCurrentPowerPlan()
{
    GUID *pGuid = nullptr;
    if (PowerGetActiveScheme(nullptr, &pGuid) == ERROR_SUCCESS && pGuid) {
        WCHAR buf[64] = {};
        StringFromGUID2(*pGuid, buf, 64);
        LocalFree(pGuid);
        return QString::fromWCharArray(buf);
    }
    return {};
}

// ── 電源プランGUID定数
static const QString PLAN_HIGH   = "{8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c}";
static const QString PLAN_BALANCE = "{381b4222-f694-41f0-9685-ff5bb260df2e}";
static const QString PLAN_SAVER  = "{a1841308-3541-4fab-bc81-f71556f20b4a}";

// ── レジストリDWORDを読む
static DWORD readRegDword(HKEY root, const QString &path, const QString &name, DWORD def = 0)
{
    HKEY hKey;
    if (RegOpenKeyExW(root, reinterpret_cast<LPCWSTR>(path.utf16()),
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) return def;
    DWORD val = def, sz = sizeof(DWORD), type = REG_DWORD;
    RegQueryValueExW(hKey, reinterpret_cast<LPCWSTR>(name.utf16()),
                     nullptr, &type, (LPBYTE)&val, &sz);
    RegCloseKey(hKey);
    return val;
}

// ── レジストリDWORDを書く（管理者権限必要）
static bool writeRegDword(HKEY root, const QString &path, const QString &name, DWORD val)
{
    HKEY hKey;
    if (RegOpenKeyExW(root, reinterpret_cast<LPCWSTR>(path.utf16()),
                      0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExW(hKey, reinterpret_cast<LPCWSTR>(name.utf16()),
                              0, REG_DWORD, (LPBYTE)&val, sizeof(DWORD)) == ERROR_SUCCESS;
    RegCloseKey(hKey);
    return ok;
}

void MainWindow::showSettings()
{
    QDialog *dlg = new QDialog(this);
    dlg->setWindowTitle("Settings — Always Player");
    dlg->setMinimumWidth(480);
    dlg->setStyleSheet(styleSheet());

    QVBoxLayout *vl = new QVBoxLayout(dlg);
    vl->setSpacing(16);

    // ── 電源プラン
    auto *grpPower = new QGroupBox("電源プラン");
    auto *pvl = new QVBoxLayout(grpPower);
    QString curPlan = getCurrentPowerPlan().toLower();

    auto *rbHigh    = new QRadioButton("高パフォーマンス（デスクトップ推奨）");
    auto *rbBalance = new QRadioButton("バランス（ノートPC推奨）");
    auto *rbSaver   = new QRadioButton("省電力");

    if (curPlan.contains("8c5e7fda")) rbHigh->setChecked(true);
    else if (curPlan.contains("381b4222")) rbBalance->setChecked(true);
    else rbSaver->setChecked(true);

    pvl->addWidget(rbHigh);
    pvl->addWidget(rbBalance);
    pvl->addWidget(rbSaver);

    auto *applyPowerBtn = new QPushButton("電源プランを変更（管理者権限が必要）");
    applyPowerBtn->setObjectName("toolBtn");
    pvl->addWidget(applyPowerBtn);

    connect(applyPowerBtn, &QPushButton::clicked, dlg, [=]() {
        QString guid = rbHigh->isChecked() ? PLAN_HIGH :
                       rbBalance->isChecked() ? PLAN_BALANCE : PLAN_SAVER;
        QString cmd = QString("powercfg /setactive %1").arg(guid);
        // 管理者権限でpowercfgを実行
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = L"powercfg.exe";
        QString params = QString("/setactive %1").arg(guid);
        sei.lpParameters = reinterpret_cast<LPCWSTR>(params.utf16());
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei))
            QMessageBox::information(dlg, "完了", "電源プランを変更しました。");
        else
            QMessageBox::warning(dlg, "エラー", "変更に失敗しました。");
    });

    vl->addWidget(grpPower);

    // ── USB最適化
    auto *grpUsb = new QGroupBox("USB最適化（管理者権限が必要）");
    auto *uvl = new QVBoxLayout(grpUsb);

    // USBセレクティブサスペンドの現在値
    DWORD suspendVal = readRegDword(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Services\\USB",
        "DisableSelectiveSuspend", 0);
    auto *cbSuspend = new QCheckBox("USBセレクティブサスペンドを無効化（USB電源を安定させる）");
    cbSuspend->setChecked(suspendVal == 1);
    uvl->addWidget(cbSuspend);

    // 高速スタートアップの現在値
    DWORD fastBoot = readRegDword(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
        "HiberbootEnabled", 1);
    auto *cbFastBoot = new QCheckBox("高速スタートアップを無効化（USB初期化を正常化する）");
    cbFastBoot->setChecked(fastBoot == 0);
    uvl->addWidget(cbFastBoot);

    auto *applyUsbBtn = new QPushButton("USB設定を適用（管理者権限が必要）");
    applyUsbBtn->setObjectName("toolBtn");
    uvl->addWidget(applyUsbBtn);

    connect(applyUsbBtn, &QPushButton::clicked, dlg, [=]() {
        auto runReg = [](const QString &args) {
            SHELLEXECUTEINFOW s = {};
            s.cbSize = sizeof(s);
            s.lpVerb = L"runas";
            s.lpFile = L"reg.exe";
            s.lpParameters = reinterpret_cast<LPCWSTR>(args.utf16());
            s.nShow  = SW_HIDE;
            s.fMask  = SEE_MASK_NOCLOSEPROCESS;
            if (ShellExecuteExW(&s)) {
                WaitForSingleObject(s.hProcess, 3000);
                CloseHandle(s.hProcess);
            }
        };
        int suspendD  = cbSuspend->isChecked()  ? 1 : 0;
        int fastBootD = cbFastBoot->isChecked() ? 0 : 1;
        runReg(QString::fromLatin1(
            "add HKLM\\SYSTEM\\CurrentControlSet\\Services\\USB"
            " /v DisableSelectiveSuspend /t REG_DWORD /d %1 /f").arg(suspendD));
        runReg(QString::fromLatin1(
            "add HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power"
            " /v HiberbootEnabled /t REG_DWORD /d %1 /f").arg(fastBootD));
        QMessageBox::information(dlg, "完了",
            "USB設定を適用しました。\n一部の設定は再起動後に有効になります。");
    });

    vl->addWidget(grpUsb);

    // ── 音場効果グループ
    auto *grpSound = new QGroupBox(QString::fromUtf8("音場効果（オプション）"));
    auto *svl = new QVBoxLayout(grpSound);
    auto *cbWow  = new QCheckBox(QString::fromUtf8("ワウフラッター（アナログレコード化）"));
    auto *cbHall = new QCheckBox(QString::fromUtf8("ホールトーン（3メーター以内の試聴で強化）"));
    if (m_soundField == "wowflutter") cbWow->setChecked(true);
    else if (m_soundField == "halltone") cbHall->setChecked(true);
    svl->addWidget(cbWow);
    svl->addWidget(cbHall);
    connect(cbWow, &QCheckBox::clicked, this, [this, cbWow, cbHall](bool checked){
        if (checked) { cbHall->setChecked(false); m_soundField = "wowflutter"; }
        else m_soundField = "";
        turnOffBitPerfect();
        m_player->setMode(currentMode(), m_hp1On, m_hp2On, m_soundField);
    });
    connect(cbHall, &QCheckBox::clicked, this, [this, cbWow, cbHall](bool checked){
        if (checked) { cbWow->setChecked(false); m_soundField = "halltone"; }
        else m_soundField = "";
        turnOffBitPerfect();
        m_player->setMode(currentMode(), m_hp1On, m_hp2On, m_soundField);
    });
    vl->addWidget(grpSound);

    // ── デフォルトに戻すボタン
    auto *resetBtn = new QPushButton(QString::fromUtf8("\xe3\x81\x99\xe3\x81\xb9\xe3\x81\xa6\xe3\x82\x92\xe3\x83\x87\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x88\xe3\x81\xab\xe6\x88\xbb\xe3\x81\x99"));
    resetBtn->setObjectName("toolBtn");
    connect(resetBtn, &QPushButton::clicked, dlg, [=]() {
        // 電源プランをバランスに戻す
        SHELLEXECUTEINFOW seiR = {};
        seiR.cbSize  = sizeof(seiR);
        seiR.lpVerb  = L"runas";
        seiR.lpFile  = L"powercfg.exe";
        seiR.lpParameters = L"/setactive 381b4222-f694-41f0-9685-ff5bb260df2e";
        seiR.nShow   = SW_HIDE;
        ShellExecuteExW(&seiR);

        // reg コマンドを直接実行（USBサスペンド・高速スタートアップを元に戻す）
        auto runReg = [](const QString &args) {
            SHELLEXECUTEINFOW s = {};
            s.cbSize = sizeof(s);
            s.lpVerb = L"runas";
            s.lpFile = L"reg.exe";
            s.lpParameters = reinterpret_cast<LPCWSTR>(args.utf16());
            s.nShow  = SW_HIDE;
            s.fMask  = SEE_MASK_NOCLOSEPROCESS;
            if (ShellExecuteExW(&s)) {
                WaitForSingleObject(s.hProcess, 3000);
                CloseHandle(s.hProcess);
            }
        };
        runReg(QString::fromLatin1(
            "add HKLM\\SYSTEM\\CurrentControlSet\\Services\\USB"
            " /v DisableSelectiveSuspend /t REG_DWORD /d 0 /f"));
        runReg(QString::fromLatin1(
            "add HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power"
            " /v HiberbootEnabled /t REG_DWORD /d 1 /f"));

        // UIを元に戻す
        rbBalance->setChecked(true);
        cbSuspend->setChecked(false);
        cbFastBoot->setChecked(false);
        QMessageBox::information(dlg, "完了",
            "すべての設定をデフォルトに戻しました。\n再起動をお勧めします。");
    });
    vl->addWidget(resetBtn);

    // ── About セクション
    auto *line = new QFrame();
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    vl->addWidget(line);

    auto *aboutLabel = new QLabel(
        QString("Always Player v6.0.0\n"
                "build %1\n\n"
                "High Fidelity PC Audio Player\n\n"
                "(c) 2026 YOUICHI SAIJO  GPL-3.0")
        .arg(QString::fromLatin1(BUILD_TIMESTAMP)));
    aboutLabel->setAlignment(Qt::AlignCenter);
    aboutLabel->setObjectName("infoLabel");
    vl->addWidget(aboutLabel);

    auto *urlLabel = new QLabel(
        "<a href='https://always-player.sakuraweb.com/' "
        "style='color:#4db8ff;'>always-player.sakuraweb.com</a>");
    urlLabel->setOpenExternalLinks(true);
    urlLabel->setAlignment(Qt::AlignCenter);
    urlLabel->setObjectName("infoLabel");
    vl->addWidget(urlLabel);

    // ── 閉じるボタン
    auto *closeBtn = new QPushButton(QString::fromUtf8("\xe9\x96\x89\xe3\x81\x98\xe3\x82\x8b"));
    closeBtn->setObjectName("toolBtn");
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::accept);
    vl->addWidget(closeBtn);

    dlg->exec();
    dlg->deleteLater();
}

void MainWindow::updateModeDesc(const QString &mode)
{
    if (!m_modeDesc) return;
    if (mode == "pure")
        m_modeDesc->setText(jp("\xe3\x83\x94\xe3\x83\xa5\xe3\x82\xa2\xe3\x83\xa2\xe3\x83\xbc\xe3\x83\x89 / \xe3\x82\xbd\xe3\x83\xbc\xe3\x82\xb9\xe5\xbf\xa0\xe5\xae\x9f\xe5\x86\x8d\xe7\x94\x9f"));
    else if (mode == "hires4")
        m_modeDesc->setText(jp("4\xe5\x80\x8d\xe3\x82\xa2\xe3\x83\x83\xe3\x83\x97\xe3\x82\xb5\xe3\x83\xb3\xe3\x83\x97\xe3\x83\xaa\xe3\x83\xb3\xe3\x82\xb0 / \xe3\x83\x8f\xe3\x82\xa4\xe3\x83\xac\xe3\x82\xbe\xe5\x87\xba\xe5\x8a\x9b"));
    else if (mode == "dsd8")
        m_modeDesc->setText(jp("8\xe5\x80\x8d\xe3\x82\xa2\xe3\x83\x83\xe3\x83\x97\xe3\x82\xb5\xe3\x83\xb3\xe3\x83\x97\xe3\x83\xaa\xe3\x83\xb3\xe3\x82\xb0 / \xe3\x83\x8e\xe3\x82\xa4\xe3\x82\xba\xe3\x82\xb7\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\x94\xe3\x83\xb3\xe3\x82\xb0 / \xe7\x96\x91\xe4\xbc\xbc" "DSD"));
    else if (mode == "loudness")
        m_modeDesc->setText(jp("\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\x89\xe3\x83\x8d\xe3\x82\xb9\xe6\xad\xa3\xe8\xa6\x8f\xe5\x8c\x96 / 4\xe5\x80\x8d\xe3\x82\xa2\xe3\x83\x83\xe3\x83\x97\xe3\x82\xb5\xe3\x83\xb3\xe3\x83\x97\xe3\x83\xaa\xe3\x83\xb3\xe3\x82\xb0"));
}

void MainWindow::turnOffBitPerfect()
{
    if (!m_bpActOff || m_bpActOff->isChecked()) return;
    // UI update (GUI thread)
    m_bpActOff->setChecked(true);
    m_bitPerfectBtn->setText("BitPerfect ▼");
    // mpv property change in worker thread (WASAPI device reinit)
    QtConcurrent::run([this]{
        mpv_set_property_string(m_player->mpvHandle(), "audio-exclusive", "no");
    });
}

void MainWindow::onCdMetaReady(CdMetaFetcher::Result result)
{
    if (!m_isCdMode) return;

    if (!result.found) {
        m_statusBar->setText(jp("\xe3\x83\xa1\xe3\x82\xbf\xe3\x83\x87\xe3\x83\xbc\xe3\x82\xbf\xe3\x81\xaa\xe3\x81\x97  >> Press Play to start"));
        return;
    }

    for (int i = 0; i < result.trackNames.size() && i < m_discInfo.tracks.size(); ++i)
        m_discInfo.tracks[i].title = result.trackNames[i];

    m_allItems.clear();
    m_playlist->clear();
    for (int i = 0; i < m_cdTrackCount; ++i) {
        QString trackNum = QString("%1. ").arg(i + 1, 2, 10, QChar('0'));
        QString name = trackNum + ((i < result.trackNames.size() && !result.trackNames[i].isEmpty())
            ? result.trackNames[i]
            : QString("Track %1").arg(i + 1, 2, 10, QChar('0')));
        m_allItems << name;
        QListWidgetItem *item = new QListWidgetItem(name);
        item->setData(Qt::UserRole, i);
        m_playlist->addItem(item);
    }
    m_playlist->setCurrentRow(m_cdCurrentTrack);

    QString titleText = result.albumTitle;
    if (!result.year.isEmpty()) titleText += QString("  (%1)").arg(result.year);
    m_title->setText(titleText.isEmpty() ? "CD" : titleText);

    QString sub = QString("%1 Tracks").arg(m_cdTrackCount);
    if (!result.artist.isEmpty()) sub += "   " + result.artist;
    m_subTitle->setText(sub);

    if (m_artistInfoBtn && !result.artist.isEmpty()) {
        m_currentArtist = result.artist;
        m_artistInfoBtn->setEnabled(true);
    }

    if (!result.coverArt.isNull()) {
        m_jacket->setPixmap(result.coverArt.scaled(200, 200,
            Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_hasArtwork = true;
        m_stack->setCurrentIndex(1);
    }

    m_statusBar->setText(jp(">> Press Play to start"));
    qDebug() << "[onCdMetaReady] album=" << result.albumTitle
             << "artist=" << result.artist
             << "tracks=" << result.trackNames.size()
             << "art=" << !result.coverArt.isNull();
}

void MainWindow::showAbout()
{
    QString buildTs = QString::fromLatin1(BUILD_TIMESTAMP);
    QMessageBox::about(this, "Always Player v6.0.0",
        QString("Always Player v6.0.0\n"
                "build %1\n\n"
                "High Fidelity PC Audio Player\n\n"
                "(c) 2026 YOUICHI SAIJO -- GPL-3.0\n\n"
                "https://always-player.sakuraweb.com/")
        .arg(buildTs));
}

// ── フォルダ内の最初の音楽ファイルからタグ埋め込みアートを取得
QPixmap MainWindow::findAlbumArt(const QString &folderPath, int size)
{
    static const QStringList audioExts = {
        "*.mp3","*.flac","*.m4a","*.mp4","*.aac","*.wav","*.ogg","*.opus","*.dsf","*.dff",
        "*.MP3","*.FLAC","*.M4A","*.MP4","*.AAC","*.WAV","*.OGG","*.OPUS","*.DSF","*.DFF"
    };

    // フォルダ内（サブフォルダも含む）の最初の音楽ファイルを探す
    std::function<QString(const QString&, int)> findFirst = [&](const QString &dir, int depth) -> QString {
        if (depth > 2) return {};
        QDir d(dir);
        d.setNameFilters(audioExts);
        d.setFilter(QDir::Files | QDir::NoDotAndDotDot);
        d.setSorting(QDir::Name);
        auto files = d.entryInfoList();
        if (!files.isEmpty()) return files.first().absoluteFilePath();
        d.setNameFilters({});
        d.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
        d.setSorting(QDir::Name);
        for (const auto &sub : d.entryInfoList()) {
            QString f = findFirst(sub.absoluteFilePath(), depth + 1);
            if (!f.isEmpty()) return f;
        }
        return {};
    };

    QString fp = findFirst(folderPath, 0);
    if (fp.isEmpty()) return QPixmap();

    QString ext = QFileInfo(fp).suffix().toLower();
    QByteArray imgData;

    auto toQByteArray = [](const TagLib::ByteVector &bv) {
        return QByteArray(bv.data(), (int)bv.size());
    };

    if (ext == "mp3") {
        TagLib::MPEG::File f(fp.toStdWString().c_str());
        if (f.ID3v2Tag()) {
            auto frames = f.ID3v2Tag()->frameListMap()["APIC"];
            TagLib::ID3v2::AttachedPictureFrame *best = nullptr;
            for (auto *fr : frames) {
                auto *apic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(fr);
                if (!apic) continue;
                if (apic->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) { best = apic; break; }
                if (!best) best = apic;
            }
            if (best) imgData = toQByteArray(best->picture());
        }
    } else if (ext == "flac") {
        TagLib::FLAC::File f(fp.toStdWString().c_str());
        TagLib::FLAC::Picture *best = nullptr;
        for (auto *pic : f.pictureList()) {
            if (pic->type() == TagLib::FLAC::Picture::FrontCover) { best = pic; break; }
            if (!best) best = pic;
        }
        if (best) imgData = toQByteArray(best->data());
    } else if (ext == "m4a" || ext == "mp4" || ext == "aac") {
        TagLib::MP4::File f(fp.toStdWString().c_str());
        if (f.tag()) {
            auto items = f.tag()->itemMap();
            if (items.contains("covr")) {
                auto covers = items["covr"].toCoverArtList();
                if (!covers.isEmpty()) imgData = toQByteArray(covers.front().data());
            }
        }
    }

    if (!imgData.isEmpty()) {
        QPixmap px;
        if (px.loadFromData(imgData))
            return px.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                     .copy(0, 0, size, size);
    }

    // fallback: フォルダ内の画像ファイル
    QDir dir(folderPath);
    static const QStringList preferred = {"cover.jpg","folder.jpg","front.jpg","cover.png","folder.png"};
    for (const QString &name : preferred) {
        QPixmap px(dir.filePath(name));
        if (!px.isNull())
            return px.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                     .copy(0, 0, size, size);
    }
    for (const QString &f : dir.entryList({"*.jpg","*.jpeg","*.png"}, QDir::Files)) {
        QPixmap px(dir.filePath(f));
        if (!px.isNull())
            return px.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                     .copy(0, 0, size, size);
    }
    return QPixmap();
}

// ── アルバムブラウザを開く
void MainWindow::onBrowseAlbums()
{
    // WASAPI排他モードを先行解除（ダイアログ表示中のフリーズ防止）
    turnOffBitPerfect();

    QString root = QFileDialog::getExistingDirectory(
        this, jp("\xe9\x9f\xb3\xe6\xa5\xbd\xe3\x83\xab\xe3\x83\xbc\xe3\x83\x88\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e"),
        m_currentFolder, QFileDialog::ShowDirsOnly);
    if (root.isEmpty()) return;

    setupAlbumBrowser();
    populateAlbumBrowser(root);

    // pageStackのindex1へ切り替え
    QStackedWidget *ps = qobject_cast<QStackedWidget*>(m_mainContent->parentWidget());
    if (ps) ps->setCurrentIndex(1);
}

// ── アルバムブラウザUIの骨格を構築（初回のみ）
void MainWindow::setupAlbumBrowser()
{
    // 既存の中身をクリア
    qDeleteAll(m_albumBrowser->children());
    QVBoxLayout *bl = new QVBoxLayout(m_albumBrowser);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(0);

    // ヘッダー
    QWidget *header = new QWidget();
    header->setObjectName("albumHeader");
    header->setFixedHeight(34);
    QHBoxLayout *hl = new QHBoxLayout(header);
    hl->setContentsMargins(14, 0, 10, 0);
    hl->setSpacing(8);
    QLabel *titleLbl = new QLabel(jp("\xe3\x82\xa2\xe3\x83\xab\xe3\x83\x90\xe3\x83\xa0\xe4\xb8\x80\xe8\xa6\xa7"));
    titleLbl->setObjectName("albumHeaderLabel");
    m_albumPathLabel = new QLabel();
    m_albumPathLabel->setObjectName("albumPathLabel");
    QPushButton *closeBtn = new QPushButton(jp("\xe2\x9c\x95 \xe9\x96\x89\xe3\x81\x98\xe3\x82\x8b"));
    closeBtn->setObjectName("albumCloseBtn");
    closeBtn->setFixedHeight(22);
    connect(closeBtn, &QPushButton::clicked, [this]{
        QStackedWidget *ps = qobject_cast<QStackedWidget*>(m_mainContent->parentWidget());
        if (ps) ps->setCurrentIndex(0);
    });
    hl->addWidget(titleLbl);
    hl->addWidget(m_albumPathLabel, 1);
    hl->addWidget(closeBtn);
    bl->addWidget(header);

    // 検索ボックス
    QWidget *searchBar = new QWidget();
    searchBar->setObjectName("albumSearchBar");
    searchBar->setFixedHeight(38);
    QHBoxLayout *sl = new QHBoxLayout(searchBar);
    sl->setContentsMargins(14, 5, 14, 5);
    sl->setSpacing(0);
    m_albumSearchBox = new QLineEdit();
    m_albumSearchBox->setObjectName("albumSearchBox");
    m_albumSearchBox->setPlaceholderText(jp("\xe3\x82\xa2\xe3\x83\xab\xe3\x83\x90\xe3\x83\xa0\xe5\x90\x8d\xe3\x83\xbb\xe3\x82\xa2\xe3\x83\xbc\xe3\x83\x86\xe3\x82\xa3\xe3\x82\xb9\xe3\x83\x88\xe5\x90\x8d\xe3\x81\xa7\xe6\xa4\x9c\xe7\xb4\xa2..."));
    m_albumSearchBox->setClearButtonEnabled(true);
    connect(m_albumSearchBox, &QLineEdit::textChanged, this, &MainWindow::filterAlbumCards);
    sl->addWidget(m_albumSearchBox);
    bl->addWidget(searchBar);
    QScrollArea *scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget *inner = new QWidget();
    inner->setObjectName("albumInner");
    QVBoxLayout *innerL = new QVBoxLayout(inner);
    innerL->setContentsMargins(14, 10, 14, 10);
    innerL->setSpacing(0);

    m_albumGrid = new QWidget();
    m_albumGridLayout = new QGridLayout(m_albumGrid);
    m_albumGridLayout->setSpacing(8);
    m_albumGridLayout->setContentsMargins(0, 0, 0, 0);

    innerL->addWidget(m_albumGrid);
    innerL->addStretch();
    scroll->setWidget(inner);
    bl->addWidget(scroll, 1);

    // フッター
    QWidget *footer = new QWidget();
    footer->setObjectName("albumFooter");
    footer->setFixedHeight(24);
    QHBoxLayout *fl = new QHBoxLayout(footer);
    fl->setContentsMargins(14, 0, 14, 0);
    m_albumFooterLabel = new QLabel();
    m_albumFooterLabel->setObjectName("albumFooterLabel");
    m_albumFooterLabel->setAlignment(Qt::AlignCenter);
    fl->addWidget(m_albumFooterLabel);
    bl->addWidget(footer);
}

// ── アルバムグリッドを生成
void MainWindow::populateAlbumBrowser(const QString &rootPath)
{
    m_albumPathLabel->setText(rootPath);
    m_albumSearchBox->clear();
    m_albumCards.clear();

    // グリッドをクリア
    QLayoutItem *item;
    while ((item = m_albumGridLayout->takeAt(0)) != nullptr) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    // ── 自然順ソート（Natural Sort）────────────────────────────
    // "1, 2, 10" を文字列順 "1, 10, 2" ではなく人間が期待する順に並べる
    auto naturalLessThan = [](const QString &a, const QString &b) -> bool {
        int ia = 0, ib = 0;
        while (ia < a.size() && ib < b.size()) {
            if (a[ia].isDigit() && b[ib].isDigit()) {
                // 数字部分を数値として比較
                int na = 0, nb = 0;
                while (ia < a.size() && a[ia].isDigit()) na = na * 10 + a[ia++].digitValue();
                while (ib < b.size() && b[ib].isDigit()) nb = nb * 10 + b[ib++].digitValue();
                if (na != nb) return na < nb;
            } else {
                // 文字部分は大文字小文字を無視して比較
                QChar ca = a[ia++].toLower();
                QChar cb = b[ib++].toLower();
                if (ca != cb) return ca < cb;
            }
        }
        return a.size() < b.size();
    };

    auto naturalEntryList = [&](const QDir &d) -> QStringList {
        QStringList list = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        std::sort(list.begin(), list.end(), naturalLessThan);
        return list;
    };
    // ────────────────────────────────────────────────────────────

    QDir root(rootPath);
    QStringList genres = naturalEntryList(root);

    // DISCフォルダ判定ヘルパー
    auto isDiscDir = [](const QString &name) -> bool {
        QString n = name.toLower();
        return n.startsWith("disc") || n.startsWith("disk") ||
               n.startsWith("cd") || n.startsWith("side");
    };

    // ジャンル階層判定：
    // サブフォルダの中に「さらにサブフォルダを持つもの」が過半数ならジャンル階層
    // Various Artistsのような単発アーティストフォルダは誤検知しない
    int genreCount = 0;
    int artistFolderCount = 0;
    for (const QString &g : genres) {
        QDir gd(root.filePath(g));
        QStringList subs = gd.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QStringList nonDiscSubs;
        for (const QString &s : subs)
            if (!isDiscDir(s)) nonDiscSubs << s;
        if (!nonDiscSubs.isEmpty()) artistFolderCount++;
        genreCount++;
    }
    // 過半数がサブフォルダを持つ場合のみジャンル階層とみなす
    bool hasGenres = (genreCount > 0) && (artistFolderCount * 2 > genreCount);

    int col = 0, row = 0;
    int albumCount = 0;
    const int COLS = 2;
    const int ART_SIZE = 72;

    auto addGenreLabel = [&](const QString &label) {
        if (col != 0) { col = 0; row++; }
        QLabel *gl = new QLabel(label.toUpper());
        gl->setObjectName("genreLabel");
        gl->setFixedHeight(24);
        m_albumGridLayout->addWidget(gl, row, 0, 1, COLS);
        row++;
        col = 0;
    };

    auto isDiscFolder = [&isDiscDir](const QString &name) -> bool {
        return isDiscDir(name);
    };

    auto addAlbumCard = [&](const QString &folderPath, const QString &albumName, const QString &genreName = QString()) {
        albumCount++;
        QDir adir(folderPath);
        QStringList subDirs = naturalEntryList(adir);

        // 音楽ファイルを数える（直下 + DISCサブフォルダ含む）
        static const QStringList exts = {"*.mp3","*.flac","*.wav","*.aac","*.m4a",
                                          "*.ogg","*.opus","*.dsf","*.dff"};
        int trackCount = 0;
        for (const QString &ext : exts)
            trackCount += adir.entryList({ext}, QDir::Files).size();
        for (const QString &sd : subDirs) {
            if (!isDiscFolder(sd)) continue;
            QDir dd(adir.filePath(sd));
            for (const QString &ext : exts)
                trackCount += dd.entryList({ext}, QDir::Files).size();
        }

        // 0曲フォルダ（対応フォーマットなし）はスキップ
        if (trackCount == 0) { albumCount--; return; }

        // サブテキスト：曲数表示
        QString subText = QString("%1 %2").arg(trackCount).arg(jp("\xe6\x9b\xb2"));
        QWidget *card = new QWidget();
        card->setObjectName("albumCard");
        card->setCursor(Qt::PointingHandCursor);
        QHBoxLayout *cardL = new QHBoxLayout(card);
        cardL->setContentsMargins(8, 8, 8, 8);
        cardL->setSpacing(10);

        // アートワーク
        QLabel *artLbl = new QLabel();
        artLbl->setObjectName("albumArtLabel");
        artLbl->setFixedSize(ART_SIZE, ART_SIZE);
        artLbl->setAlignment(Qt::AlignCenter);
        artLbl->setText("\xe2\x99\xaa");  // まず即座に♪プレースホルダーを表示

        // アートワーク読み込みは別スレッドで（TagLib処理がGUIをブロックしない）
        QPointer<QLabel> artPtr = artLbl;  // カード削除時のダングリングポインタ防止
        QtConcurrent::run([this, folderPath, artPtr, ART_SIZE]() {
            QPixmap art = findAlbumArt(folderPath, ART_SIZE);
            if (!art.isNull()) {
                QMetaObject::invokeMethod(qApp, [artPtr, art]() {
                    if (artPtr)  // ウィジェットがまだ生きていれば反映
                        artPtr->setPixmap(art);
                }, Qt::QueuedConnection);
            }
        });

        // テキスト情報
        QWidget *infoW = new QWidget();
        QVBoxLayout *infoL = new QVBoxLayout(infoW);
        infoL->setContentsMargins(0, 0, 0, 0);
        infoL->setSpacing(2);
        QLabel *nameLbl = new QLabel(albumName);
        nameLbl->setObjectName("albumNameLabel");
        nameLbl->setWordWrap(false);

        QLabel *subLbl = new QLabel(subText);
        subLbl->setObjectName("albumSubLabel");

        infoL->addStretch();
        infoL->addWidget(nameLbl);
        infoL->addWidget(subLbl);
        infoL->addStretch();

        cardL->addWidget(artLbl);
        cardL->addWidget(infoW, 1);

        // クリックで読み込み→メインUIへ戻る
        connect(new QObject(card), &QObject::destroyed, []{}); // dummy
        card->installEventFilter(this);
        card->setProperty("albumPath", folderPath);

        // 検索キーに登録（アルバム名＋ジャンル名を正規化）
        QString searchKey = normalizeForSearch(albumName);
        if (!genreName.isEmpty())
            searchKey += " " + normalizeForSearch(genreName);
        m_albumCards.append({card, searchKey});

        m_albumGridLayout->addWidget(card, row, col);
        col++;
        if (col >= COLS) { col = 0; row++; }
    };

    if (hasGenres) {
        for (const QString &genre : genres) {
            QString genrePath = root.filePath(genre);
            QDir gd(genrePath);
            QStringList albums = naturalEntryList(gd);
            albums.erase(std::remove_if(albums.begin(), albums.end(),
                [&](const QString &a){ return isDiscDir(a); }), albums.end());
            if (albums.isEmpty()) continue;
            addGenreLabel(genre);
            for (const QString &album : albums)
                addAlbumCard(gd.filePath(album), album, genre);
            if (col != 0) { col = 0; row++; }
        }
    } else {
        for (const QString &entry : genres) {
            if (isDiscDir(entry)) continue;
            QString entryPath = root.filePath(entry);
            QDir entryDir(entryPath);
            QStringList subs = naturalEntryList(entryDir);
            // DISCフォルダ以外のサブフォルダを持つ → アーティストフォルダとして展開
            QStringList nonDiscSubs;
            for (const QString &s : subs)
                if (!isDiscDir(s)) nonDiscSubs << s;
            if (!nonDiscSubs.isEmpty()) {
                // アーティスト名をラベルとして表示し、中のアルバムを展開
                addGenreLabel(entry);
                for (const QString &album : nonDiscSubs)
                    addAlbumCard(entryDir.filePath(album), album, entry);
                if (col != 0) { col = 0; row++; }
            } else {
                // 普通のアルバムフォルダ
                addAlbumCard(entryPath, entry);
            }
        }
    }

    m_albumFooterLabel->setText(
        QString("%1 %2").arg(albumCount).arg(jp("\xe3\x82\xa2\xe3\x83\xab\xe3\x83\x90\xe3\x83\xa0 \xe2\x80\x94 \xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x83\xe3\x82\xaf\xe3\x81\xa7\xe8\xaa\xad\xe3\x81\xbf\xe8\xbe\xbc\xe3\x82\x93\xe3\x81\xa7\xe5\x86\x8d\xe7\x94\x9f")));
}

