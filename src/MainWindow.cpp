#include "MainWindow.h"
#include <QPainter>
#include <QProxyStyle>
#include <QStyleOption>
#include "CdMetaFetcher.h"
#include "RemoteServer.h"
#include <QBuffer>
#include <QCollator>
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
#include <QMutex>
#include "IniFileLock.h"
#include <windows.h>
#include <io.h>        // _get_osfhandle（INI保存のFlushFileBuffers用）
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
#include <taglib/xiphcomment.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>

// ── チェックボックス・ラジオボタンをC++で直接描画するカスタムスタイル
class AlwaysStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    void drawControl(ControlElement element, const QStyleOption *option,
                     QPainter *painter, const QWidget *widget) const override
    {
        if (element == CE_CheckBox) {
            const auto *opt = qstyleoption_cast<const QStyleOptionButton*>(option);
            if (!opt) { QProxyStyle::drawControl(element, option, painter, widget); return; }

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);

            QRect indRect = subElementRect(SE_CheckBoxIndicator, opt, widget);
            QRect textRect = subElementRect(SE_CheckBoxContents, opt, widget);
            bool hovered = opt->state & State_MouseOver;
            bool checked = opt->state & State_On;

            QColor frameColor = hovered ? QColor("#3a8fe8") : QColor("#1a3a5a");
            QColor fillColor  = QColor("#0a1e30");
            QColor markColor  = QColor("#3a8fe8");
            QColor textColor  = hovered ? QColor("#40c8ff") : QColor("#7aaed8");

            QRect box = indRect.adjusted(1,1,-1,-1);

            // ── 背景（暗い塗りつぶし）
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#0a0a1a"));
            painter->drawRoundedRect(box, 3, 3);

            // ── 枠（ネオンブルー・角丸）
            painter->setPen(QPen(frameColor, 2.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(box, 3, 3);

            // ── チェック時：シアン塗りつぶし＋黒チェックマーク
            if (checked) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(fillColor);
                painter->drawRoundedRect(box, 3, 3);

                painter->setPen(QPen(markColor, 2.2, Qt::SolidLine,
                                     Qt::RoundCap, Qt::RoundJoin));
                QRect r = box.adjusted(3, 3, -3, -3);
                QPointF p1(r.left(),                      r.top() + r.height() * 0.5f);
                QPointF p2(r.left() + r.width() * 0.38f, r.bottom());
                QPointF p3(r.right(),                     r.top());
                painter->drawPolyline(QPolygonF({p1, p2, p3}));
            }

            // ── ラベルテキスト
            painter->setPen(textColor);
            if (widget) painter->setFont(widget->font());
            painter->drawText(textRect, Qt::AlignVCenter | Qt::TextShowMnemonic, opt->text);

            painter->restore();
            return;
        }

        if (element == CE_RadioButton) {
            // インジケーター部分はQProxyStyleに委譲（State_Onの判定が確実）
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }

        if (element == CE_RadioButtonLabel) {
            const auto *opt = qstyleoption_cast<const QStyleOptionButton*>(option);
            if (!opt) { QProxyStyle::drawControl(element, option, painter, widget); return; }
            painter->save();
            bool hovered = opt->state & State_MouseOver;
            painter->setPen(hovered ? QColor("#a0d0f0") : QColor("#7aaed8"));
            if (widget) painter->setFont(widget->font());
            QRect textRect = subElementRect(SE_RadioButtonContents, opt, widget);
            painter->drawText(textRect, Qt::AlignVCenter | Qt::TextShowMnemonic, opt->text);
            painter->restore();
            return;
        }

        QProxyStyle::drawControl(element, option, painter, widget);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget = nullptr) const override
    {
        if (element == PE_IndicatorCheckBox) {
            bool checked = option->state & State_On;
            bool hovered = option->state & State_MouseOver;

            QColor frameColor = hovered ? QColor("#3a8fe8") : QColor("#1a3a5a");
            QColor fillColor  = QColor("#0a1e30");
            QColor markColor  = QColor("#3a8fe8");

            QRect box = option->rect.adjusted(1,1,-1,-1);

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);

            // ── 背景
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#0a0a1a"));
            painter->drawRoundedRect(box, 3, 3);

            // ── 枠
            painter->setPen(QPen(frameColor, 2.0));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(box, 3, 3);

            // ── チェック時
            if (checked) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(fillColor);
                painter->drawRoundedRect(box, 3, 3);

                painter->setPen(QPen(markColor, 2.2, Qt::SolidLine,
                                     Qt::RoundCap, Qt::RoundJoin));
                QRect r = box.adjusted(3, 3, -3, -3);
                QPointF p1(r.left(),                      r.top() + r.height() * 0.5f);
                QPointF p2(r.left() + r.width() * 0.38f, r.bottom());
                QPointF p3(r.right(),                     r.top());
                painter->drawPolyline(QPolygonF({p1, p2, p3}));
            }

            painter->restore();
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};

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
    setWindowTitle("Always Player  v10.0.0");
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
    // v10: 再生できなかった曲の理由をステータス欄に表示（その後は次の曲へ進む）
    connect(m_player, &Player::errorOccurred, this, [this](const QString &msg){
        if (m_statusBar) m_statusBar->setText(QString::fromUtf8("\xe2\x9a\xa0 ") + msg);
    });

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
            return;
        }

        if (m_player->isPlaying()) {
            m_infoLabel->setText(m_player->getInfo(currentMode()));

            // シークスライダー＆時間表示の更新（ドラッグ中は止める）
            if (!m_seekDragging) {
                const double pos      = m_player->getPosition();
                const double duration = m_player->getDuration();
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
    // ★ シークバー描画のポーリング間隔。以前は500msだったが、ギャップレス遷移時に
    //   Player側の内部タイマー（100ms間隔）で計算した「実質満タン」の瞬間を、
    //   この描画ポーリングが500ms周期のため捉え損ね、シークバーが最後まで
    //   伸びきる前に次曲へ切り替わって見えてしまう不具合があった
    //   （実測ログではPlayer側のgetPosition()自体は切替直前にほぼ満タン
    //    〜満タンに達していることを確認済み。ズレは純粋にこの描画側の
    //    ポーリング粒度が原因）。100msに短縮してPlayer側の内部タイマーと
    //    同程度の精度にし、見た目のズレを解消する。
    m_infoTimer->start(100);

    loadFavorites();

    QString last = m_player->loadLastFolder();
    if (!last.isEmpty() && QDir(last).exists())
        loadFolder(last, false);  // 起動時は自動再生しない

    setupRemote();
}

MainWindow::~MainWindow() {}

// ============================================================================
//  リモコン(Always Link)
//  スマホからの操作は、画面のボタンを押したのと同じ経路(click())で実行する。
//  CD再生中/ファイル再生中の分岐やUI更新をボタン側の処理と完全に共通化するため。
// ============================================================================
void MainWindow::setupRemote()
{
    m_remote = new RemoteServer(this);
    connect(m_remote, &RemoteServer::commandReceived, this, &MainWindow::onRemoteCommand);
    connect(m_remote, &RemoteServer::clientConnected, this, &MainWindow::publishRemoteStatus);
    if (!m_remote->start())
        qDebug() << "[Remote] disabled (port busy)";

    // 状態の送信は500msごと。内容が変わらなければRemoteServer側で送信を省く
    m_remoteTimer = new QTimer(this);
    connect(m_remoteTimer, &QTimer::timeout, this, &MainWindow::publishRemoteStatus);
    m_remoteTimer->start(500);
}

void MainWindow::onRemoteCommand(const QString &cmd, const QJsonObject &obj)
{
    const double value = obj.value("value").toDouble();
    const QString path = obj.value("path").toString();

    // ── フォルダ操作（スマホから別のアルバムを開く）
    if (cmd == "browse")     { remoteBrowse(path); return; }

    // ── 曲一覧（今のフォルダ／CDの曲）と、その曲への直接ジャンプ
    if (cmd == "tracks") {
        QJsonArray items;
        if (m_isCdMode) {
            for (int i = 0; i < m_playlist->count(); ++i) items.append(m_playlist->item(i)->text());
        } else {
            for (int i = 0; i < m_player->total(); ++i) items.append(m_player->fileAt(i));
        }
        m_remote->send(QJsonObject{{"type", "tracks"}, {"items", items}});
        return;
    }
    if (cmd == "playIndex") {
        const int idx = int(value);
        if (m_isCdMode) { if (idx >= 0 && idx < m_cdTrackCount) startCdTrackStream(idx); }
        else if (idx >= 0 && idx < m_player->total()) m_player->play(idx);
        publishRemoteStatus();
        return;
    }

    // ── 再生モード（PC版のShuffle / Repeatメニューと同じ設定をする）
    if (cmd == "repeat") {
        const QString v = obj.value("mode").toString();
        if (v == "one") {
            m_player->setRepeat(Player::RepeatMode::One);
            m_repeatBtn->setChecked(true);
            m_repeatBtn->setText(QString::fromUtf8("\xe2\x86\xa9 1\xe6\x9b\xb2"));
        } else if (v == "all") {
            m_player->setRepeat(Player::RepeatMode::All);
            m_repeatBtn->setChecked(true);
            m_repeatBtn->setText(QString::fromUtf8("\xe2\x86\xa9 \xe5\x85\xa8\xe6\x9b\xb2"));
        } else {
            m_player->setRepeat(Player::RepeatMode::None);
            m_repeatBtn->setChecked(false);
            m_repeatBtn->setText(QString::fromUtf8("\xe2\x86\xa9 Repeat"));
        }
        publishRemoteStatus();
        return;
    }
    if (cmd == "shuffle") {
        if (obj.value("mode").toString() == "folder") {
            m_player->setShuffle(Player::ShuffleMode::Folder);
            m_shuffleBtn->setChecked(true);
            m_shuffleBtn->setText(QString::fromUtf8("\xe2\x87\x8c \xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80\xe5\x86\x85"));
        } else {
            m_player->setShuffle(Player::ShuffleMode::None);
            m_shuffleBtn->setChecked(false);
            m_shuffleBtn->setText(QString::fromUtf8("\xe2\x87\x8c Shuffle"));
        }
        publishRemoteStatus();
        return;
    }

    // ── ヘッドホン補正（HP1 / HP2）。画面のボタンを押すのと同じ（押すたびにON/OFF、片方ONでもう片方OFF）
    if (cmd == "hp") {
        QPushButton *b = obj.value("mode").toString() == "hp2" ? m_hp2Btn : m_hp1Btn;
        if (b && b->isEnabled()) b->click();
        publishRemoteStatus();
        return;
    }

    // ── 音質モード（ピュア / ハイレゾx4 / 疑似DSDx8 / ラウドネス）。画面のボタンを押すのと同じ
    if (cmd == "mode") {
        const QString key = obj.value("mode").toString();
        if (m_modeBtns.contains(key) && m_modeBtns[key]->isEnabled()) m_modeBtns[key]->click();
        publishRemoteStatus();
        return;
    }
    if (cmd == "folderArt")  { remoteFolderArt(path); return; }
    if (cmd == "openFolder") {
        if (path.isEmpty() || !QDir(path).exists()) return;
        // アルバムブラウザでカードをクリックした時と同じ手順で開いて、1曲目から再生
        stopIfCd();
        clearCdState();
        m_player->stop();
        turnOffBitPerfect();
        loadFolder(path, false);
        if (m_player->total() > 0) m_player->play(0);
        if (QStackedWidget *ps = qobject_cast<QStackedWidget*>(m_mainContent->parentWidget()))
            ps->setCurrentIndex(0);
        publishRemoteStatus();
        return;
    }

    if      (cmd == "play")   m_playBtn->click();
    else if (cmd == "pause")  { if (m_isCdMode ? !m_cdPaused : !m_player->isPaused()) m_pauseBtn->click(); }
    else if (cmd == "toggle") {
        const bool playing = m_isCdMode ? (m_mciPlaying && !m_cdPaused)
                                        : (m_player->isPlaying() && !m_player->isPaused());
        const bool paused  = m_isCdMode ? m_cdPaused : m_player->isPaused();
        if (playing || paused) m_pauseBtn->click();   // 再生中⇔一時停止
        else                   m_playBtn->click();    // 停止中 → 再生
    }
    else if (cmd == "stop")   m_stopBtn->click();
    else if (cmd == "next")   m_nextBtn->click();
    else if (cmd == "prev")   m_prevBtn->click();
    else if (cmd == "volume") m_volSlider->setValue(qBound(0, int(value + 0.5), 100));
    else if (cmd == "seek") {
        if (!m_isCdMode && m_player->isPlaying() && value >= 0) m_player->seekTo(value);
    }
    publishRemoteStatus();
}

// 曲として数える拡張子（Player::SUPPORTED_EXT と同じ）
static const QStringList kRemoteAudioFilters = {
    "*.mp3","*.aac","*.ogg","*.wav","*.flac","*.opus","*.dsf","*.dff","*.m4a","*.aiff","*.aif","*.wv"
};

// スマホへフォルダ一覧を送る。path="" は「今のフォルダの親」、"::drives" はドライブ一覧
void MainWindow::remoteBrowse(const QString &pathIn)
{
    QString path = pathIn;
    if (path.isEmpty()) {
        QDir cur(m_currentFolder);
        path = (!m_currentFolder.isEmpty() && cur.exists() && cur.cdUp()) ? cur.absolutePath() : "::drives";
    }

    QJsonArray items;
    QString parent, title;

    auto folderEntry = [](const QFileInfo &fi, const QString &name) {
        QDir d(fi.absoluteFilePath());
        const int tracks = d.entryList(kRemoteAudioFilters, QDir::Files).size();
        const bool sub = !d.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
        return QJsonObject{
            {"name", name}, {"path", fi.absoluteFilePath()}, {"tracks", tracks}, {"sub", sub},
        };
    };

    if (path == "::drives") {
        title = QString::fromUtf8("PC");
        for (const QFileInfo &fi : QDir::drives()) {
            const QString root = fi.absoluteFilePath();          // 例: "C:/"
            // CDドライブは中身の読み取りに時間がかかるため一覧に出さない
            if (GetDriveTypeW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(root).utf16())) == DRIVE_CDROM)
                continue;
            items.append(QJsonObject{
                {"name", QDir::toNativeSeparators(root).chopped(1)}, {"path", root}, {"tracks", 0}, {"sub", true},
            });
        }
    } else {
        QDir d(path);
        if (!d.exists()) { remoteBrowse("::drives"); return; }
        title = d.isRoot() ? QDir::toNativeSeparators(d.absolutePath()) : d.dirName();
        QDir up(d);
        parent = up.cdUp() ? up.absolutePath() : QString("::drives");
        if (d.isRoot()) parent = "::drives";

        // 自然順（1,2,10）で並べる。隠し・システムフォルダは出さない
        QCollator col;
        col.setNumericMode(true);
        col.setCaseSensitivity(Qt::CaseInsensitive);
        QFileInfoList dirs = d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        std::sort(dirs.begin(), dirs.end(), [&](const QFileInfo &a, const QFileInfo &b) {
            return col.compare(a.fileName(), b.fileName()) < 0;
        });
        for (const QFileInfo &fi : dirs) {
            if (fi.isHidden() || fi.fileName().startsWith('$')) continue;
            items.append(folderEntry(fi, fi.fileName()));
        }
    }

    m_remote->send(QJsonObject{
        {"type", "folders"}, {"path", path}, {"parent", parent}, {"title", title}, {"items", items},
    });
}

