#include <QApplication>
#include <windows.h>
#include <cwchar>
#include "MainWindow.h"
#include "IniFileLock.h"
#include <QTranslator>
#include <QLocale>

int main(int argc, char *argv[])
{
    // 多重起動防止
    HANDLE mutex = CreateMutexA(nullptr, TRUE, "AlwaysPlayerMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // ★ ウィンドウタイトルは「Always Player  v10.0.0  -  曲名」のように変わるため、
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
    app.setApplicationVersion("10.0.0");

    // ★ v10: 表示言語。ini の [ui] lang= が "ja" なら日本語、"en" なら英語。
    //   未設定・"auto" のときは Windows の表示言語が日本語なら日本語、それ以外は英語。
    //   英語の翻訳(always_en.qm)はexeのリソースに埋め込まれている。
    //   将来、ほかの言語の .qm を exe の横の translations フォルダに置けば読み込める。
    static QTranslator translator;
    {
        QString lang = iniReadValue(QStringLiteral("ui"), QStringLiteral("lang"));
        if (lang.isEmpty() || lang == QLatin1String("auto"))
            lang = (QLocale::system().language() == QLocale::Japanese) ? QStringLiteral("ja")
                                                                       : QStringLiteral("en");
        if (lang != QLatin1String("ja")) {
            const QString ext = QCoreApplication::applicationDirPath() + "/translations";
            if (translator.load("always_" + lang, ext) ||
                translator.load(":/i18n/always_" + lang + ".qm") ||
                translator.load(":/i18n/always_en.qm"))
                app.installTranslator(&translator);
        }
    }

    MainWindow window;
    window.showMaximized();

    int ret = app.exec();
    CloseHandle(mutex);
    return ret;
}
