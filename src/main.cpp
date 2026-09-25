#include <QApplication>
#include <windows.h>
#include <cwchar>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    // 多重起動防止
    HANDLE mutex = CreateMutexA(nullptr, TRUE, "AlwaysPlayerMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // ★ ウィンドウタイトルは「Always Player  v9.0.0  -  曲名」のように変わるため、
        //   完全一致のFindWindowでは見つからない。先頭が "Always Player" の
        //   ウィンドウを探して前面に出す。
        HWND hwnd = nullptr;
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            wchar_t buf[256] = {};
            GetWindowTextW(h, buf, 256);
            if (wcsncmp(buf, L"Always Player", 13) == 0) {
                *reinterpret_cast<HWND*>(lp) = h;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&hwnd));
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
    app.setApplicationVersion("9.0.0");

    MainWindow window;
    window.showMaximized();

    int ret = app.exec();
    CloseHandle(mutex);
    return ret;
}
