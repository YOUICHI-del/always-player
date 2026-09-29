#pragma once
#include <QMainWindow>
#include <QLabel>
#include <QSlider>
#include <QPushButton>
#include <QListWidget>
#include <QStackedWidget>
#include <QLineEdit>
#include <QMap>
#include <QStringList>
#include <QTimer>
#include <QAction>
#include <QScrollArea>
#include <QGridLayout>
#include <QInputDialog>
#include <QJsonObject>
#include <QSet>
#include <QHash>
#include <memory>
#include "Player.h"
#include "VUMeter.h"
#include "TrayManager.h"
#include "CdDrive.h"
#include "CDReader.h"
#include "CdStreamWriter.h"
#include "RingBuffer.h"
#include "CdMetaFetcher.h"

class RemoteServer;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private slots:
    void onSelectFolder();
    void onTrackChanged(int index, const QString &filename,
                        const QString &title, const QString &artist);
    void onPlaybackStarted();
    void onPlaybackStopped();
    void onPlaybackPaused();
    void onSearchChanged(const QString &text);
    void onFavoriteClicked();
    void onShowFavorites();
    void showAbout();
    void showSettings();
    void onSleepTimer();
    void onBrowseAlbums();
    void onCdMetaReady(CdMetaFetcher::Result result);

