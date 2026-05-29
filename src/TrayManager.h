#pragma once
#include <QObject>
#include <QSystemTrayIcon>

class QMenu;

class TrayManager : public QObject
{
    Q_OBJECT

public:
    explicit TrayManager(QObject *parent = nullptr);

    void showMessage(const QString &title, const QString &msg);
    void setToolTip(const QString &tip);
    void setPlaying(bool playing);

signals:
    void showRequested();
    void folderRequested();
    void quitRequested();

private:
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;
};