// フォルダのジャケット（アルバムブラウザと同じ findAlbumArt）を小さく送る
void MainWindow::remoteFolderArt(const QString &path)
{
    QByteArray jpeg;
    if (!path.isEmpty() && !path.startsWith("::") && QDir(path).exists()) {
        const QPixmap px = findAlbumArt(path, 200);
        if (!px.isNull()) {
            QBuffer buf(&jpeg);
            buf.open(QIODevice::WriteOnly);
            px.save(&buf, "JPG", 80);
        }
    }
    m_remote->send(QJsonObject{
        {"type", "folderArt"}, {"path", path}, {"data", QString::fromLatin1(jpeg.toBase64())},
    });
}

void MainWindow::publishRemoteStatus()
{
    if (!m_remote || m_remote->clientCount() == 0) return;   // 誰もつないでいなければ何もしない

    QString state = "stopped";
    double pos = 0.0, dur = 0.0;
    if (m_isCdMode) {
        if (m_cdPaused) state = "paused";
        else if (m_mciPlaying) state = "playing";
    } else if (m_player->isPlaying() || m_player->isPaused()) {
        // ★ Playerは一時停止中 m_playing=false / m_paused=true になるため、
        //   isPlaying()だけで判定すると一時停止が「停止」に見えてしまう
        state = m_player->isPaused() ? "paused" : "playing";
        pos = m_player->getPosition();
        dur = m_player->getDuration();
    }

    // 音質モードの一覧（ボタンの表示名はCD/ハイレゾで変わるので毎回画面から取る）
    QJsonArray modes;
    for (const QString &k : {QStringLiteral("pure"), QStringLiteral("hires4"), QStringLiteral("dsd8"), QStringLiteral("loudness")}) {
        if (!m_modeBtns.contains(k)) continue;
        modes.append(QJsonObject{{"key", k}, {"label", m_modeBtns[k]->text()}, {"enabled", m_modeBtns[k]->isEnabled()}});
    }

    // ジャケット：画面に出ている画像をそのまま送る（CDのネット取得画像も含む）
    const QPixmap art = m_jacket ? m_jacket->pixmap() : QPixmap();
    const qint64 key = art.isNull() ? 0 : art.cacheKey();
    if (key != m_remoteArtKey) {
        m_remoteArtKey = key;
        QByteArray jpeg;
        if (!art.isNull()) {
            QBuffer buf(&jpeg);
            buf.open(QIODevice::WriteOnly);
            art.scaled(400, 400, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buf, "JPG", 85);
            ++m_remoteArtId;
        }
        m_remote->publishArt(art.isNull() ? 0 : m_remoteArtId, jpeg);
    }

    m_remote->publishStatus(QJsonObject{
        {"state",  state},
        {"title",  m_title ? m_title->text() : QString()},
        {"artist", m_subTitle ? m_subTitle->text() : QString()},
        {"index",  m_isCdMode ? m_cdCurrentTrack : m_player->currentIndex()},
        {"total",  m_isCdMode ? m_cdTrackCount   : m_player->total()},
        {"pos",    qRound(pos)},   // 1秒単位（スマホ側で秒表示するのに十分。通信量を抑える）
        {"dur",    qRound(dur)},
        {"volume", m_volSlider ? m_volSlider->value() : 100},
        {"cd",     m_isCdMode},
        {"artId",  double(art.isNull() ? 0 : m_remoteArtId)},
        {"repeat", m_player->repeatMode() == Player::RepeatMode::One ? "one"
                 : m_player->repeatMode() == Player::RepeatMode::All ? "all" : "none"},
        {"shuffle", m_player->shuffleMode() == Player::ShuffleMode::Folder ? "folder"
                  : m_player->shuffleMode() == Player::ShuffleMode::Favorites ? "favorites" : "none"},
        {"mode",   currentMode()},
        {"modes",  modes},
        {"hp", QJsonArray{
            QJsonObject{{"key", "hp1"}, {"label", m_hp1Btn ? m_hp1Btn->text() : QString()},
                        {"on", m_hp1On}, {"enabled", m_hp1Btn && m_hp1Btn->isEnabled()}},
            QJsonObject{{"key", "hp2"}, {"label", m_hp2Btn ? m_hp2Btn->text() : QString()},
                        {"on", m_hp2On}, {"enabled", m_hp2Btn && m_hp2Btn->isEnabled()}},
        }},
        {"modeDesc", m_modeDesc ? m_modeDesc->text() : QString()},
    });
}

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

        // 現在のシャッフルモードを取得
        auto cur = m_player->shuffleMode();

        QActionGroup *grp = new QActionGroup(menu);
        grp->setExclusive(true);

        auto *aOff = menu->addAction(QString::fromUtf8("OFF"), [this]{
            m_player->setShuffle(Player::ShuffleMode::None);
            m_shuffleBtn->setChecked(false);
            m_shuffleBtn->setText(QString::fromUtf8("\xe2\x87\x8c Shuffle"));
        });
        aOff->setCheckable(true);
        aOff->setChecked(cur == Player::ShuffleMode::None);
        grp->addAction(aOff);

        auto *aFolder = menu->addAction(QString::fromUtf8("\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80\xe5\x86\x85"), [this]{
            m_player->setShuffle(Player::ShuffleMode::Folder);
            m_shuffleBtn->setChecked(true);
            m_shuffleBtn->setText(QString::fromUtf8("\xe2\x87\x8c \xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80\xe5\x86\x85"));
        });
        aFolder->setCheckable(true);
        aFolder->setChecked(cur == Player::ShuffleMode::Folder);
        grp->addAction(aFolder);

        auto *aFav = menu->addAction(QString::fromUtf8("\xe3\x81\x8a\xe6\xb0\x97\xe3\x81\xab\xe5\x85\xa1\xe3\x82\x8a\xef\xbc\x88\xe5\x85\xa8\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88\xef\xbc\x89"), [this]{
            // 全リストのパスをまとめる
            QStringList paths;
            for (const auto &fl : m_favoriteLists)
                for (auto it = fl.items.begin(); it != fl.items.end(); ++it)
                    if (!paths.contains(it.key())) paths << it.key();
            m_player->setFavoritePaths(paths);
            m_player->setShuffle(Player::ShuffleMode::Favorites);
            m_shuffleBtn->setChecked(true);
            m_shuffleBtn->setText(QString::fromUtf8("\xe2\x87\x8c \xe3\x81\x8a\xe6\xb0\x97\xe3\x81\xab\xe5\x85\xa5\xe3\x82\x8a"));
            if (!paths.isEmpty()) { loadFolder(paths.first(), false); m_player->play(0); }
        });
        aFav->setCheckable(true);
        aFav->setChecked(cur == Player::ShuffleMode::Favorites);
        grp->addAction(aFav);

        // リストごとのシャッフル
        if (!m_favoriteLists.isEmpty()) {
            menu->addSeparator();
            for (int i = 0; i < m_favoriteLists.size(); ++i) {
                const auto &fl = m_favoriteLists[i];
                auto *aList = menu->addAction(fl.name, [this, i]{
                    QStringList paths;
                    for (auto it = m_favoriteLists[i].items.begin();
                         it != m_favoriteLists[i].items.end(); ++it)
                        paths << it.key();
                    m_player->setFavoritePaths(paths);
                    m_player->setShuffle(Player::ShuffleMode::Favorites);
                    m_activeListIndex = i;
                    m_shuffleBtn->setChecked(true);
                    m_shuffleBtn->setText(QString::fromUtf8("\xe2\x87\x8c ") + m_favoriteLists[i].name);
                    if (!paths.isEmpty()) { loadFolder(paths.first(), false); m_player->play(0); }
                });
                aList->setCheckable(true);
                grp->addAction(aList);
            }
        }

        menu->exec(QCursor::pos());
    });

    // リピートボタン
    m_repeatBtn = new QPushButton(QString::fromUtf8("\xe2\x86\xa9 Repeat"));
    m_repeatBtn->setObjectName("toolBtn");
    m_repeatBtn->setCheckable(true);
    connect(m_repeatBtn, &QPushButton::clicked, this, [this]{
        QMenu *menu = new QMenu(this);

        // 現在のリピートモードを取得
        auto cur = m_player->repeatMode();

        QActionGroup *grp = new QActionGroup(menu);
        grp->setExclusive(true);

        auto *aOff = menu->addAction("OFF", [this]{
            m_player->setRepeat(Player::RepeatMode::None);
            m_repeatBtn->setChecked(false);
            m_repeatBtn->setText(QString::fromUtf8("\xe2\x86\xa9 Repeat"));
        });
        aOff->setCheckable(true);
        aOff->setChecked(cur == Player::RepeatMode::None);
        grp->addAction(aOff);

        auto *aOne = menu->addAction(QString::fromUtf8("1\xe6\x9b\xb2"), [this]{
            m_player->setRepeat(Player::RepeatMode::One);
            m_repeatBtn->setChecked(true);
            m_repeatBtn->setText(QString::fromUtf8("\xe2\x86\xa9 1\xe6\x9b\xb2"));
        });
        aOne->setCheckable(true);
        aOne->setChecked(cur == Player::RepeatMode::One);
        grp->addAction(aOne);

        auto *aAll = menu->addAction(QString::fromUtf8("\xe5\x85\xa8\xe6\x9b\xb2"), [this]{
            m_player->setRepeat(Player::RepeatMode::All);
            m_repeatBtn->setChecked(true);
            m_repeatBtn->setText(QString::fromUtf8("\xe2\x86\xa9 \xe5\x85\xa8\xe6\x9b\xb2"));
        });
        aAll->setCheckable(true);
        aAll->setChecked(cur == Player::RepeatMode::All);
        grp->addAction(aAll);

        menu->exec(QCursor::pos());
    });

    // タイマーボタン
    m_sleepBtn = new QPushButton("Timer");
    m_sleepBtn->setObjectName("toolBtn");
    connect(m_sleepBtn, &QPushButton::clicked, this, &MainWindow::onSleepTimer);

    tbL->addWidget(m_albumBrowseBtn);
    tbL->addWidget(folderBtn);
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
    // v10: 共有モードは無くなったので「自動（モード連動）」に名称変更
    m_bpActOff = bpMenu->addAction(QString::fromUtf8("\xe8\x87\xaa\xe5\x8b\x95\xef\xbc\x88\xe3\x83\xa2\xe3\x83\xbc\xe3\x83\x89\xe9\x80\xa3\xe5\x8b\x95\xef\xbc\x89"));
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
            m_bpManualOff = true;   // ★ 手動OFFを記憶
            m_bpManualRatePinned = false;  // ★ OFFにしたので固定レートも解除
            m_player->setManualRateOverride(false);  // ★ Player側のガードも解除
            m_bitPerfectBtn->setText("BitPerfect ▼");
            qDebug() << "[BitPerfect] OFF (manual)";
            // ★ 排他モードを解除したので、次にexclusive=yesへ戻すときは
            //   同一レート判定でスキップされないようキャッシュを無効化する。
            m_lastAppliedRate = -1;
            m_lastAppliedBits = -1;
        } else {
            m_bpManualOff = false;  // ★ 手動でONにしたのでフラグ解除
            QVariantList data = act->data().toList();
            int rate = data[0].toInt();
            int bits = data[1].toInt();
            // ★ 特定のレート/ビット数を手動で固定選択したので記憶する。
            //   曲が変わっても、この値を onTrackChanged() が上書きしないようにする。
            m_bpManualRatePinned = true;
            m_pinnedBpRate = rate;
            m_pinnedBpBits = bits;
            // ★ Player::applyAudioChain()にこの固定を伝え、次の曲でm_modeに
            //   基づくaudio-samplerateの自動上書きが起きないようにする。
            m_player->setManualRateOverride(true);
            m_bitPerfectBtn->setText(
                QString("BitPerfect %1Hz/%2 ▼").arg(rate/1000).arg(bits));
            qDebug() << "[BitPerfect] rate=" << rate << "bits=" << bits;
            m_lastAppliedRate = rate;
            m_lastAppliedBits = bits;
            // v10: 選んだ出力形式を自作エンジンへ（再生中なら開き直して即反映）
            m_player->setPinnedOutput(rate, bits);
        }
        scheduleSave();  // BitPerfect設定を保存
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
    m_vuMeter->setPlayer(m_player);  // ★ 実信号取得のために Player を渡す
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
            // ★ モードボタンを直接押した場合は、手動固定レート選択を解除し、
            //   モード連動の自動レート決定に戻す。
            m_bpManualRatePinned = false;
            m_player->setManualRateOverride(false);
            m_userMode = key; // v10: 選んだモードを覚えておき、次の曲でも使う
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
            m_mciPlaying = false;
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
                QString playCmd = QString("play cd from %1").arg(static_cast<long long>(seekMs));
                mciSendStringW(reinterpret_cast<LPCWSTR>(playCmd.utf16()), nullptr, 0, nullptr);
                m_cdPaused = false;
            }
            mciSendStringW(L"set cd time format tmsf", nullptr, 0, nullptr);
            return;
        }
        const double duration = m_player->getDuration();
        if (duration > 0) {
            double pos = duration * m_seekSlider->value() / 1000.0;
            m_player->seekTo(pos);
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
    qApp->setStyle(new AlwaysStyle("Fusion"));
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
        QLabel#infoLabel { color:#3a6080; font-size:10px; font-family:'Meiryo UI'; }
        QLabel#statusBar { color:#304050; font-size:9px; padding-left:4px; }
        QLabel#jacketLabel { color:#141414; font-size:11px; }
        QGroupBox {
            color:#3a6080; font-size:10px; font-family:'Meiryo UI';
            border:1px solid #1a3a5a; border-radius:6px;
            margin-top:8px; padding-top:6px;
        }
        QGroupBox::title {
            subcontrol-origin:margin; subcontrol-position:top left;
            left:10px; padding:0 4px; color:#3a8fe8;
        }
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
        QWidget#albumCard { background:#0d0d0d; border:1px solid #1a2a3a; border-radius:6px; }
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

    // ── 電源イベント監視：スリープ復帰後にUSB設定を自動再適用
    if (msg->message == WM_POWERBROADCAST) {
        if (msg->wParam == PBT_APMRESUMEAUTOMATIC ||
            msg->wParam == PBT_APMPOWERSTATUSCHANGE) {
            // スリープ復帰後3秒待ってからUSBサスペンド無効化を再適用
            QTimer::singleShot(3000, this, [this]() {
                // USBセレクティブサスペンドが無効化設定のときのみ再適用
                DWORD val = 0;
                HKEY hKey;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SYSTEM\\CurrentControlSet\\Services\\USB",
                    0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
                    DWORD size = sizeof(DWORD);
                    RegQueryValueExW(hKey, L"DisableSelectiveSuspend",
                                     nullptr, nullptr, (LPBYTE)&val, &size);
                    RegCloseKey(hKey);
                }
                if (val == 1) {
                    // 管理者権限なしで再設定（既に設定済みのレジストリを維持）
                    qDebug() << "[Power] Resume detected, USB suspend already disabled";
                }
                // timeBeginPeriod を再設定（スリープ復帰で解除される場合がある）
                timeBeginPeriod(1);
                qDebug() << "[Power] Resume detected, timeBeginPeriod(1) re-applied";
            });
        }
    }

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
    m_player->stop();
    saveFavorites();
    event->accept();
    QApplication::quit();
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

    // 通常フォルダ：CD再生中なら先に停止
    stopIfCd();
    clearCdState();
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
    setWindowTitle(jp("Always Player  v10.0.0  -  CD"));
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

    m_cdPaused = false;   // ★ 新トラック開始時は一時停止解除

    // ★ stopCdStream前にtrackIndexを設定（infoTimerの誤検知を防ぐ）
    m_cdCurrentTrack = trackIndex;
    m_playlist->setCurrentRow(trackIndex);

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
                    setWindowTitle(jp("Always Player  v10.0.0  -  CD"));
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
    setWindowTitle(jp("Always Player  v10.0.0  -  CD  -  ") + name);


}