private:
    void setupUI();
    void setupAlbumBrowser();
    void populateAlbumBrowser(const QString &rootPath);
    QPixmap findAlbumArt(const QString &folderPath, int size);
    QString normalizeForSearch(const QString &text) const;
    void filterAlbumCards(const QString &query);
    void setupTray();
    void applyStyle();
    QString currentMode() const;
    void loadFolder(const QString &path, bool autoPlay = true);
    void saveFavorites();           // 終了時専用：同期・即時保存
    void scheduleSave();            // 操作時専用：遅延＋非同期保存
    void writeFavoritesToDisk(const QMap<QString, QString> &favorites,
                              const QString &soundField,
                              bool bpOn = false, int bpRate = 0, int bpBits = 0);
    void loadFavorites();
    void updateJacket();
    void requestOnlineCover(const QString &audioFile);   // ★ v10: ジャケット画像のネット取得
    void updateModeDesc(const QString &mode);
    void turnOffBitPerfect();
    void clearCdState();

    // ★ CD Stream Mode
    void playCd(const QString &drive);
    void startCdStreamMode();
    void startCdTrackStream(int trackIndex);
    void stopIfCd();
    void stopCdStream();

    // ★ v9: Always Player for Android からのリモコン(Always Link)
    void setupRemote();
    void onRemoteCommand(const QString &cmd, const QJsonObject &obj);
    void remoteBrowse(const QString &path);
    void remoteFolderArt(const QString &path);
    void publishRemoteStatus();
    void remoteSendOutputs();               // v10: 出力方式の一覧をスマホへ
    QString currentBitPerfectKey() const;   // v10: "auto" または "44100/24"
    void remoteSendArtistInfo(const QString &artist);   // v10: アーティスト情報をスマホへ
    QStringList currentArtistList() const;  // 「アーティスト情報」ボタンと同じ分割（複数アーティスト）
    void applyDspOff(bool off);             // DSP完全バイパス（設定画面・リモコン共通）

    Player         *m_player   = nullptr;
    TrayManager    *m_tray     = nullptr;
    VUMeter        *m_vuMeter  = nullptr;
    QStackedWidget *m_stack       = nullptr;
    QWidget        *m_displayWrap = nullptr;

    QLabel      *m_jacket    = nullptr;
    QLabel      *m_title     = nullptr;
    QLabel      *m_subTitle  = nullptr;
    QLabel      *m_modeDesc  = nullptr;
    QLabel      *m_infoLabel = nullptr;
    QLabel      *m_statusBar = nullptr;

    QPushButton *m_prevBtn  = nullptr;
    QPushButton *m_playBtn  = nullptr;
    QPushButton *m_pauseBtn = nullptr;
    QPushButton *m_stopBtn  = nullptr;
    QPushButton *m_nextBtn  = nullptr;
    QPushButton *m_starBtn    = nullptr;
    QPushButton *m_favBtn     = nullptr;
    QPushButton *m_shuffleBtn = nullptr;
    QPushButton *m_repeatBtn  = nullptr;
    QPushButton *m_sleepBtn      = nullptr;
    QPushButton *m_artistInfoBtn  = nullptr;
    QPushButton *m_bitPerfectBtn  = nullptr;
    QAction     *m_bpActOff       = nullptr;
    QAction     *m_dopAct         = nullptr;   // v10: 「DoP出力（DSD対応DACのみ）」
    bool         m_bpManualOff    = false;   // ★ 手動OFFフラグ（true時は自動BitPerfectをスキップ）
    // ★ 「16種類の手動ビットパーフェクト」メニューで特定のレート/ビット数を
    //   選んだかどうかのフラグ。true の間は、曲が変わっても onTrackChanged() 内の
    //   ハイレゾ自動モード判定（dsd8/pure切り替え）で上書きせず、ここに記録した
    //   レート/ビット数をそのまま維持する。
    //   （このフラグが無かったため、手動選択が次の曲で352.8kHz(dsd8)に
    //     勝手に戻ってしまうバグがあった。）
    bool         m_bpManualRatePinned = false;
    // v10: ユーザーが最後に自分で選んだ音質モード。曲が変わっても維持する
    //      （ハイレゾ曲の間だけは強制的にピュア、終われば元に戻す）
    QString      m_userMode = "dsd8";
    int          m_pinnedBpRate   = 0;
    int          m_pinnedBpBits   = 0;
    // ★ 曲間ノイズ対策：直前にWASAPI排他へ実際に適用した出力レート/ビット数を記録。
    //   次の曲も同じ値なら audio-exclusive/audio-samplerate の再設定（＝デバイス
    //   再初期化）自体をスキップし、無用な曲間ノイズ・無音を防ぐ。
    int          m_lastAppliedRate = -1;
    int          m_lastAppliedBits = -1;
    QString      m_currentArtist;
    QString      m_savedPowerPlan;  // INIから読み込んだ電源プランGUID

    QTimer      *m_sleepTimer   = nullptr;
    int          m_sleepSeconds = 0;
    int          m_sleepAction  = 0;  // 0=停止のみ 1=スリープ 2=シャットダウン

    // INI遅延保存
    bool         m_iniDirty      = false;
    QTimer      *m_iniSaveTimer  = nullptr;

    QLineEdit   *m_searchBox = nullptr;
    QSlider     *m_volSlider  = nullptr;
    QSlider     *m_seekSlider = nullptr;
    QLabel      *m_timeLabel  = nullptr;
    bool         m_seekDragging = false;
    QListWidget *m_playlist  = nullptr;
    QTimer      *m_infoTimer = nullptr;

    QMap<QString, QPushButton*> m_modeBtns;

    // ── お気に入り複数リスト
    struct FavoriteList {
        QString name;
        QMap<QString, QString> items;  // path → folderName
    };
    QList<FavoriteList>         m_favoriteLists;
    int                         m_activeListIndex = 0;  // 現在アクティブなリスト
    QMap<QString, QString>      m_favorites;  // 後方互換用（廃止予定）

    // ★ 保存スナップショット（GUIスレッドで取得、バックグラウンドスレッドで参照）
    bool                m_dspOffSnapshot          = false;
    bool                m_chainOnSnapshot         = true;
    QString             m_savedPowerPlanSnapshot;
    QList<FavoriteList> m_favListsSnapshot;
    int                 m_activeListIndexSnapshot  = 0;
    QStringList         m_dopDevicesSnapshot;                  // v10: DoPをONにした出力デバイス

    QString     m_currentFolder;
    QStringList m_allItems;
    QList<int>  m_allIndices;

    bool m_showVU    = true;
    bool m_hasArtwork = false;
    bool m_hp1On  = false;
    bool m_hp2On  = false;
    QPushButton *m_hp1Btn = nullptr;
    QPushButton *m_hp2Btn = nullptr;
    QString     m_soundField;

    // ★ CD再生（MCI方式）
    DiscInfo  m_discInfo;
    QString   m_cdDrive;
    bool      m_isCdMode       = false;
    QSet<QString> m_coverTried;   // ★ v10: このセッションでネット取得を試したアルバム（アーティスト＋アルバム名）
    int       m_cdTrackCount   = 0;
    int       m_cdCurrentTrack = 0;
    bool      m_mciOpen        = false;
    bool      m_mciPlaying     = false;   // ★ MCI実再生中フラグ
    bool      m_cdPaused       = false;
    QTimer   *m_cdTrackTimer   = nullptr;
    std::unique_ptr<RingBuffer>     m_cdBuffer;
    std::unique_ptr<CDReader>       m_cdReader;
    std::unique_ptr<CdStreamWriter> m_cdWriter;
    CdMetaFetcher  *m_cdMetaFetcher = nullptr;

    // ★ リモコン
    RemoteServer   *m_remote        = nullptr;
    QTimer         *m_remoteTimer   = nullptr;
    qint64          m_remoteArtKey  = 0;   // 最後に送ったジャケットの QPixmap::cacheKey
    qint64          m_remoteArtId   = 0;   // スマホへ渡す画像番号（送るたびに+1）
    QHash<QString, QJsonObject> m_remoteArtistCache;   // v10: 調べ済みのアーティスト情報（言語+名前）

    // アルバムブラウザ
    QWidget      *m_mainContent      = nullptr;
    QWidget      *m_albumBrowser     = nullptr;
    QWidget      *m_albumGrid        = nullptr;
    QGridLayout  *m_albumGridLayout  = nullptr;
    QPushButton  *m_albumBrowseBtn   = nullptr;
    QLabel       *m_albumPathLabel   = nullptr;
    QLabel       *m_albumFooterLabel = nullptr;
    QLineEdit    *m_albumSearchBox   = nullptr;

    struct AlbumCardInfo {
        QWidget *card;
        QString  searchKey;
    };
    QList<AlbumCardInfo> m_albumCards;
};
