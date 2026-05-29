#include <QApplication>
#include <windows.h>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    // 多重起動防止
    HANDLE mutex = CreateMutexA(nullptr, TRUE, "AlwaysPlayerMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hwnd = FindWindowA(nullptr, "Always Player");
        if (hwnd) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        CloseHandle(mutex);
        return 0;
    }

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName("Always Player");
    app.setApplicationVersion("4.0.0");

    MainWindow window;
    window.showMaximized();

    int ret = app.exec();
    CloseHandle(mutex);
    return ret;
}