void MainWindow::loadFolder(const QString &path, bool autoPlay)
{
    m_isCdMode = false;  // ★ フォルダ読み込み時はCDモードを解除
    // モードボタンのテキストを元に戻す（ハイレゾ/CD表示から復元）
    {
        QMap<QString,QString> origLabels;
        origLabels["hires4"]   = jp("\xe3\x83\x8f\xe3\x82\xa4\xe3\x83\xac\xe3\x82\xbe x4");
        origLabels["dsd8"]     = jp("\xe7\x96\x91\xe4\xbc\xbc") + "DSD x8";
        origLabels["loudness"] = jp("\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\x89\xe3\x83\x8d\xe3\x82\xb9");
        for (auto it = origLabels.begin(); it != origLabels.end(); ++it)
            if (m_modeBtns.contains(it.key()))
                m_modeBtns[it.key()]->setText(it.value());
    }
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
                    // ★ FLACの正規タグはVorbisComment（XiphComment）を優先
                    if (tf.xiphComment() && !tf.xiphComment()->isEmpty()) {
                        title  = QString::fromUtf8(tf.xiphComment()->title().toCString(true));
                        artist = QString::fromUtf8(tf.xiphComment()->artist().toCString(true));
                    } else if (tf.tag()) {
                        title  = QString::fromUtf8(tf.tag()->title().toCString(true));
                        artist = QString::fromUtf8(tf.tag()->artist().toCString(true));
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
                        title  = QString::fromUtf8(tf.tag()->title().toCString(true));
                        artist = QString::fromUtf8(tf.tag()->artist().toCString(true));
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
                        title  = QString::fromUtf8(ref.tag()->title().toCString(true));
                        artist = QString::fromUtf8(ref.tag()->artist().toCString(true));
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
        QString autoMode = hiRes ? "pure" : m_userMode; // v10: 以前は常にdsd8へ戻していた
        {
            QMap<QString,QString> hiResLabels;
            hiResLabels["hires4"]   = jp("\xe3\x83\x8f\xe3\x82\xa4\xe3\x83\xac\xe3\x82\xbe x4  " "\xef\xbc\x88\xe4\xbd\xbf\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
            hiResLabels["dsd8"]     = jp("\xe7\x96\x91\xe4\xbc\xbc" "DSD x8  " "\xef\xbc\x88\xe4\xbd\xbf\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
            hiResLabels["loudness"] = jp("\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\x89\xe3\x83\x8d\xe3\x82\xb9  " "\xef\xbc\x88\xe4\xbd\xbf\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
            QMap<QString,QString> origLabels;
            origLabels["hires4"]   = jp("\xe3\x83\x8f\xe3\x82\xa4\xe3\x83\xac\xe3\x82\xbe x4");
            origLabels["dsd8"]     = jp("\xe7\x96\x91\xe4\xbc\xbc") + "DSD x8";
            origLabels["loudness"] = jp("\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\x89\xe3\x83\x8d\xe3\x82\xb9");
            for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it) {
                it.value()->setChecked(it.key() == autoMode);
                bool enabled = !hiRes || it.key() == "pure";
                it.value()->setEnabled(enabled);
                if (it.key() != "pure") {
                    if (hiRes && hiResLabels.contains(it.key()))
                        it.value()->setText(hiResLabels[it.key()]);
                    else if (origLabels.contains(it.key()))
                        it.value()->setText(origLabels[it.key()]);
                }
            }
        }
        updateModeDesc(autoMode);
        m_player->setModeQuiet(autoMode);
        // infoLabel即時表示
        m_infoLabel->setText(m_player->getInfo(currentMode()));
        // タグ表示
        QString firstFileName = m_player->fileAt(0);
        m_title->setText(QFileInfo(fp).completeBaseName());  // ファイル名で統一（他プレイヤーと同仕様）
        QString sub = QString("1  /  %1").arg(m_player->total());
        if (!artist.isEmpty()) sub += "   " + artist;
        m_subTitle->setText(sub);
        m_currentArtist = artist;
        if (m_artistInfoBtn) m_artistInfoBtn->setEnabled(!artist.isEmpty());
        m_playlist->setCurrentRow(0);
        setWindowTitle(QString::fromUtf8("Always Player  v10.0.0  -  ") + QFileInfo(fp).fileName());
        m_statusBar->setText(">> " + QFileInfo(fp).completeBaseName());
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
            // ★ 手動でVUに切り替えている場合は表示を上書きしない
            if (!m_showVU) {
                m_stack->setCurrentIndex(1);
                m_vuMeter->setPlaying(false);
            }
            return;
        }
    }
    m_jacket->setPixmap(QPixmap());
    m_jacket->setText("No artwork");
    m_hasArtwork = false;
    m_showVU = true;
    m_stack->setCurrentIndex(0);  // アートワークなしは常にVU表示
}

void MainWindow::onTrackChanged(int index, const QString &filename,
                                const QString &title, const QString &artist)
{
    // ★ CD再生中はStreamModeで管理するのでスキップ
    if (m_isCdMode) return;

    m_title->setText(filename.section('.', 0, -2));  // ファイル名で統一（他プレイヤーと同仕様）
    QString sub = QString("%1  /  %2").arg(index + 1).arg(m_player->total());
    if (!artist.isEmpty()) sub += "   " + artist;
    m_subTitle->setText(sub);
    m_currentArtist = artist;
    if (m_artistInfoBtn) m_artistInfoBtn->setEnabled(!artist.isEmpty());
    m_playlist->setCurrentRow(index);
    setWindowTitle(QString::fromUtf8("Always Player  v10.0.0  -  ") + filename);
    m_statusBar->setText(">> " + filename.section('.', 0, -2));

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

            // ── BitPerfect表示更新（常に実行。曲そのものの元の値を表示する）
            // ★ mpvへの強制変換命令（audio-samplerate等）は、フリーズの原因に
            //   なったため m_bpManualOff=true の間は呼ばない。表示の更新自体は
            //   実際の出力と食い違わないよう、手動モードでも常に行う。
            if (sr > 0) {
                if (m_player->isUsingNewEngineNow()) {
                    // ── v9: 新エンジン(FLAC/WAV/AIFF/WavPack)はWASAPI排他
                    // モードで出力する。dsd8/hires4モード時はPlayer内部で
                    // SincResamplerによりアップサンプリング済みのレートに
                    // なっているため、newEngineActualSampleRate()は
                    // （ネイティブではなく）実際の出力レートを返す。
                    // mpvへの命令は一切呼ばない。
                    uint32_t newRate = m_player->newEngineActualSampleRate();
                    uint32_t newBits = m_player->newEngineActualBits();

                    QMenu *bpMenu2 = m_bitPerfectBtn->menu();
                    QAction *matched2 = nullptr;
                    for (QAction *act : bpMenu2->actions()) {
                        if (act->data().isValid()) {
                            QVariantList d = act->data().toList();
                            if (d[0].toInt() == static_cast<int>(newRate) &&
                                d[1].toInt() == static_cast<int>(newBits)) {
                                matched2 = act;
                                break;
                            }
                        }
                    }
                    // v10: 手動指定中はその項目、自動のときは「自動（モード連動）」に
                    //      チェックを付ける（ボタンの文字は常に実際の出力形式）。
                    if (m_bpManualRatePinned) { if (matched2) matched2->setChecked(true); }
                    else if (m_bpActOff) m_bpActOff->setChecked(true);

                    // v10: 共有モード（Bluetoothなど）はビットパーフェクトではないので、
                    //      そうとわかる表示にする
                    if (m_player->isSharedOutput())
                        m_bitPerfectBtn->setText(
                            QString::fromUtf8("共有モード %1kHz ▼")
                            .arg(newRate / 1000.0, 0, 'f', newRate % 1000 == 0 ? 0 : 1));
                    else
                    m_bitPerfectBtn->setText(
                        QString("BitPerfect %1kHz/%2 ▼")
                        .arg(newRate / 1000.0, 0, 'f', newRate % 1000 == 0 ? 0 : 1)
                        .arg(newBits));
                }
            } // if (sr > 0)

            // ★ 「16種類の手動ビットパーフェクト」を固定選択中は、以下の
            //   ハイレゾ自動モード切り替え（dsd8/pureへの強制変更）を一切行わない。
            //   これを無条件に実行していたため、手動選択が次の曲で352.8kHz(dsd8)
            //   に勝手に戻ってしまうバグがあった。
            if (!m_bpManualRatePinned) {
                // ハイレゾ判定
                bool hiRes = (sr > 48000);
                QString autoMode = hiRes ? "pure" : m_userMode; // v10: 以前は常にdsd8へ戻していた

                // ボタン状態更新
                {
                    QMap<QString,QString> hiResLabels;
                    hiResLabels["hires4"]   = jp("\xe3\x83\x8f\xe3\x82\xa4\xe3\x83\xac\xe3\x82\xbe x4  " "\xef\xbc\x88\xe4\xbd\xbf\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
                    hiResLabels["dsd8"]     = jp("\xe7\x96\x91\xe4\xbc\xbc" "DSD x8  " "\xef\xbc\x88\xe4\xbd\xbf\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
                    hiResLabels["loudness"] = jp("\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\x89\xe3\x83\x8d\xe3\x82\xb9  " "\xef\xbc\x88\xe4\xbd\xbf\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
                    QMap<QString,QString> origLabels;
                    origLabels["hires4"]   = jp("\xe3\x83\x8f\xe3\x82\xa4\xe3\x83\xac\xe3\x82\xbe x4");
                    origLabels["dsd8"]     = jp("\xe7\x96\x91\xe4\xbc\xbc") + "DSD x8";
                    origLabels["loudness"] = jp("\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\x89\xe3\x83\x8d\xe3\x82\xb9");
                    for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it) {
                        it.value()->setChecked(it.key() == autoMode);
                        bool enabled = !hiRes || it.key() == "pure";
                        it.value()->setEnabled(enabled);
                        if (it.key() != "pure") {
                            if (hiRes && hiResLabels.contains(it.key()))
                                it.value()->setText(hiResLabels[it.key()]);
                            else if (origLabels.contains(it.key()))
                                it.value()->setText(origLabels[it.key()]);
                        }
                    }
                }
                updateModeDesc(autoMode);

                // モードを実際に切り替え（DSP処理も反映）
                m_player->setMode(autoMode, m_hp1On, m_hp2On, m_soundField);
            }

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

    // リストが空なら自動でリスト1を作成
    if (m_favoriteLists.isEmpty()) {
        FavoriteList fl;
        fl.name = QString::fromUtf8("\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88") + "1";
        m_favoriteLists.append(fl);
    }

    // どのリストに追加するか選択ダイアログ
    QDialog *dlg = new QDialog(this);
    dlg->setWindowTitle(QString::fromUtf8("\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88\xe3\x81\xab\xe8\xbf\xbd\xe5\x8a\xa0"));
    dlg->resize(300, 200);
    QVBoxLayout *vl = new QVBoxLayout(dlg);

    auto *label = new QLabel(QString::fromUtf8(
        "\xe3\x81\xa9\xe3\x81\xae\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88\xe3\x81\xab\xe8\xbf\xbd\xe5\x8a\xa0\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x81\x8b\xef\xbc\x9f"));
    vl->addWidget(label);

    QListWidget *lw = new QListWidget();
    for (int i = 0; i < m_favoriteLists.size(); ++i) {
        const auto &fl = m_favoriteLists[i];
        auto *item = new QListWidgetItem(
            QString("%1  (%2%3)").arg(fl.name).arg(fl.items.size())
            .arg(QString::fromUtf8("\xe6\x9b\xb2")));
        item->setData(Qt::UserRole, i);
        lw->addItem(item);
        if (i == m_activeListIndex) lw->setCurrentRow(i);
    }
    vl->addWidget(lw);

    // 新しいリストを作成
    auto *newListBtn = new QPushButton(QString::fromUtf8("＋ \xe6\x96\xb0\xe3\x81\x97\xe3\x81\x84\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88\xe3\x82\x92\xe4\xbd\x9c\xe6\x88\x90"));
    newListBtn->setObjectName("toolBtn");
    connect(newListBtn, &QPushButton::clicked, dlg, [this, lw, dlg]() {
        bool ok;
        QString listName = QInputDialog::getText(dlg,
            QString::fromUtf8("\xe6\x96\xb0\xe3\x81\x97\xe3\x81\x84\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88"),
            QString::fromUtf8("\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88\xe5\x90\x8d\xe3\x82\x92\xe5\x85\xa5\xe5\x8a\x9b\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84\xef\xbc\x9a"),
            QLineEdit::Normal, QString::fromUtf8("\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88") + QString::number(m_favoriteLists.size() + 1), &ok);
        if (ok && !listName.isEmpty()) {
            FavoriteList fl;
            fl.name = listName;
            m_favoriteLists.append(fl);
            int idx = m_favoriteLists.size() - 1;
            auto *item = new QListWidgetItem(
                QString("%1  (0%2)").arg(listName).arg(QString::fromUtf8("\xe6\x9b\xb2")));
            item->setData(Qt::UserRole, idx);
            lw->addItem(item);
            lw->setCurrentRow(idx);
        }
    });
    vl->addWidget(newListBtn);

    QHBoxLayout *hl = new QHBoxLayout();
    auto *addBtn    = new QPushButton(QString::fromUtf8("\xe8\xbf\xbd\xe5\x8a\xa0"));
    auto *removeBtn = new QPushButton(QString::fromUtf8("\xe5\x89\x8a\xe9\x99\xa4"));
    auto *cancelBtn = new QPushButton(QString::fromUtf8("\xe9\x96\x89\xe3\x81\x98\xe3\x82\x8b"));
    addBtn->setObjectName("toolBtn");
    removeBtn->setObjectName("toolBtn");
    cancelBtn->setObjectName("toolBtn");
    hl->addWidget(addBtn); hl->addWidget(removeBtn); hl->addStretch(); hl->addWidget(cancelBtn);
    vl->addLayout(hl);

    // リスト選択時にボタン状態を更新
    auto updateBtns = [this, lw, addBtn, removeBtn]() {
        auto *item = lw->currentItem();
        if (!item) { addBtn->setEnabled(false); removeBtn->setEnabled(false); return; }
        int idx = item->data(Qt::UserRole).toInt();
        bool registered = (idx >= 0 && idx < m_favoriteLists.size()
                           && m_favoriteLists[idx].items.contains(m_currentFolder));
        addBtn->setEnabled(!registered);
        removeBtn->setEnabled(registered);
    };
    connect(lw, &QListWidget::currentRowChanged, dlg, [updateBtns](int){ updateBtns(); });
    updateBtns();

    connect(addBtn, &QPushButton::clicked, dlg, [this, lw, name, dlg]() {
        auto *item = lw->currentItem();
        if (!item) return;
        int idx = item->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= m_favoriteLists.size()) return;
        m_favoriteLists[idx].items[m_currentFolder] = name;
        m_activeListIndex = idx;
        scheduleSave();
        dlg->accept();
    });
    connect(removeBtn, &QPushButton::clicked, dlg, [this, lw, dlg]() {
        auto *item = lw->currentItem();
        if (!item) return;
        int idx = item->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= m_favoriteLists.size()) return;
        m_favoriteLists[idx].items.remove(m_currentFolder);
        scheduleSave();
        dlg->accept();
    });
    connect(cancelBtn, &QPushButton::clicked, dlg, &QDialog::accept);

    dlg->exec();
    dlg->deleteLater();
}

void MainWindow::onShowFavorites()
{
    stopIfCd();

    if (m_favoriteLists.isEmpty()) {
        QMessageBox::information(this,
            QString::fromUtf8("\xe3\x81\x8a\xe6\xb0\x97\xe3\x81\xab\xe5\x85\xa5\xe3\x82\x8a"),
            QString::fromUtf8("\xe3\x81\x8a\xe6\xb0\x97\xe3\x81\xab\xe5\x85\xa5\xe3\x82\x8a\xe3\x81\xaf\xe3\x81\x82\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93"));
        return;
    }

    QDialog *dlg = new QDialog(this);
    dlg->setWindowTitle(QString::fromUtf8("\xe3\x81\x8a\xe6\xb0\x97\xe3\x81\xab\xe5\x85\xa5\xe3\x82\x8a\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88"));
    dlg->resize(480, 420);
    QVBoxLayout *vl = new QVBoxLayout(dlg);

    // ── リスト選択タブ（横並びボタン）
    auto *tabRow = new QHBoxLayout();
    QList<QPushButton*> tabBtns;
    for (int i = 0; i < m_favoriteLists.size(); ++i) {
        auto *btn = new QPushButton(m_favoriteLists[i].name);
        btn->setObjectName("toolBtn");
        btn->setCheckable(true);
        btn->setChecked(i == m_activeListIndex);
        tabBtns.append(btn);
        tabRow->addWidget(btn);
    }
    tabRow->addStretch();
    vl->addLayout(tabRow);

    // ── 曲リスト
    auto *lw = new QListWidget();
    auto refreshList = [&](int idx) {
        lw->clear();
        if (idx < 0 || idx >= m_favoriteLists.size()) return;
        for (auto it = m_favoriteLists[idx].items.begin();
             it != m_favoriteLists[idx].items.end(); ++it) {
            auto *item = new QListWidgetItem(it.value());
            item->setData(Qt::UserRole, it.key());
            lw->addItem(item);
        }
    };
    refreshList(m_activeListIndex);
    vl->addWidget(lw);

    // タブボタンのクリック
    for (int i = 0; i < tabBtns.size(); ++i) {
        connect(tabBtns[i], &QPushButton::clicked, dlg, [i, &tabBtns, &refreshList, this](bool) {
            m_activeListIndex = i;
            for (int j = 0; j < tabBtns.size(); ++j)
                tabBtns[j]->setChecked(j == i);
            refreshList(i);
        });
    }

    // ── ボタン行
    QHBoxLayout *hl = new QHBoxLayout();
    auto *openBtn   = new QPushButton(QString::fromUtf8("\xe9\x96\x8b\xe3\x81\x84\xe3\x81\xa6\xe5\x86\x8d\xe7\x94\x9f"));
    auto *delBtn    = new QPushButton(QString::fromUtf8("\xe5\x89\x8a\xe9\x99\xa4"));
    auto *renameBtn = new QPushButton(QString::fromUtf8("\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88\xe5\x90\x8d\xe5\xa4\x89\xe6\x9b\xb4"));
    auto *delListBtn= new QPushButton(QString::fromUtf8("\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88\xe5\x89\x8a\xe9\x99\xa4"));
    auto *closeBtn  = new QPushButton(QString::fromUtf8("\xe9\x96\x89\xe3\x81\x98\xe3\x82\x8b"));
    for (auto *b : {openBtn, delBtn, renameBtn, delListBtn, closeBtn})
        b->setObjectName("toolBtn");
    hl->addWidget(openBtn); hl->addWidget(delBtn);
    hl->addWidget(renameBtn); hl->addWidget(delListBtn);
    hl->addStretch(); hl->addWidget(closeBtn);
    vl->addLayout(hl);

    // 開いて再生
    connect(openBtn, &QPushButton::clicked, dlg, [this, lw, dlg]() {
        auto *item = lw->currentItem();
        if (!item) return;
        stopIfCd();
        m_player->stop();
        turnOffBitPerfect();
        loadFolder(item->data(Qt::UserRole).toString(), false);
        dlg->accept();
    });

    // 曲を削除
    connect(delBtn, &QPushButton::clicked, dlg, [this, lw]() {
        auto *item = lw->currentItem();
        if (!item) return;
        if (m_activeListIndex < m_favoriteLists.size())
            m_favoriteLists[m_activeListIndex].items.remove(
                item->data(Qt::UserRole).toString());
        delete item;
        scheduleSave();
    });

    // リスト名変更
    connect(renameBtn, &QPushButton::clicked, dlg, [this, &tabBtns, dlg]() {
        if (m_activeListIndex >= m_favoriteLists.size()) return;
        bool ok;
        QString newName = QInputDialog::getText(dlg,
            QString::fromUtf8("\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88\xe5\x90\x8d\xe5\xa4\x89\xe6\x9b\xb4"),
            QString::fromUtf8("\xe6\x96\xb0\xe3\x81\x97\xe3\x81\x84\xe5\x90\x8d\xe5\x89\x8d\xef\xbc\x9a"),
            QLineEdit::Normal,
            m_favoriteLists[m_activeListIndex].name, &ok);
        if (ok && !newName.isEmpty()) {
            m_favoriteLists[m_activeListIndex].name = newName;
            tabBtns[m_activeListIndex]->setText(newName);
            scheduleSave();
        }
    });

    // リストを削除
    connect(delListBtn, &QPushButton::clicked, dlg, [this, dlg]() {
        if (m_favoriteLists.size() <= 1) {
            QMessageBox::warning(dlg,
                QString::fromUtf8("\xe8\xad\xa6\xe5\x91\x8a"),
                QString::fromUtf8("\xe6\x9c\x80\xe5\xbe\x8c\xe3\x81\xae\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88\xe3\x81\xaf\xe5\x89\x8a\xe9\x99\xa4\xe3\x81\xa7\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93"));
            return;
        }
        m_favoriteLists.removeAt(m_activeListIndex);
        if (m_activeListIndex >= m_favoriteLists.size())
            m_activeListIndex = m_favoriteLists.size() - 1;
        scheduleSave();
        dlg->accept();  // ダイアログを閉じて再表示
        QTimer::singleShot(0, this, &MainWindow::onShowFavorites);
    });

    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::accept);
    dlg->exec();
    dlg->deleteLater();
}

