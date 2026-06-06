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
#include "Player.h"
#include "VUMeter.h"
#include "TrayManager.h"
// ★ CD再生用
#include "CdDrive.h"
#include "CDPlaybackController.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onSelectFolder();
    void onSelectFile();
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
    void saveFavorites();
    void loadFavorites();
    void updateJacket();
    void updateModeDesc(const QString &mode);
    void turnOffBitPerfect();

    // ★ CD再生
    void playCd(const QString &drive);
    void playCdTrack(int trackIndex);
    void onCdTempWavReady(const QString &path);

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
    QString      m_currentArtist;

    QTimer      *m_sleepTimer   = nullptr;
    int          m_sleepSeconds = 0;

    QLineEdit   *m_searchBox = nullptr;
    QSlider     *m_volSlider  = nullptr;
    QSlider     *m_seekSlider = nullptr;
    QLabel      *m_timeLabel  = nullptr;
    bool         m_seekDragging = false;
    QListWidget *m_playlist  = nullptr;
    QTimer      *m_infoTimer = nullptr;

    QMap<QString, QPushButton*> m_modeBtns;
    QMap<QString, QString>      m_favorites;

    QString     m_currentFolder;
    QStringList m_allItems;
    QList<int>  m_allIndices;

    bool m_showVU    = true;
    bool m_hasArtwork = false;
    bool m_hp1On  = false;
    bool m_hp2On  = false;
    QString     m_soundField;

    // ★ CD再生モード
    CDPlaybackController *m_cdController  = nullptr;
    bool                  m_isCdMode      = false;
    int                   m_cdTrackCount  = 0;

    // ★ CD再生タイマー（スライダー・時間表示・EOF検知に使用）
    QTimer               *m_cdTimer       = nullptr;
    int                   m_cdElapsedMs   = 0;
    double                m_cdTrackSec    = 0.0;

    // アルバムブラウザ
    QWidget      *m_mainContent      = nullptr;
    QWidget      *m_albumBrowser     = nullptr;
    QWidget      *m_albumGrid        = nullptr;
    QGridLayout  *m_albumGridLayout  = nullptr;
    QPushButton  *m_albumBrowseBtn   = nullptr;
    QLabel       *m_albumPathLabel   = nullptr;
    QLabel       *m_albumFooterLabel = nullptr;
    QLineEdit    *m_albumSearchBox   = nullptr;

    // アルバムカード情報（検索フィルタ用）
    struct AlbumCardInfo {
        QWidget *card;
        QString  searchKey;  // 正規化済み検索キー
    };
    QList<AlbumCardInfo> m_albumCards;
};
