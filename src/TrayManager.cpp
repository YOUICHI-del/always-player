#include "TrayManager.h"
#include <QMenu>
#include <QAction>
#include <QIcon>

TrayManager::TrayManager(QObject *parent) : QObject(parent)
{
    m_tray = new QSystemTrayIcon(QIcon(":/icons/Always.ico"), this);
    m_tray->setToolTip("Always Player");

    m_menu = new QMenu();

    QAction *actShow = new QAction("Always を開く", m_menu);
    connect(actShow, &QAction::triggered, this, &TrayManager::showRequested);

    QAction *actFolder = new QAction("フォルダを選択して再生", m_menu);
    connect(actFolder, &QAction::triggered, this, &TrayManager::folderRequested);

    QAction *actQuit = new QAction("終了", m_menu);
    connect(actQuit, &QAction::triggered, this, &TrayManager::quitRequested);

    m_menu->addAction(actShow);
    m_menu->addAction(actFolder);
    m_menu->addSeparator();
    m_menu->addAction(actQuit);

    m_tray->setContextMenu(m_menu);

    connect(m_tray, &QSystemTrayIcon::activated,
        [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::DoubleClick)
                emit showRequested();
        });

    m_tray->show();
}

void TrayManager::showMessage(const QString &title, const QString &msg)
{
    m_tray->showMessage(title, msg,
        QSystemTrayIcon::Information, 2000);
}

void TrayManager::setToolTip(const QString &tip)
{
    m_tray->setToolTip(tip);
}

void TrayManager::setPlaying(bool playing)
{
    Q_UNUSED(playing)
}