// ★ v10修正：INI保存を1本に直列化する。終了時の同期保存(GUIスレッド)と
//   2秒後のバックグラウンド保存が同時に走ると、同じ .tmp に同時に書いたり、
//   書き込み中のスナップショットをGUI側が上書きしたりして、INI消失や
//   クラッシュの原因になっていた。スナップショットの代入と書き込みの両方を
//   このmutexで囲む（Playerの最後のフォルダ保存とも共通。IniFileLock.h参照）。

// 終了時専用：GUIスレッドで即時・同期保存
void MainWindow::saveFavorites()
{
    if (m_iniSaveTimer) m_iniSaveTimer->stop();
    QMutexLocker iniLock(&iniFileMutex());

    // ★ スナップショットを設定してから書き込む
    m_dspOffSnapshot          = m_player->dspOff();
    m_chainOnSnapshot         = m_player->chainOn();
    m_savedPowerPlanSnapshot  = m_savedPowerPlan;
    m_favListsSnapshot        = m_favoriteLists;
    m_activeListIndexSnapshot = m_activeListIndex;

    bool bpOn = !m_bpActOff->isChecked();
    int  bpRate = 0, bpBits = 0;
    if (bpOn) {
        for (QAction *act : m_bitPerfectBtn->menu()->actions()) {
            if (act->isChecked() && act->data().isValid()) {
                QVariantList d = act->data().toList();
                bpRate = d[0].toInt();
                bpBits = d[1].toInt();
                break;
            }
        }
    }
    writeFavoritesToDisk(m_favorites, m_soundField, bpOn, bpRate, bpBits);
    m_iniDirty = false;
}

