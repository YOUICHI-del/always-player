"""
Always Player - always_server.py
打倒 foobar2000 / パワフル・クリア・ジッターレス
"""

import os, sys, json, time, platform, subprocess, threading, glob, ctypes, socket, tempfile, math
from pathlib import Path
from flask import Flask, request, jsonify, send_from_directory
from flask_cors import CORS

app = Flask(__name__, static_folder=".")
CORS(app)

IS_WIN = platform.system() == "Windows"

# ═════════════════════════════════════════
# 電源・優先度（起動時1回）
# ═════════════════════════════════════════
def setup_power():
    if IS_WIN:
        try:
            # プロセス優先度 Realtime
            handle = ctypes.windll.kernel32.GetCurrentProcess()
            ctypes.windll.kernel32.SetPriorityClass(handle, 0x00000100)
            # 最大パフォーマンス電源プラン（高パフォーマンスより上）
            subprocess.run(["powercfg", "/setactive", "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c"],
                           capture_output=True)
            # CPUパークingを無効化（全コアを常時稼働）
            subprocess.run(["powercfg", "/setacvalueindex", "SCHEME_CURRENT",
                            "54533251-82be-4824-96c1-47b60b740d00",
                            "0cc5b647-c1df-4637-891a-dec35c318583", "100"],
                           capture_output=True)
            subprocess.run(["powercfg", "/setactive", "SCHEME_CURRENT"], capture_output=True)
            print("[Power] Realtime優先度 + 最大パフォーマンス + CPUパーキング無効")
        except Exception as e:
            print(f"[Power] {e}")
    else:
        try: os.nice(-20)
        except: pass

# ═════════════════════════════════════════
# mpvパス
# ═════════════════════════════════════════
def find_mpv():
    if IS_WIN:
        for c in [
            Path(__file__).parent / "mpv.exe",
            Path("C:/Program Files/MPV Player/mpv.exe"),
        ]:
            if c.exists(): return str(c)
        return "mpv"
    return "mpv"

MPV_PATH = find_mpv()

# ═════════════════════════════════════════
# nircmdパス
# ═════════════════════════════════════════
def find_nircmd():
    for c in [
        Path(__file__).parent / "nircmd.exe",
        Path("C:/Windows/System32/nircmd.exe"),
        Path("C:/tools/nircmd.exe"),
    ]:
        if c.exists(): return str(c)
    return "nircmd"

NIRCMD_PATH = find_nircmd()

# ═════════════════════════════════════════
# サンプルレート自動切替
# ═════════════════════════════════════════
_saved_samplerate = None

def get_current_samplerate():
    try:
        r = subprocess.run(
            ["powershell", "-Command",
             "$key='HKCU:\\Software\\Microsoft\\Multimedia\\Audio';"
             "$sr=(Get-ItemProperty $key -ErrorAction SilentlyContinue).DefaultSampleRate;"
             "if($sr){Write-Output $sr}else{Write-Output '48000'}"],
            capture_output=True, text=True, timeout=3)
        val = r.stdout.strip()
        if val.isdigit(): return int(val)
    except: pass
    return 48000

def set_samplerate(rate):
    if not IS_WIN: return False
    try:
        r = subprocess.run(
            [NIRCMD_PATH, "setdefaultsoundformat", "1", f"{rate}", "16"],
            capture_output=True, timeout=3)
        print(f"[SampleRate] → {rate}Hz")
        return r.returncode == 0
    except Exception as e:
        print(f"[SampleRate] 変更失敗: {e}")
        return False

def save_and_set_samplerate(target=44100):
    global _saved_samplerate
    if not IS_WIN: return
    _saved_samplerate = get_current_samplerate()
    if _saved_samplerate == target:
        _saved_samplerate = None
        print(f"[SampleRate] すでに {target}Hz — 変更不要")
        return
    print(f"[SampleRate] 保存: {_saved_samplerate}Hz → {target}Hz")
    set_samplerate(target)

def restore_samplerate():
    global _saved_samplerate
    if not IS_WIN: return
    if _saved_samplerate is not None:
        print(f"[SampleRate] 復元: → {_saved_samplerate}Hz")
        set_samplerate(_saved_samplerate)
        _saved_samplerate = None



