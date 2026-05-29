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
#include "Player.h"
#include "VUMeter.h"
#include "TrayManager.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onSelectFolder();
    void onSelectFile();
    void onTrackChanged(int index, const QString &filename);
    void onPlaybackStarted();
    void onPlaybackStopped();
    void onPlaybackPaused();
    void onSearchChanged(const QString &text);
    void onFavoriteClicked();
    void onShowFavorites();
    void showAbout();

private:
    void setupUI();
    void setupTray();
    void applyStyle();
    QString currentMode() const;
    void loadFolder(const QString &path);
    void saveFavorites();
    void loadFavorites();
    void updateJacket();

    Player         *m_player   = nullptr;
    TrayManager    *m_tray     = nullptr;
    VUMeter        *m_vuMeter  = nullptr;
    QStackedWidget *m_stack    = nullptr;

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
    QPushButton *m_starBtn  = nullptr;
    QPushButton *m_favBtn   = nullptr;

    QLineEdit   *m_searchBox = nullptr;
    QSlider     *m_volSlider = nullptr;
    QListWidget *m_playlist  = nullptr;
    QTimer      *m_infoTimer = nullptr;

    QMap<QString, QPushButton*> m_modeBtns;
    QMap<QString, QString>      m_favorites;

    QString     m_currentFolder;
    QStringList m_allItems;
    QList<int>  m_allIndices;

    bool m_showVU = true;
    bool m_hp1On  = false;
    bool m_hp2On  = false;
};