// 操作時専用：最後の操作から2秒後にバックグラウンドで保存
// 連続操作はタイマーリセットでまとめて1回の書き込みに集約される
void MainWindow::scheduleSave()
{
    m_iniDirty = true;
    if (!m_iniSaveTimer) {
        m_iniSaveTimer = new QTimer(this);
        m_iniSaveTimer->setSingleShot(true);
        connect(m_iniSaveTimer, &QTimer::timeout, this, [this] {
            if (!m_iniDirty) return;

            // ★ GUIスレッドで全スナップショットを取得（バックグラウンドスレッドから
            //    メンバーに触らないためにここで全て値コピーする）
            // ★ v10修正：バックグラウンド書き込み中にスナップショットを上書きしないよう、
            //   書き込み側と同じmutexで囲む（書き込み中なら終わるまで待つ）。
            QMutexLocker snapLock(&iniFileMutex());
            m_dspOffSnapshot          = m_player->dspOff();
            m_chainOnSnapshot         = m_player->chainOn();
            m_savedPowerPlanSnapshot  = m_savedPowerPlan;
            m_favListsSnapshot        = m_favoriteLists;
            m_activeListIndexSnapshot = m_activeListIndex;

            QString sfSnapshot = m_soundField;
            bool   bpOn   = !m_bpActOff->isChecked();
            int    bpRate = 0, bpBits = 0;
            if (bpOn) {
                for (QAction *act : m_bitPerfectBtn->menu()->actions()) {
                    if (act->isChecked() && act->data().isValid()) {
                        QVariantList d = act->data().toList();
                        bpRate = d[0].toInt();
                        bpBits = d[1].toInt();
                        break;
                    }
                }
            }
            m_iniDirty = false;
            snapLock.unlock();
            // バックグラウンドスレッドには引数だけ渡す（this経由でメンバーを読むのはNG）
            QThreadPool::globalInstance()->start([this, sfSnapshot, bpOn, bpRate, bpBits] {
                QMutexLocker writeLock(&iniFileMutex());
                writeFavoritesToDisk({}, sfSnapshot, bpOn, bpRate, bpBits);
            });
        });
    }
    m_iniSaveTimer->start(2000);  // 2秒後に書き込み（連続操作はリセット）
}

// 実際のディスク書き込み：同期・非同期どちらからでも呼べる純粋な関数
// ★ 引数で全データを受け取る（バックグラウンドスレッドからメンバーに触らない）
void MainWindow::writeFavoritesToDisk(const QMap<QString, QString> &favorites,
                                      const QString &soundField,
                                      bool bpOn, int bpRate, int bpBits)
{
    QString ini = QDir::homePath() + "/AlwaysPlayer.ini";
    QString tmp = QDir::homePath() + "/AlwaysPlayer.ini.tmp";
    QString bak = QDir::homePath() + "/AlwaysPlayer.ini.bak";

    // 既存のiniから管理対象セクション以外を保持
    QStringList lines;
    {
        QFile rf(ini);
        if (rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&rf);
            in.setEncoding(QStringConverter::Utf8);
            bool inFav = false;
            while (!in.atEnd()) {
                QString line = in.readLine();
                if (line.trimmed() == "[favorites]")    { inFav = true;  continue; }
                if (line.trimmed() == "[sound_field]")  { inFav = true;  continue; }
                if (line.trimmed() == "[dsp]")           { inFav = true;  continue; }
                if (line.trimmed() == "[bitperfect]")    { inFav = true;  continue; }
                if (line.trimmed() == "[power]")         { inFav = true;  continue; }
                if (line.trimmed() == "[favorites_meta]"){ inFav = true;  continue; }
                if (line.trimmed().startsWith("[favorites_list_")) { inFav = true; continue; }
                if (line.startsWith("[") && inFav)     { inFav = false; }
                if (!inFav) lines << line;
            }
        }
    }

    // ① tmp に書く（この時点では ini は無傷）
    {
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&f);
        out.setEncoding(QStringConverter::Utf8);
        for (const auto &l : lines) out << l << "\n";
        out << "[sound_field]\n";
        out << "value=" << soundField << "\n";
        out << "[dsp]\n";
        // ★ 引数で受け取った値を使う（バックグラウンドスレッド安全）
        out << "dsp_off="   << (m_dspOffSnapshot   ? "1" : "0") << "\n";
        out << "chain_off=" << (m_chainOnSnapshot  ? "0" : "1") << "\n";
        out << "[power]\n";
        if (!m_savedPowerPlanSnapshot.isEmpty())
            out << "plan=" << m_savedPowerPlanSnapshot << "\n";
        out << "[bitperfect]\n";
        out << "enabled=" << (bpOn ? "1" : "0") << "\n";
        if (bpOn && bpRate > 0) {
            out << "rate=" << bpRate << "\n";
            out << "bits=" << bpBits << "\n";
        }
        out << "[favorites_meta]\n";
        out << "count=" << m_favListsSnapshot.size() << "\n";
        out << "active=" << m_activeListIndexSnapshot << "\n";
        for (int i = 0; i < m_favListsSnapshot.size(); ++i) {
            out << "[favorites_list_" << i << "]\n";
            out << "name=" << m_favListsSnapshot[i].name << "\n";
            for (auto it = m_favListsSnapshot[i].items.begin();
                 it != m_favListsSnapshot[i].items.end(); ++it)
                out << it.key() << "|" << it.value() << "\n";
        }
        out.flush();
        f.flush();
        // ★ v10修正：close()だけではOSのキャッシュに残るだけなので、
        //   FlushFileBuffersで実際にディスクまで書き切ってから置き換える。
        if (f.handle() >= 0)
            FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(f.handle())));
        f.close();
        if (out.status() != QTextStream::Ok || f.error() != QFileDevice::NoError) {
            QFile::remove(tmp);   // 書き込み失敗：iniには一切触らない
            return;
        }
    }

    // ② ini → bak（バックアップ）。
    // ★ v10修正：以前は無条件に bak を先に消していたため、ini が無い状態で
    //   保存が走ると bak まで失われていた。ini が存在するときだけ更新する。
    if (QFile::exists(ini)) {
        QFile::remove(bak);
        QFile::copy(ini, bak);
    }

    // ③ tmp → ini を1回のAPIで上書き置換する（本当のアトミック置換）。
    // ★ v10修正：以前は QFile::remove(ini) → QFile::rename(tmp, ini) の2段階で、
    //   その間に落ちる、またはウイルス対策ソフト等が ini を掴んでいて rename が
    //   失敗すると ini が消え、お気に入りが空になっていた。
    //   MoveFileExW(REPLACE_EXISTING) なら ini が存在しない瞬間がない。
    const std::wstring tmpW = QDir::toNativeSeparators(tmp).toStdWString();
    const std::wstring iniW = QDir::toNativeSeparators(ini).toStdWString();
    bool moved = false;
    for (int attempt = 0; attempt < 5 && !moved; ++attempt) {
        if (attempt > 0) Sleep(100);   // 他プロセスが一時的に掴んでいる場合に備えて再試行
        moved = MoveFileExW(tmpW.c_str(), iniW.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!moved)
        qWarning() << "[INI] MoveFileExW failed:" << GetLastError() << "(ini は旧内容のまま)";
}