# ═════════════════════════════════════════
# 出力デバイス一覧
# ═════════════════════════════════════════
def get_audio_devices():
    devices = [
        {"id": "auto",                 "name": "自動（システム既定）",      "exclusive": False},
        {"id": "wasapi/default",       "name": "WASAPI排他（高音質）",      "exclusive": True},
        {"id": "wasapi_shared/default","name": "WASAPI共有（Bluetooth対応）","exclusive": False},
    ]
    try:
        r = subprocess.run([MPV_PATH, "--audio-device=help"],
                           capture_output=True, text=True, timeout=5)
        for line in (r.stdout + r.stderr).split("\n"):
            line = line.strip()
            if line.startswith("wasapi/") and "default" not in line:
                parts = line.split(None, 1)
                if parts:
                    dev_id   = parts[0]
                    dev_name = parts[1].strip() if len(parts) > 1 else dev_id
                    devices.append({"id": dev_id, "name": dev_name,
                                    "exclusive": "shared" not in dev_id})
    except Exception as e:
        print(f"[Device] {e}")
    return devices

# ═════════════════════════════════════════
# フォーマット
# ═════════════════════════════════════════
SUPPORTED_EXT = {".mp3",".aac",".ogg",".wav",".flac",".opus",".dsf",".dff",".m4a",".aiff",".wv"}
UNSUPPORTED_MSG = {
    ".wma": "WMA はライセンス制限により再生できません",
    ".mqa": "MQA はライセンス技術のため再生できません",
    ".dts": "DTS はライセンス制限により再生できません",
    ".ac3": "AC3 はライセンス制限により再生できません",
    ".mp4": "MP4 動画ファイルは再生できません",
}

# ═════════════════════════════════════════
# マシンスペック自動判定（起動時1回）
# ═════════════════════════════════════════
def auto_precision():
    """CPUベンチマークでprecisionを自動決定"""
    import multiprocessing
    cores = multiprocessing.cpu_count()
    end = time.perf_counter() + 0.1
    count = 0
    while time.perf_counter() < end:
        math.sin(count * 0.001)
        count += 1
    if count >= 800000 or cores >= 8:
        p = 32   # ハイスペック → 最高精度
    elif count >= 400000 or cores >= 4:
        p = 28   # ミドル → 標準精度
    else:
        p = 24   # ローエンド → 軽量精度
    print(f"[CPU] コア数={cores} / ベンチ={count:,} → precision={p} に自動設定")
    return p

PRECISION = auto_precision()

# ═════════════════════════════════════════
# 音質モード
# パワフル路線 / Minimum Phase寄り / エッジ重視
# ═════════════════════════════════════════
MODES = {
    "pure": {
        "label": "ピュア",
        "desc":  "無加工・原音忠実 / 最短信号経路",
        "upsample": 0,
        "af": []
    },
    "hires4": {
        "label": "ハイレゾ ×4",
        "desc":  "4倍アップサンプリング / Minimum Phase / パワフル",
        "upsample": "x4",
        "af": ["lavfi=[aresample=filter_type=kaiser:cutoff=0.9702:precision=28"
               ":dither_method=triangular_hp:min_comp=0.001:min_hard_comp=0.1]"]
    },
    "dsd8": {
        "label": "疑似DSD ×8",
        "desc":  f"8倍アップサンプリング / 高純度ディザー / precision={PRECISION}",
        "upsample": "x8",
        "af": [f"lavfi=[aresample=filter_type=sinc:cutoff=0.9998:precision={PRECISION}"
               f":dither_method=triangular_hp]"]
    },
    "loudness": {
        "label": "ラウドネス",
        "desc":  "音圧最適化 + True Peak制御",
        "upsample": "x4",
        "af": ["lavfi=[loudnorm=I=-14:TP=-1:LRA=11]",
               "lavfi=[aresample=filter_type=kaiser:cutoff=0.9702:precision=28"
               ":dither_method=triangular_hp:min_comp=0.001:min_hard_comp=0.1]"]
    },
    "hp_soft": {
        "label": "HP 弱",
        "desc":  "クロスフィード（自然な広がり）",
        "upsample": "x4",
        "af": ["bs2b=cmoy",
               "lavfi=[aresample=filter_type=kaiser:cutoff=0.9702:precision=28"
               ":dither_method=triangular_hp:min_comp=0.001:min_hard_comp=0.1]"]
    },
    "hp_strong": {
        "label": "HP 強",
        "desc":  "クロスフィード強 + 前方定位",
        "upsample": "x4",
        "af": ["bs2b=jmeier",
               "lavfi=[aresample=filter_type=kaiser:cutoff=0.9702:precision=28"
               ":dither_method=triangular_hp:min_comp=0.001:min_hard_comp=0.1]"]
    },
}

