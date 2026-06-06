"""
Always Player - always_app.py
PyQt6 + WebEngineView + タスクトレイ + Flaskサーバー内蔵
"""

import sys, os, threading, socket, webbrowser
from pathlib import Path

from PyQt6.QtWidgets import QApplication, QMainWindow, QSystemTrayIcon, QMenu
from PyQt6.QtWebEngineWidgets import QWebEngineView
from PyQt6.QtWebEngineCore import QWebEngineSettings
from PyQt6.QtCore import QUrl, Qt, QThread, pyqtSignal
from PyQt6.QtGui import QIcon, QAction

# ── Flaskサーバーをスレッドで起動
class ServerThread(QThread):
    started_signal = pyqtSignal()

    def run(self):
        from always_server import app, setup_power, MPV_PATH, _ini_path, _ini_bak_path
        import platform
        setup_power()
        # ini状態を確認
        ini_status = "正常"
        if not _ini_path().exists() and _ini_bak_path().exists():
            ini_status = "⚠️ バックアップから復元"
        elif not _ini_path().exists():
            ini_status = "新規作成"
        print("=" * 52)
        print("  Always Player")
        print(f"  mpv  : {MPV_PATH}")
        print(f"  OS   : {platform.system()}")
        print(f"  INI  : {ini_status}")
        print(f"  SP   : http://{get_local_ip()}:8765")
        print("=" * 52)
        self.started_signal.emit()
        app.run(host="0.0.0.0", port=8765, debug=False, threaded=True, use_reloader=False)

def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except:
        return "127.0.0.1"

# ── メインウィンドウ
class AlwaysWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Always Player")
        self.setFixedSize(440, 720)

        base = Path(__file__).parent
        icon_path = str(base / "Always.ico")
        self.setWindowIcon(QIcon(icon_path))

        # WebEngineView
        self.view = QWebEngineView()
        settings = self.view.settings()
        settings.setAttribute(QWebEngineSettings.WebAttribute.JavascriptEnabled, True)
        settings.setAttribute(QWebEngineSettings.WebAttribute.LocalStorageEnabled, True)
        self.setCentralWidget(self.view)

        # タスクトレイ
        self.tray = QSystemTrayIcon(QIcon(icon_path), self)
        self.tray.setToolTip("Always Player")
        tray_menu = QMenu()

        act_open   = QAction("開く", self)
        act_open.triggered.connect(self.show_window)
        act_quit   = QAction("終了", self)
        act_quit.triggered.connect(self.quit_app)

        tray_menu.addAction(act_open)
        tray_menu.addSeparator()
        tray_menu.addAction(act_quit)
        self.tray.setContextMenu(tray_menu)
        self.tray.activated.connect(self._tray_activated)
        self.tray.show()

    def load_url(self):
        self.view.setUrl(QUrl("http://127.0.0.1:8765"))

    def closeEvent(self, event):
        # ×ボタンでトレイに格納（終了しない）
        event.ignore()
        self.hide()
        self.tray.showMessage(
            "Always Player",
            "バックグラウンドで再生中です。トレイアイコンから操作できます。",
            QSystemTrayIcon.MessageIcon.Information, 2000
        )

    def show_window(self):
        self.show()
        self.raise_()
        self.activateWindow()

    def _tray_activated(self, reason):
        if reason == QSystemTrayIcon.ActivationReason.DoubleClick:
            self.show_window()

    def quit_app(self):
        from always_server import player
        player.stop()
        QApplication.quit()


# ── 起動
def main():
    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)  # トレイ残留のため

    window = AlwaysWindow()

    # Flaskサーバー起動
    server = ServerThread()
    server.started_signal.connect(window.load_url)
    server.start()

    # サーバー起動を少し待ってからURL読み込み
    import time
    def delayed_load():
        time.sleep(1.5)
        window.load_url()
    threading.Thread(target=delayed_load, daemon=True).start()

    window.show()
    sys.exit(app.exec())

if __name__ == "__main__":
    main()

# ── QFileDialog連携（server.pyから呼ばれる）
def request_folder_dialog():
    from PyQt6.QtWidgets import QFileDialog
    from PyQt6.QtCore import QMetaObject, Qt
    result = [None]
    def _open():
        path = QFileDialog.getExistingDirectory(None, "音楽フォルダを選択", "",
            QFileDialog.Option.ShowDirsOnly)
        result[0] = path
    # UIスレッドで実行
    import threading
    done = threading.Event()
    def _run():
        _open()
        done.set()
    from PyQt6.QtCore import QTimer
    QTimer.singleShot(0, _run)
    done.wait(timeout=30)
    return result[0]