void MainWindow::loadFavorites()
{
    m_favorites.clear();
    QString ini = QDir::homePath() + "/AlwaysPlayer.ini";
    // ★ v10修正：ini が無い（過去の保存失敗などで消えた）ときは bak から復元する。
    //   以前は復元処理が無く、空のまま起動→次の保存で空リストが確定していた。
    {
        const QString bak = QDir::homePath() + "/AlwaysPlayer.ini.bak";
        if (!QFile::exists(ini) && QFile::exists(bak)) {
            qWarning() << "[INI] AlwaysPlayer.ini が見つからないため .bak から復元します";
            QFile::copy(bak, ini);
        }
    }
    QFile f(ini);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    bool inFav   = false;
    bool inSound = false;
    bool inDsp   = false;
    bool inBp    = false;
    bool inMeta  = false;
    bool inPower = false;
    int  inListIdx = -1;
    int  bpRate  = 0, bpBits = 0, bpEnabled = 0;
    QString savedPowerPlan;
    m_favoriteLists.clear();

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line == "[sound_field]")   { inSound = true;  inFav = false; inDsp = false; inBp = false; inMeta = false; inPower = false; inListIdx = -1; continue; }
        if (line == "[dsp]")           { inDsp   = true;  inFav = false; inSound = false; inBp = false; inMeta = false; inPower = false; inListIdx = -1; continue; }
        if (line == "[bitperfect]")    { inBp    = true;  inFav = false; inSound = false; inDsp = false; inMeta = false; inPower = false; inListIdx = -1; continue; }
        if (line == "[power]")         { inPower = true;  inFav = false; inSound = false; inDsp = false; inBp = false; inMeta = false; inListIdx = -1; continue; }
        if (line == "[favorites_meta]"){ inMeta  = true;  inFav = false; inSound = false; inDsp = false; inBp = false; inPower = false; inListIdx = -1; continue; }
        if (line == "[favorites]")     { inFav   = true;  inSound = false; inDsp = false; inBp = false; inMeta = false; inPower = false; inListIdx = -1; continue; }
        if (line.startsWith("[favorites_list_")) {
            int n = line.mid(16).remove(']').toInt();
            while (m_favoriteLists.size() <= n) m_favoriteLists.append(FavoriteList());
            inListIdx = n;
            inFav = false; inSound = false; inDsp = false; inBp = false; inMeta = false; inPower = false;
            continue;
        }
        if (line.startsWith("[")) { inFav = false; inSound = false; inDsp = false; inBp = false; inMeta = false; inPower = false; inListIdx = -1; continue; }

        if (inSound && line.startsWith("value="))
            m_soundField = line.mid(6);
        if (inDsp && line.startsWith("dsp_off="))
            m_player->setDspOff(line.mid(8) == "1");
        if (inDsp && line.startsWith("chain_off="))
            m_player->setChainOn(line.mid(10) == "0");
        if (inBp && line.startsWith("enabled="))
            bpEnabled = line.mid(8).toInt();
        if (inBp && line.startsWith("rate="))
            bpRate = line.mid(5).toInt();
        if (inBp && line.startsWith("bits="))
            bpBits = line.mid(5).toInt();
        if (inPower && line.startsWith("plan=")) {
            savedPowerPlan = line.mid(5).trimmed();
            m_savedPowerPlan = savedPowerPlan;
        }
        if (inMeta && line.startsWith("active="))
            m_activeListIndex = line.mid(7).toInt();

        // 新形式：リストの曲
        if (inListIdx >= 0 && inListIdx < m_favoriteLists.size()) {
            if (line.startsWith("name="))
                m_favoriteLists[inListIdx].name = line.mid(5);
            else if (line.contains("|")) {
                int sep = line.indexOf("|");
                m_favoriteLists[inListIdx].items[line.left(sep)] = line.mid(sep + 1);
            }
        }

        // 旧形式（[favorites]）→ リスト1として読み込み（後方互換）
        if (inFav && line.contains("|")) {
            if (m_favoriteLists.isEmpty()) {
                FavoriteList fl;
                fl.name = QString::fromUtf8("\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88") + "1";
                m_favoriteLists.append(fl);
            }
            int sep = line.indexOf("|");
            m_favoriteLists[0].items[line.left(sep)] = line.mid(sep + 1);
        }
    }

    // リストが空なら初期リストを作成
    if (m_favoriteLists.isEmpty()) {
        FavoriteList fl;
        fl.name = QString::fromUtf8("\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x88") + "1";
        m_favoriteLists.append(fl);
    }
    if (m_activeListIndex >= m_favoriteLists.size())
        m_activeListIndex = 0;
    // 読み込んだ音場効果をPlayerに反映
    if (m_player && !m_soundField.isEmpty())
        m_player->setMode("dsd8", false, false, m_soundField);

    // BitPerfect設定を復元
    if (bpEnabled && bpRate > 0 && bpBits > 0) {
        m_bpManualOff = false;  // ★ ONで保存されていたのでフラグ解除
        QMenu *bpMenu = m_bitPerfectBtn->menu();
        for (QAction *act : bpMenu->actions()) {
            if (act->data().isValid()) {
                QVariantList d = act->data().toList();
                if (d[0].toInt() == bpRate && d[1].toInt() == bpBits) {
                    act->setChecked(true);
                    m_bitPerfectBtn->setText(
                        QString("BitPerfect %1kHz/%2 ▼")
                        .arg(bpRate / 1000.0, 0, 'f', bpRate % 1000 == 0 ? 0 : 1)
                        .arg(bpBits));
                    m_lastAppliedRate = bpRate;
                    m_lastAppliedBits = bpBits;
                    // ★ 起動時に保存済みの固定レートを復元した場合も、
                    //   曲が変わるたびに踏み潰されないよう固定フラグを立てる。
                    m_bpManualRatePinned = true;
                    m_pinnedBpRate = bpRate;
                    m_pinnedBpBits = bpBits;
                    m_player->setManualRateOverride(true);
                    // v10: 保存されていた出力形式を自作エンジンへ
                    m_player->setPinnedOutput(bpRate, bpBits);
                    break;
                }
            }
        }
    } else {
        m_bpManualOff = true;   // ★ OFFで保存されていたので手動OFFフラグを立てる
        m_bpManualRatePinned = false;
        m_player->setManualRateOverride(false);
        m_bpActOff->setChecked(true);
        m_bitPerfectBtn->setText("BitPerfect ▼");
    }

    // DSP OFFが保存されていた場合はモードボタンを起動時に復元
    if (m_player && m_player->dspOff()) {
        QMap<QString,QString> dspOffLabels;
        dspOffLabels["hires4"]   = jp("\xe3\x83\x8f\xe3\x82\xa4\xe3\x83\xac\xe3\x82\xbe x4  \xef\xbc\x88\xe4\xbd\xbf\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
        dspOffLabels["dsd8"]     = jp("\xe7\x96\x91\xe4\xbc\xbc" "DSD x8  \xef\xbc\x88\xe4\xbd\xbf\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
        dspOffLabels["loudness"] = jp("\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\x89\xe3\x83\x8d\xe3\x82\xb9  \xef\xbc\x88\xe4\xbd\xbf\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
        for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it) {
            it.value()->setEnabled(it.key() == "pure");
            it.value()->setChecked(it.key() == "pure");
            if (it.key() != "pure" && dspOffLabels.contains(it.key()))
                it.value()->setText(dspOffLabels[it.key()]);
        }
        // HP1・HP2もdisabled
        if (m_hp1Btn) { m_hp1On = false; m_hp1Btn->setChecked(false); m_hp1Btn->setEnabled(false); }
        if (m_hp2Btn) { m_hp2On = false; m_hp2Btn->setChecked(false); m_hp2Btn->setEnabled(false); }
        m_player->setMode("pure", false, false, "");
        updateModeDesc("pure");
    }

    // 電源プランを起動時に自動適用（保存済みプランが現在と異なる場合）
    if (!savedPowerPlan.isEmpty()) {
        QTimer::singleShot(1500, this, [this, savedPowerPlan]() {
            SHELLEXECUTEINFOW sei = {};
            sei.cbSize = sizeof(sei);
            sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb = L"runas";
            sei.lpFile = L"powercfg.exe";
            QString params = QString("/setactive %1").arg(savedPowerPlan);
            sei.lpParameters = reinterpret_cast<LPCWSTR>(params.utf16());
            sei.nShow = SW_HIDE;
            if (ShellExecuteExW(&sei) && sei.hProcess) {
                WaitForSingleObject(sei.hProcess, 3000);
                CloseHandle(sei.hProcess);
                // ステータスバーにメッセージ表示（5秒後に消える）
                if (m_statusBar) {
                    m_statusBar->setText(QString::fromUtf8(
                        "\xe9\x9b\xbb\xe6\xba\x90\xe3\x83\x97\xe3\x83\xa9\xe3\x83\xb3\xe3\x82\x92\xe4\xbb\xa5\xe5\x89\x8d\xe3\x81\xae\xe8\xa8\xad\xe5\xae\x9a\xe3\x81\xab\xe6\x88\xbb\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f\xe3\x80\x82"));
                    QTimer::singleShot(5000, this, [this]() {
                        if (m_statusBar) m_statusBar->setText(" ");
                    });
                }
            }
        });
    }
}

QString MainWindow::currentMode() const
{
    for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it)
        if (it.value()->isChecked()) return it.key();
    return "dsd8";
}