def get_upsample_rate(filepath, mult):
    try:
        r = subprocess.run(
            [MPV_PATH,"--no-video","--frames=0","--of=null","--o=-",
             "--really-quiet","--msg-level=all=no,audio=v", filepath],
            capture_output=True, text=True, timeout=3)
        for line in r.stderr.split("\n"):
            if "Hz" in line:
                for part in line.split():
                    if part.endswith("Hz"):
                        sr = int(part.replace("Hz","").replace(",",""))
                        return sr * mult
    except: pass
    return 44100 * mult

# ═════════════════════════════════════════
# IPC通信
# ═════════════════════════════════════════
class MpvIPC:
    def __init__(self, pipe_path):
        self.pipe_path = pipe_path
        self._lock = threading.Lock()
        self._sock = None
        self._req_id = 1

    def connect(self):
        for _ in range(20):
            try:
                if IS_WIN:
                    import win32file
                    self._sock = win32file.CreateFile(
                        self.pipe_path,
                        win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                        0, None, win32file.OPEN_EXISTING, 0, None)
                    return True
                else:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(self.pipe_path)
                    self._sock = s
                    return True
            except: time.sleep(0.15)
        return False

    def _send_cmd(self, cmd_list):
        try:
            with self._lock:
                rid = self._req_id; self._req_id += 1
                msg = json.dumps({"command": cmd_list, "request_id": rid}) + "\n"
                if IS_WIN:
                    import win32file
                    win32file.WriteFile(self._sock, msg.encode())
                    _, data = win32file.ReadFile(self._sock, 65536)
                    return json.loads(data.decode().strip().split("\n")[0])
                else:
                    self._sock.sendall(msg.encode())
                    buf = b""
                    while b"\n" not in buf:
                        chunk = self._sock.recv(4096)
                        if not chunk: break
                        buf += chunk
                    return json.loads(buf.split(b"\n")[0])
        except: return {}

    def get_property(self, prop):
        return self._send_cmd(["get_property", prop]).get("data")

    def set_property(self, prop, val):
        return self._send_cmd(["set_property", prop, val])

    def command(self, *args):
        return self._send_cmd(list(args))

    def close(self):
        try:
            if self._sock:
                if IS_WIN:
                    import win32file; win32file.CloseHandle(self._sock)
                else: self._sock.close()
        except: pass
        self._sock = None