void MainWindow::onSleepTimer()
{
    QMenu *menu = new QMenu(this);

    QActionGroup *grp = new QActionGroup(menu);
    grp->setExclusive(true);

    // ── OFF
    auto *aOff = menu->addAction("OFF", [this] {
        if (m_sleepTimer) { m_sleepTimer->stop(); delete m_sleepTimer; m_sleepTimer = nullptr; }
        m_sleepBtn->setText("Timer");
    });
    aOff->setCheckable(true);
    aOff->setChecked(m_sleepTimer == nullptr);
    grp->addAction(aOff);

    // ── 時間×動作の組み合わせ
    struct Item { int minutes; int action; };  // action: 1=スリープ 2=シャットダウン
    const QList<Item> items = {
        {15, 1}, {15, 2},
        {30, 1}, {30, 2},
        {60, 1}, {60, 2},
        {90, 1}, {90, 2},
        {120,1}, {120,2},
    };

    int prevMin = -1;
    for (const auto &item : items) {
        if (item.minutes != prevMin) {
            menu->addSeparator();
            prevMin = item.minutes;
        }
        QString actionLabel = (item.action == 1)
            ? QString::fromUtf8("\xe3\x82\xb9\xe3\x83\xaa\xe3\x83\xbc\xe3\x83\x97")
            : QString::fromUtf8("\xe3\x82\xb7\xe3\x83\xa3\xe3\x83\x83\xe3\x83\x88\xe3\x83\x80\xe3\x82\xa6\xe3\x83\xb3");
        QString label = QString("%1").arg(item.minutes)
                + QString::fromUtf8("\xe5\x88\x86\xef\xbc\x88")
                + actionLabel
                + QString::fromUtf8("\xef\xbc\x89");

        int min = item.minutes;
        int act = item.action;
        auto *a = menu->addAction(label, [this, min, act] {
            if (m_sleepTimer) { m_sleepTimer->stop(); delete m_sleepTimer; m_sleepTimer = nullptr; }
            m_sleepSeconds = min * 60;
            m_sleepAction  = act;
            m_sleepTimer   = new QTimer(this);
            connect(m_sleepTimer, &QTimer::timeout, this, [this] {
                m_sleepSeconds--;
                int m = m_sleepSeconds / 60, s = m_sleepSeconds % 60;
                m_sleepBtn->setText(QString("%1:%2")
                    .arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')));
                if (m_sleepSeconds <= 0) {
                    m_sleepTimer->stop();
                    m_sleepBtn->setText("Timer");
                    m_player->stop();
                    if (m_sleepAction == 2)
                        QProcess::startDetached("shutdown", {"/s", "/t", "0"});
                    else if (m_sleepAction == 1)
                        QProcess::startDetached("rundll32", {"powrprof.dll,SetSuspendState", "0,1,0"});
                }
            });
            m_sleepTimer->start(1000);
            QString actionLabel2 = (act == 1)
                ? QString::fromUtf8("\xe3\x82\xb9\xe3\x83\xaa\xe3\x83\xbc\xe3\x83\x97")
                : QString::fromUtf8("\xe3\x82\xb7\xe3\x83\xa3\xe3\x83\x83\xe3\x83\x88\xe3\x83\x80\xe3\x82\xa6\xe3\x83\xb3");
            m_sleepBtn->setText(QString("%1:00 ").arg(min, 2, 10, QChar('0')) + actionLabel2);
        });
        a->setCheckable(true);
        a->setChecked(m_sleepTimer != nullptr &&
                      m_sleepAction == act &&
                      m_sleepSeconds > (min - 1) * 60 &&
                      m_sleepSeconds <= min * 60);
        grp->addAction(a);
    }

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
    dlg->setWindowTitle("Settings \xe2\x80\x94 Always Player");
    dlg->setMinimumWidth(780);
    dlg->resize(820, 560);

    QVBoxLayout *outerVl = new QVBoxLayout(dlg);
    outerVl->setContentsMargins(0, 0, 0, 8);
    outerVl->setSpacing(0);

    QScrollArea *scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    QWidget *scrollContent = new QWidget();
    QVBoxLayout *vl = new QVBoxLayout(scrollContent);
    vl->setContentsMargins(16, 16, 16, 8);
    vl->setSpacing(10);
    scroll->setWidget(scrollContent);
    outerVl->addWidget(scroll, 1);

    // ══ 1行目：電源プラン ＋ USB最適化 ══
    auto *row1 = new QHBoxLayout();
    row1->setSpacing(10);

    // ── 電源プラン
    auto *grpPower = new QGroupBox("\xe9\x9b\xbb\xe6\xba\x90\xe3\x83\x97\xe3\x83\xa9\xe3\x83\xb3");
    auto *pvl = new QVBoxLayout(grpPower);
    // 電源プランの表示：INI保存値を優先、なければ現在値
    QString curPlan = m_savedPowerPlan.isEmpty()
                      ? getCurrentPowerPlan().toLower()
                      : m_savedPowerPlan.toLower();
    auto *rbHigh    = new QRadioButton("\xe9\xab\x98\xe3\x83\x91\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xbc\xe3\x83\x9e\xe3\x83\xb3\xe3\x82\xb9\xef\xbc\x88\xe3\x83\x87\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\x88\xe3\x83\x83\xe3\x83\x97\xe6\x8e\xa8\xe5\xa5\xa8\xef\xbc\x89");
    auto *rbBalance = new QRadioButton(QString::fromUtf8("\xe3\x83\x90\xe3\x83\xa9\xe3\x83\xb3\xe3\x82\xb9\xef\xbc\x88\xe3\x83\x8e\xe3\x83\xbc\xe3\x83\x88") + "PC" + QString::fromUtf8("\xe6\x8e\xa8\xe5\xa5\xa8\xef\xbc\x89"));
    auto *rbSaver   = new QRadioButton("\xe7\x9c\x81\xe9\x9b\xbb\xe5\x8a\x9b");
    if (curPlan.contains("8c5e7fda")) rbHigh->setChecked(true);
    else if (curPlan.contains("381b4222")) rbBalance->setChecked(true);
    else rbSaver->setChecked(true);
    pvl->addWidget(rbHigh);
    pvl->addWidget(rbBalance);
    pvl->addWidget(rbSaver);
    auto *applyPowerBtn = new QPushButton("\xe9\x9b\xbb\xe6\xba\x90\xe3\x83\x97\xe3\x83\xa9\xe3\x83\xb3\xe3\x82\x92\xe5\xa4\x89\xe6\x9b\xb4\xef\xbc\x88\xe7\xae\xa1\xe7\x90\x86\xe8\x80\x85\xe6\xa8\xa9\xe9\x99\x90\xe3\x81\x8c\xe5\xbf\x85\xe8\xa6\x81\xef\xbc\x89");
    applyPowerBtn->setObjectName("toolBtn");
    pvl->addWidget(applyPowerBtn);
    pvl->addStretch();
    connect(applyPowerBtn, &QPushButton::clicked, dlg, [=]() {
        QString guid = rbHigh->isChecked() ? PLAN_HIGH :
                       rbBalance->isChecked() ? PLAN_BALANCE : PLAN_SAVER;
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = L"powercfg.exe";
        QString params = QString("/setactive %1").arg(guid);
        sei.lpParameters = reinterpret_cast<LPCWSTR>(params.utf16());
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 3000);
            CloseHandle(sei.hProcess);
            m_savedPowerPlan = guid;  // メンバ変数に保存
            QMessageBox::information(dlg, QString::fromUtf8("\xe5\xae\x8c\xe4\xba\x86"),
                QString::fromUtf8("\xe9\x9b\xbb\xe6\xba\x90\xe3\x83\x97\xe3\x83\xa9\xe3\x83\xb3\xe3\x82\x92\xe5\xa4\x89\xe6\x9b\xb4\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f\xe3\x80\x82"));
            scheduleSave();  // 電源プランをINIに保存
        } else {
            QMessageBox::warning(dlg, QString::fromUtf8("\xe3\xa8\xa8\xe3\x83\xa9\xe3\x83\xbc"),
                QString::fromUtf8("\xe5\xa4\x89\xe6\x9b\xb4\xe3\x81\xab\xe5\xa4\xb1\xe6\x95\x97\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f\xe3\x80\x82"));
        }
    });
    row1->addWidget(grpPower, 1);

    // ── USB最適化
    auto *grpUsb = new QGroupBox("USB\xe6\x9c\x80\xe9\x81\xa9\xe5\x8c\x96\xef\xbc\x88\xe7\xae\xa1\xe7\x90\x86\xe8\x80\x85\xe6\xa8\xa9\xe9\x99\x90\xe3\x81\x8c\xe5\xbf\x85\xe8\xa6\x81\xef\xbc\x89");
    auto *uvl = new QVBoxLayout(grpUsb);
    DWORD suspendVal = readRegDword(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Services\\USB",
        "DisableSelectiveSuspend", 0);
    auto *cbSuspend = new QCheckBox(QString::fromUtf8("USB\xe3\x82\xbb\xe3\x83\xac\xe3\x82\xaf\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x96\xe3\x82\xb5\xe3\x82\xb9\xe3\x83\x9a\xe3\x83\xb3\xe3\x83\x89\xe3\x82\x92\xe7\x84\xa1\xe5\x8a\xb9\xe5\x8c\x96\xef\xbc\x88") + "USB" + QString::fromUtf8("\xe9\x9b\xbb\xe6\xba\x90\xe3\x82\x92\xe5\xae\x89\xe5\xae\x9a\xe3\x81\x95\xe3\x81\x9b\xe3\x82\x8b\xef\xbc\x89"));
    cbSuspend->setChecked(suspendVal == 1);
    uvl->addWidget(cbSuspend);
    DWORD fastBoot = readRegDword(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
        "HiberbootEnabled", 1);
    auto *cbFastBoot = new QCheckBox(QString::fromUtf8("\xe9\xab\x98\xe9\x80\x9f\xe3\x82\xb9\xe3\x82\xbf\xe3\x83\xbc\xe3\x83\x88\xe3\x82\xa2\xe3\x83\x83\xe3\x83\x97\xe3\x82\x92\xe7\x84\xa1\xe5\x8a\xb9\xe5\x8c\x96\xef\xbc\x88") + "USB" + QString::fromUtf8("\xe5\x88\x9d\xe6\x9c\x9f\xe5\x8c\x96\xe3\x82\x92\xe6\xad\xa3\xe5\xb8\xb8\xe5\x8c\x96\xe3\x81\x99\xe3\x82\x8b\xef\xbc\x89"));
    cbFastBoot->setChecked(fastBoot == 0);
    uvl->addWidget(cbFastBoot);
    auto *applyUsbBtn = new QPushButton("USB\xe8\xa8\xad\xe5\xae\x9a\xe3\x82\x92\xe9\x81\xa9\xe7\x94\xa8\xef\xbc\x88\xe7\xae\xa1\xe7\x90\x86\xe8\x80\x85\xe6\xa8\xa9\xe9\x99\x90\xe3\x81\x8c\xe5\xbf\x85\xe8\xa6\x81\xef\xbc\x89");
    applyUsbBtn->setObjectName("toolBtn");
    uvl->addWidget(applyUsbBtn);
    uvl->addStretch();
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
        QMessageBox::information(dlg, "\xe5\xae\x8c\xe4\xba\x86",
            "USB\xe8\xa8\xad\xe5\xae\x9a\xe3\x82\x92\xe9\x81\xa9\xe7\x94\xa8\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f\xe3\x80\x82\n\xe4\xb8\x80\xe9\x83\xa8\xe3\x81\xae\xe8\xa8\xad\xe5\xae\x9a\xe3\x81\xaf\xe5\x86\x8d\xe8\xb5\xb7\xe5\x8b\x95\xe5\xbe\x8c\xe3\x81\xab\xe6\x9c\x89\xe5\x8a\xb9\xe3\x81\xab\xe3\x81\xaa\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82");
    });
    row1->addWidget(grpUsb, 1);
    vl->addLayout(row1);

    // ══ 2行目：中密度チェーン ＋ 音場効果 ══
    auto *row2 = new QHBoxLayout();
    row2->setSpacing(10);

    // ── 中密度チェーン
    auto *grpChain = new QGroupBox(QString::fromUtf8("\xe4\xb8\xad\xe5\xaf\x86\xe5\xba\xa6\xe3\x83\x81\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\xb3"));
    auto *cvl = new QVBoxLayout(grpChain);
    cvl->setSpacing(4);
    auto *cbChainOff = new QCheckBox(
        QString::fromUtf8("\xe4\xb8\xad\xe5\xaf\x86\xe5\xba\xa6\xe3\x83\x81\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\xb3\xe3\x82\x92\xe6\x9c\x89\xe5\x8a\xb9\xe3\x81\xab\xe3\x81\x99\xe3\x82\x8b\xef\xbc\x88")
        + "Always Player "
        + QString::fromUtf8("\xe6\x8e\xa8\xe5\xa5\xa8\xe8\xa8\xad\xe5\xae\x9a\xef\xbc\x89"));
    cbChainOff->setChecked(m_player->chainOn());
    QString chainText =
        QString::fromUtf8("  \xe3\x83\x97\xe3\x83\xac\xe3\x82\xbc\xe3\x83\xb3\xe3\x82\xb9\xe6\x95\xb4\xe5\x90\x88")
        + "(3.2kHz)\n\n  "
        + QString::fromUtf8("\xe4\xbd\x8d\xe7\x9b\xb8\xe6\x95\xb4\xe5\x90\x88")
        + "(allpass 8kHz)\n\n  "
        + QString::fromUtf8("\xe7\xa9\xba\xe8\x8a\xaf\xe3\x82\xb3\xe3\x82\xa4\xe3\x83\xab\xe7\x89\xb9\xe6\x80\xa7")
        + "(lowpass 45kHz / poles=1)\n\n  "
        + QString::fromUtf8("\xe5\x81\xb6\xe6\x95\xb0\xe6\xac\xa1\xe9\xab\x98\xe8\xaa\xbf\xe6\xb3\xa2")
        + "("
        + QString::fromUtf8("\xe4\xbf\x82\xe6\x95\xb0")
        + " 0.03)";
    auto *chainDetail = new QLabel(chainText);
    chainDetail->setObjectName("infoLabel");
    chainDetail->setWordWrap(true);
    cvl->addWidget(cbChainOff);
    cvl->addWidget(chainDetail);
    cvl->addStretch();
    connect(cbChainOff, &QCheckBox::clicked, this, [this](bool checked) {
        m_player->setChainOn(checked);
        scheduleSave();
    });
    row2->addWidget(grpChain, 1);

    // ── 音場効果
    auto *grpSound = new QGroupBox(QString::fromUtf8("\xe9\x9f\xb3\xe9\x9f\xbf\xe8\xa8\xad\xe5\xae\x9a\xef\xbc\x88\xe3\x82\xaa\xe3\x83\x97\xe3\x82\xb7\xe3\x83\xa7\xe3\x83\xb3\xef\xbc\x89"));
    auto *svl = new QVBoxLayout(grpSound);
    svl->setSpacing(4);
    auto *cbWow  = new QCheckBox(QString::fromUtf8("\xe3\x83\xaf\xe3\x82\xa6\xe3\x83\x95\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xbf\xe3\x83\xbc\xef\xbc\x88\xe3\x82\xa2\xe3\x83\x8a\xe3\x83\xad\xe3\x82\xb0\xe3\x83\xac\xe3\x82\xb3\xe3\x83\xbc\xe3\x83\x89\xe5\x8c\x96\xef\xbc\x89"));
    auto *cbHall = new QCheckBox(QString::fromUtf8("\xe3\x83\x9b\xe3\x83\xbc\xe3\x83\xab\xe3\x83\x88\xe3\x83\xbc\xe3\x83\xb3\xef\xbc\x88\xe3\x83\x8b\xe3\x82\xa2\xe3\x83\x95\xe3\x82\xa3\xe3\x83\xbc\xe3\x83\xab\xe3\x83\x89\xe5\xbc\xb7\xe5\x8c\x96\xef\xbc\x89"));
    if (m_soundField == "wowflutter") cbWow->setChecked(true);
    else if (m_soundField == "halltone") cbHall->setChecked(true);
    svl->addWidget(cbWow);
    svl->addWidget(cbHall);
    svl->addStretch();
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
    row2->addWidget(grpSound, 1);
    vl->addLayout(row2);

    // ══ 3行目：DSP完全バイパス（全幅）══
    auto *grpDsp = new QGroupBox(QString::fromUtf8("DSP\xe5\xae\x8c\xe5\x85\xa8\xe3\x83\x90\xe3\x82\xa4\xe3\x83\x91\xe3\x82\xb9"));
    auto *dvl = new QHBoxLayout(grpDsp);
    auto *cbDspOff = new QCheckBox(
        QString::fromUtf8("DSP OFF\xef\xbc\x88\xe3\x82\xa2\xe3\x83\x83\xe3\x83\x97\xe3\x82\xb5\xe3\x83\xb3\xe3\x83\x97\xe3\x83\xaa\xe3\x83\xb3\xe3\x82\xb0\xe3\x83\xbb\xe4\xb8\xad\xe5\xaf\x86\xe5\xba\xa6\xe3\x83\x81\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\xb3\xe3\x82\x92\xe3\x81\x99\xe3\x81\xb9\xe3\x81\xa6\xe7\x84\xa1\xe5\x8a\xb9\xe3\x81\xab\xe3\x81\x99\xe3\x82\x8b\xef\xbc\x89"));
    cbDspOff->setChecked(m_player->dspOff());
    QString dspDescText =
        "OFF" + QString::fromUtf8("\xe3\x81\xab\xe3\x81\x99\xe3\x82\x8b\xe3\x81\xa8\xe3\x82\xbd\xe3\x83\xbc\xe3\x82\xb9\xe4\xbf\xa1\xe5\x8f\xb7\xe3\x82\x92\xe3\x81\x9d\xe3\x81\xae\xe3\x81\xbe\xe3\x81\xbe")
        + "DAC"
        + QString::fromUtf8("\xe3\x81\xab\xe9\x80\x81\xe3\x82\x8b\xe6\x9c\x80\xe7\x9f\xad\xe7\xb5\x8c\xe8\xb7\xaf\xe3\x81\xab\xe3\x81\xaa\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82\n")
        + QString::fromUtf8("\xe3\x83\x93\xe3\x83\x83\xe3\x83\x88\xe3\x83\x91\xe3\x83\xbc\xe3\x83\x95\xe3\x82\xa7\xe3\x82\xaf\xe3\x83\x88\xe5\x87\xba\xe5\x8a\x9b\xe3\x81\xa8\xe7\xb5\x84\xe3\x81\xbf\xe5\x90\x88\xe3\x82\x8f\xe3\x81\x9b\xe3\x82\x8b\xe3\x81\xa8\xe5\xae\x8c\xe5\x85\xa8\xe3\x81\xaa\xe3\x83\x90\xe3\x82\xa4\xe3\x83\x91\xe3\x82\xb9\xe5\x86\x8d\xe7\x94\x9f\xe3\x81\x8c\xe5\x8f\xaf\xe8\x83\xbd\xe3\x81\xa7\xe3\x81\x99\xe3\x80\x82");
    auto *dspDesc = new QLabel(dspDescText);
    dspDesc->setObjectName("infoLabel");
    dspDesc->setWordWrap(true);
    dvl->addWidget(cbDspOff);
    dvl->addWidget(dspDesc, 1);
    connect(cbDspOff, &QCheckBox::clicked, this, [this, cbChainOff, cbWow, cbHall](bool checked) {
        m_player->setDspOff(checked);
        QMap<QString,QString> dspOffLabels;
        dspOffLabels["hires4"]   = jp("\xe3\x83\x8f\xe3\x82\xa4\xe3\x83\xac\xe3\x82\xbe x4  \xef\xbc\x88\xe4\xbd\xbf\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
        dspOffLabels["dsd8"]     = jp("\xe7\x96\x91\xe4\xbc\xbc" "DSD x8  \xef\xbc\x88\xe4\xbd\xbf\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
        dspOffLabels["loudness"] = jp("\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\x89\xe3\x83\x8d\xe3\x82\xb9  \xef\xbc\x88\xe4\xbd\xbf\xe7\x94\xa8\xe4\xb8\x8d\xe5\x8f\xaf\xef\xbc\x89");
        QMap<QString,QString> origLabels;
        origLabels["hires4"]   = jp("\xe3\x83\x8f\xe3\x82\xa4\xe3\x83\xac\xe3\x82\xbe x4");
        origLabels["dsd8"]     = jp("\xe7\x96\x91\xe4\xbc\xbc") + "DSD x8";
        origLabels["loudness"] = jp("\xe3\x83\xa9\xe3\x82\xa6\xe3\x83\x89\xe3\x83\x8d\xe3\x82\xb9");
        for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it) {
            bool enabled = !checked || it.key() == "pure";
            it.value()->setEnabled(enabled);
            if (it.key() != "pure") {
                if (checked && dspOffLabels.contains(it.key()))
                    it.value()->setText(dspOffLabels[it.key()]);
                else if (origLabels.contains(it.key()))
                    it.value()->setText(origLabels[it.key()]);
            }
        }
        if (checked) {
            for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it)
                it.value()->setChecked(it.key() == "pure");
            m_soundField = "";
            cbWow->setChecked(false);
            cbHall->setChecked(false);
            m_player->setChainOn(false);
            cbChainOff->setChecked(false);
            if (m_hp1Btn) { m_hp1On = false; m_hp1Btn->setChecked(false); m_hp1Btn->setEnabled(false); }
            if (m_hp2Btn) { m_hp2On = false; m_hp2Btn->setChecked(false); m_hp2Btn->setEnabled(false); }
            m_player->setMode("pure", false, false, "");
            updateModeDesc("pure");
        } else {
            // ★ DSP OFF 解除 → cbChainOff の現在状態でチェーンを復元
            if (m_hp1Btn) m_hp1Btn->setEnabled(true);
            if (m_hp2Btn) m_hp2Btn->setEnabled(true);
            bool chainShouldBeOn = cbChainOff->isChecked();
            m_player->setChainOn(chainShouldBeOn);
            // モード・HP・音場もまとめて再適用（setDspOff(false) だけでは不十分）
            m_player->setMode(currentMode(), m_hp1On, m_hp2On, m_soundField);
        }
        scheduleSave();
    });
    vl->addWidget(grpDsp);
    
    // ── デフォルトに戻す
    auto *resetBtn = new QPushButton(QString::fromUtf8("\xe3\x81\x99\xe3\x81\xb9\xe3\x81\xa6\xe3\x82\x92\xe3\x83\x87\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x88\xe3\x81\xab\xe6\x88\xbb\xe3\x81\x99"));
    resetBtn->setObjectName("toolBtn");
    connect(resetBtn, &QPushButton::clicked, dlg, [=]() {
        SHELLEXECUTEINFOW seiR = {};
        seiR.cbSize  = sizeof(seiR);
        seiR.lpVerb  = L"runas";
        seiR.lpFile  = L"powercfg.exe";
        seiR.lpParameters = L"/setactive 381b4222-f694-41f0-9685-ff5bb260df2e";
        seiR.nShow   = SW_HIDE;
        ShellExecuteExW(&seiR);
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
        rbBalance->setChecked(true);
        cbSuspend->setChecked(false);
        cbFastBoot->setChecked(false);
        QMessageBox::information(dlg, "\xe5\xae\x8c\xe4\xba\x86",
            "\xe3\x81\x99\xe3\x81\xb9\xe3\x81\xa6\xe3\x82\x92\xe3\x83\x87\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x88\xe3\x81\xab\xe6\x88\xbb\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f\xe3\x80\x82\n\xe5\x86\x8d\xe8\xb5\xb7\xe5\x8b\x95\xe3\x82\x92\xe3\x81\x8a\xe5\x8b\xa7\xe3\x82\x81\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82");
    });

    // ── About
    auto *line = new QFrame();
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    vl->addWidget(line);

    auto *aboutLabel = new QLabel(
        QString("Always Player v10.0.0  (build %1)<br>"
                "High Fidelity PC Audio Player　　"
                "(c) 2026 YOUICHI SAIJO  GPL-3.0<br><br>"
                "<a href='https://always-player.sakuraweb.com/' "
                "style='color:#4db8ff;'>always-player.sakuraweb.com</a><br>")
        .arg(QString::fromLatin1(BUILD_TIMESTAMP)));
    aboutLabel->setAlignment(Qt::AlignCenter);
    aboutLabel->setObjectName("infoLabel");
    aboutLabel->setOpenExternalLinks(true);
    aboutLabel->setWordWrap(true);
    aboutLabel->setTextFormat(Qt::RichText);
    vl->addWidget(aboutLabel);

    // ── 閉じる＋デフォルトに戻す（スクロール外・中央揃え）
    auto *closeBtn = new QPushButton(QString::fromUtf8("\xe9\x96\x89\xe3\x81\x98\xe3\x82\x8b"));
    closeBtn->setObjectName("toolBtn");
    closeBtn->setFixedHeight(36);
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::accept);

    resetBtn->setFixedHeight(36);

    QHBoxLayout *closeLay = new QHBoxLayout();
    closeLay->setContentsMargins(16, 4, 16, 4);
    closeLay->setSpacing(16);
    closeLay->addWidget(resetBtn, 1);
    closeLay->addWidget(closeBtn, 1);
    outerVl->addLayout(closeLay);

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
    // ★ m_bpManualOff は変更しない（内部的な一時解除のみ）
    m_bpActOff->setChecked(true);
    m_bitPerfectBtn->setText("BitPerfect ▼");
    // ★ 排他モードを内部的に一時解除したので、次にexclusive=yesへ戻すときは
    //   同一レート判定でスキップされないようキャッシュを無効化する。
    m_lastAppliedRate = -1;
    m_lastAppliedBits = -1;
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
    QMessageBox::about(this, "Always Player v10.0.0",
        QString("Always Player v10.0.0\n"
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
        QThreadPool::globalInstance()->start([this, folderPath, artPtr, ART_SIZE]() {
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