# ═════════════════════════════════════════
# プレイヤー
# ═════════════════════════════════════════
class AlwaysPlayer:
    def __init__(self):
        self.process      = None
        self.ipc          = None
        self.ipc_path     = ""
        self.playlist     = []
        self.current_index= 0
        self.is_playing   = False
        self.paused       = False
        self.volume       = 100
        self.mode         = "dsd8"          # デフォルトを疑似DSD×8に
        self.audio_device = "auto"          # WASAPIなし・シンプルに
        self.audio_exclusive = False
        self.lock         = threading.Lock()
        self._buffer      = {}

    def _preload(self, fp):
        if fp not in self._buffer:
            try:
                with open(fp,"rb") as f: self._buffer[fp] = f.read()
                print(f"[Preload] {Path(fp).name} ({len(self._buffer[fp])//1024}KB)")
            except Exception as e: print(f"[Preload error] {e}")

    def _ipc_path_new(self):
        if IS_WIN:
            return r"\\.\pipe\always_mpv_" + str(os.getpid())
        return os.path.join(tempfile.gettempdir(), f"always_mpv_{os.getpid()}.sock")

    def _build_args(self, filepath):
        mode = MODES.get(self.mode, MODES["pure"])
        self.ipc_path = self._ipc_path_new()
        args = [
            MPV_PATH, "--no-video", "--really-quiet",
            f"--volume={self.volume}",
            "--cache=no",           # キャッシュ無効（メモリ展開済みのため不要）
            "--audio-buffer=0.05",  # バッファ最小
            f"--input-ipc-server={self.ipc_path}",
            f"--audio-device={self.audio_device}",
        ]
        mult = mode.get("upsample", 0)
        if mult == "x4": args.append(f"--audio-samplerate={get_upsample_rate(filepath,4)}")
        elif mult == "x8": args.append(f"--audio-samplerate={get_upsample_rate(filepath,8)}")

        af = mode.get("af", [])
        if af: args.append(f"--af={','.join(af)}")
        args.append(filepath)
        return args

    def play(self, index=None):
        with self.lock:
            if index is not None: self.current_index = index
            if not self.playlist: return {"error": "プレイリストが空です"}
            fp  = self.playlist[self.current_index]
            ext = Path(fp).suffix.lower()
            if ext not in SUPPORTED_EXT:
                return {"error": UNSUPPORTED_MSG.get(ext, f"{ext} は再生できません")}
            self._preload(fp)
            self._stop_process()
            save_and_set_samplerate()  # サンプルレート44.1kHz固定
            args = self._build_args(fp)
            try:
                self.process = subprocess.Popen(
                    args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.is_playing = True
                self.paused = False
                self.ipc = MpvIPC(self.ipc_path)
                threading.Thread(target=lambda: self.ipc.connect(), daemon=True).start()
                threading.Thread(target=self._watch, daemon=True).start()
                return {"ok": True, "index": self.current_index, "file": Path(fp).name,
                        "mode": self.mode, "mode_label": MODES[self.mode]["label"]}
            except Exception as e: return {"error": str(e)}

    def _watch(self):
        if self.process:
            self.process.wait()
            with self.lock:
                if self.is_playing and not self.paused:
                    self.is_playing = False
                    if self.current_index + 1 < len(self.playlist):
                        self.current_index += 1
                        self.lock.release()
                        self._preload(self.playlist[self.current_index])
                        self.play()
                        self.lock.acquire()

    def get_position(self):
        if self.ipc and self.is_playing:
            pos = self.ipc.get_property("time-pos")
            dur = self.ipc.get_property("duration")
            return {"position": round(pos,2) if pos else 0,
                    "duration": round(dur,2) if dur else 0}
        return {"position": 0, "duration": 0}

    def seek(self, seconds):
        if self.ipc: self.ipc.command("seek", seconds, "absolute")
        return {"ok": True}

    def pause(self):
        with self.lock:
            if self.ipc and self.is_playing:
                self.ipc.set_property("pause", True)
                self.paused = True; self.is_playing = False
        return {"ok": True}

    def resume(self):
        if self.paused and self.ipc:
            with self.lock:
                self.ipc.set_property("pause", False)
                self.is_playing = True; self.paused = False
            return {"ok": True}
        return self.play()

    def stop(self):
        with self.lock:
            self._stop_process()
            self.is_playing = False; self.paused = False
        restore_samplerate()  # サンプルレート復元
        return {"ok": True}

    def next_track(self):
        with self.lock:
            if self.current_index + 1 < len(self.playlist): self.current_index += 1
        return self.play()

    def prev_track(self):
        with self.lock:
            if self.current_index > 0: self.current_index -= 1
        return self.play()

    def set_volume(self, vol):
        self.volume = max(0, min(100, int(vol)))
        if self.ipc: self.ipc.set_property("volume", self.volume)
        return {"ok": True, "volume": self.volume}

    def set_mode(self, mode):
        if mode not in MODES: return {"error": f"不明なモード: {mode}"}
        self.mode = mode
        was_playing = self.is_playing; idx = self.current_index
        if was_playing: self.stop(); self.play(idx)
        return {"ok": True, "mode": mode, "mode_label": MODES[mode]["label"]}

    def set_device(self, device_id, exclusive):
        self.audio_device    = device_id
        self.audio_exclusive = exclusive
        was_playing = self.is_playing; idx = self.current_index
        if was_playing: self.stop(); self.play(idx)
        return {"ok": True, "device": device_id, "exclusive": exclusive}

    def load_folder(self, folder_path):
        files = []
        for ext in list(SUPPORTED_EXT) + list(UNSUPPORTED_MSG.keys()):
            files += glob.glob(os.path.join(folder_path,f"**/*{ext}"),      recursive=True)
            files += glob.glob(os.path.join(folder_path,f"**/*{ext.upper()}"), recursive=True)
        files = sorted(set(files))
        supported   = [f for f in files if Path(f).suffix.lower() in SUPPORTED_EXT]
        unsupported = [f"{Path(f).name}（{UNSUPPORTED_MSG.get(Path(f).suffix.lower(),'再生できません')}）"
                       for f in files if Path(f).suffix.lower() not in SUPPORTED_EXT]
        with self.lock:
            self.playlist = supported; self.current_index = 0
            self.is_playing = False; self._buffer = {}; self._stop_process()
        for f in supported[:2]:
            threading.Thread(target=self._preload, args=(f,), daemon=True).start()
        return {"ok": True, "count": len(supported), "unsupported": unsupported,
                "playlist": [Path(f).name for f in supported]}

    def status(self):
        return {
            "is_playing":     self.is_playing,
            "paused":         self.paused,
            "current_index":  self.current_index,
            "current_file":   Path(self.playlist[self.current_index]).name if self.playlist else None,
            "total":          len(self.playlist),
            "volume":         self.volume,
            "mode":           self.mode,
            "mode_label":     MODES.get(self.mode,{}).get("label",""),
            "mode_desc":      MODES.get(self.mode,{}).get("desc",""),
            "audio_device":   self.audio_device,
            "audio_exclusive":self.audio_exclusive,
            "audio_label":    self._device_label(),
            "platform":       platform.system(),
            "modes":          {k:{"label":v["label"],"desc":v["desc"]} for k,v in MODES.items()},
        }

    def _device_label(self):
        if self.audio_exclusive:              return "WASAPI排他"
        if "bluetooth" in self.audio_device.lower(): return "Bluetooth"
        if "shared"    in self.audio_device.lower(): return "WASAPI共有"
        return "共有モード"

    def _stop_process(self):
        if self.ipc: self.ipc.close(); self.ipc = None
        if self.process:
            try: self.process.terminate(); self.process.wait(timeout=2)
            except:
                try: self.process.kill()
                except: pass
            self.process = None


player = AlwaysPlayer()

# ═════════════════════════════════════════
# API
# ═════════════════════════════════════════
@app.route("/")
def index(): return send_from_directory(".", "always.html")

@app.route("/manifest.json")
def manifest(): return send_from_directory(".", "manifest.json")

@app.route("/sw.js")
def sw(): return send_from_directory(".", "sw.js")

@app.route("/Always.png")
def icon_png(): return send_from_directory(".", "Always.png")

@app.route("/api/status")
def api_status(): return jsonify(player.status())

@app.route("/api/position")
def api_position(): return jsonify(player.get_position())

@app.route("/api/devices")
def api_devices(): return jsonify(get_audio_devices())

@app.route("/api/play",    methods=["POST"])
def api_play():   return jsonify(player.play((request.json or {}).get("index")))

@app.route("/api/pause",   methods=["POST"])
def api_pause():  return jsonify(player.pause())

@app.route("/api/resume",  methods=["POST"])
def api_resume(): return jsonify(player.resume())

@app.route("/api/stop",    methods=["POST"])
def api_stop():   return jsonify(player.stop())

@app.route("/api/next",    methods=["POST"])
def api_next():   return jsonify(player.next_track())

@app.route("/api/prev",    methods=["POST"])
def api_prev():   return jsonify(player.prev_track())

@app.route("/api/seek",    methods=["POST"])
def api_seek():   return jsonify(player.seek((request.json or {}).get("seconds", 0)))

@app.route("/api/volume",  methods=["POST"])
def api_volume(): return jsonify(player.set_volume((request.json or {}).get("volume", 100)))

@app.route("/api/mode",    methods=["POST"])
def api_mode():   return jsonify(player.set_mode((request.json or {}).get("mode","dsd8")))

@app.route("/api/device",  methods=["POST"])
def api_device():
    data = request.json or {}
    return jsonify(player.set_device(data.get("device","auto"), data.get("exclusive", False)))

@app.route("/api/load_folder", methods=["POST"])
def api_load_folder():
    data = request.json or {}
    folder = data.get("path","")
    if not os.path.isdir(folder):
        return jsonify({"error": f"フォルダが見つかりません: {folder}"})
    return jsonify(player.load_folder(folder))

@app.route("/api/modes")
def api_modes():
    return jsonify({k:{"label":v["label"],"desc":v["desc"]} for k,v in MODES.items()})

# ═════════════════════════════════════════
# ジャケット取得API
# mutagen でID3/FLAC/M4Aタグから埋め込み画像を抽出
# ═════════════════════════════════════════
@app.route("/api/jacket", methods=["GET"])
def api_jacket():
    """現在再生中の曲からジャケット画像をbase64で返す"""
    import base64
    if not player.playlist:
        return jsonify({"ok": False})
    fp = player.playlist[player.current_index]
    ext = Path(fp).suffix.lower()
    try:
        from mutagen.id3 import ID3, APIC
        from mutagen.mp3 import MP3
        from mutagen.flac import FLAC
        from mutagen.mp4 import MP4
        from mutagen.oggvorbis import OggVorbis
        import base64 as b64

        img_data = None
        mime = "image/jpeg"

        if ext == ".mp3":
            try:
                tags = ID3(fp)
                for tag in tags.values():
                    if isinstance(tag, APIC):
                        img_data = tag.data
                        mime = tag.mime or "image/jpeg"
                        break
            except: pass

        elif ext == ".flac":
            try:
                audio = FLAC(fp)
                if audio.pictures:
                    pic = audio.pictures[0]
                    img_data = pic.data
                    mime = pic.mime or "image/jpeg"
            except: pass

        elif ext in (".m4a", ".aac"):
            try:
                audio = MP4(fp)
                covr = audio.tags.get("covr")
                if covr:
                    img_data = bytes(covr[0])
                    mime = "image/jpeg"
            except: pass

        elif ext == ".ogg":
            try:
                audio = OggVorbis(fp)
                pics = audio.get("metadata_block_picture", [])
                if pics:
                    import base64 as b64m, struct
                    raw = b64m.b64decode(pics[0])
                    # FLAC Picture block パース
                    offset = 4  # type
                    mlen = struct.unpack(">I", raw[offset:offset+4])[0]; offset += 4
                    mime = raw[offset:offset+mlen].decode(); offset += mlen
                    dlen = struct.unpack(">I", raw[offset:offset+4])[0]; offset += 4
                    offset += dlen  # description
                    offset += 16   # width, height, depth, colors
                    dlen2 = struct.unpack(">I", raw[offset:offset+4])[0]; offset += 4
                    img_data = raw[offset:offset+dlen2]
            except: pass

        if img_data:
            encoded = b64.b64encode(img_data).decode()
            return jsonify({
                "ok": True,
                "data": f"data:{mime};base64,{encoded}"
            })
        return jsonify({"ok": False})

    except Exception as e:
        print(f"[Jacket] {e}")
        return jsonify({"ok": False})

# ═════════════════════════════════════════
# お気に入り保存・復元（ini）
# クラッシュ対策：変更のたびに即時保存 + .bakで二重保持
# ═════════════════════════════════════════
import json as _json

def _ini_path():
    """AlwaysPlayer.ini の保存場所（always_server.pyと同じフォルダ）"""
    return Path(__file__).parent / "AlwaysPlayer.ini"

def _ini_bak_path():
    return Path(__file__).parent / "AlwaysPlayer.ini.bak"

def load_favorites():
    """ini読み込み。壊れていたら.bakから復元"""
    for path in [_ini_path(), _ini_bak_path()]:
        try:
            if path.exists():
                data = _json.loads(path.read_text(encoding="utf-8"))
                if isinstance(data, dict):
                    print(f"[INI] 読み込み完了: {path.name}")
                    return data
        except Exception as e:
            print(f"[INI] 読み込み失敗 ({path.name}): {e}")
    return {"favorites": []}

def save_favorites(data):
    """即時保存 + .bakに二重保持"""
    try:
        ini = _ini_path()
        bak = _ini_bak_path()
        text = _json.dumps(data, ensure_ascii=False, indent=2)
        # ① まず.bakに現在のiniをコピー
        if ini.exists():
            bak.write_text(ini.read_text(encoding="utf-8"), encoding="utf-8")
        # ② 新しい内容を書き込む
        ini.write_text(text, encoding="utf-8")
        print(f"[INI] 保存完了 ({len(data.get('favorites',[]))}件)")
        return True
    except Exception as e:
        print(f"[INI] 保存失敗: {e}")
        return False

# 起動時に読み込んでメモリに保持
_favorites_data = load_favorites()

@app.route("/api/favorites", methods=["GET"])
def api_favorites_get():
    """お気に入り一覧を返す"""
    return jsonify(_favorites_data)

@app.route("/api/favorites", methods=["POST"])
def api_favorites_save():
    """お気に入りを保存（変更のたびに呼ぶ）"""
    global _favorites_data
    data = request.json or {}
    if "favorites" not in data:
        return jsonify({"error": "favorites キーがありません"})
    _favorites_data = data
    ok = save_favorites(_favorites_data)
    return jsonify({"ok": ok})

# ═════════════════════════════════════════
# ファイルアップロード（ブラウザ・スマホ用）
# ═════════════════════════════════════════
import tempfile as _tempfile
UPLOAD_DIR = os.path.join(_tempfile.gettempdir(), "always_uploads")

@app.route("/api/upload_files", methods=["POST"])
def api_upload_files():
    os.makedirs(UPLOAD_DIR, exist_ok=True)
    files = request.files.getlist("files")
    if not files: return jsonify({"error": "ファイルがありません"})
    saved=[]; unsupported=[]
    for f in files:
        ext = Path(f.filename).suffix.lower()
        if ext in SUPPORTED_EXT:
            dest = os.path.join(UPLOAD_DIR, f.filename.replace("/","_").replace("\\","_"))
            f.save(dest); saved.append(dest)
        else:
            unsupported.append(f.filename)
    saved = sorted(saved)
    with player.lock:
        player.playlist=saved; player.current_index=0
        player.is_playing=False; player._buffer={}; player._stop_process()
    for fp in saved[:2]:
        threading.Thread(target=player._preload, args=(fp,), daemon=True).start()
    return jsonify({"ok":True,"count":len(saved),"unsupported":unsupported,
                    "playlist":[Path(f).name for f in saved]})

# ═════════════════════════════════════════
# PyQt6 QFileDialog連携
# ═════════════════════════════════════════
@app.route("/api/open_folder", methods=["POST"])
def api_open_folder():
    try:
        from always_app import request_folder_dialog
        path = request_folder_dialog()
        if path:
            result = player.load_folder(path)
            result["folder_path"] = path  # HTML側でお気に入り登録に使う
            return jsonify(result)
        return jsonify({"error": "キャンセルされました"})
    except Exception as e:
        return jsonify({"error": str(e)})

# ═════════════════════════════════════════
# 起動
# ═════════════════════════════════════════
if __name__ == "__main__":
    setup_power()
    import atexit
    atexit.register(restore_samplerate)
    print("=" * 52)
    print("  Always Player — 打倒 foobar2000")
    print(f"  mpv   : {MPV_PATH}")
    print(f"  nircmd: {NIRCMD_PATH}")
    print("  → http://127.0.0.1:8765 をブラウザで開く")
    print("=" * 52)
    app.run(host="0.0.0.0", port=8765, debug=False, threaded=True)
