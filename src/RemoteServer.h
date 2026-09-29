#pragma once
// ============================================================================
//  RemoteServer — Always Player for Android からのリモコン受信（Always Link）
//
//  ・通信 : TCP ポート 50505。1行 = 1つのJSON（UTF-8、改行区切り）。
//  ・検出 : UDP ポート 50506。スマホが "ALWAYS_DISCOVER" をブロードキャストすると
//           "ALWAYS_HERE <TCPポート> <PC名>" を返す（自宅Wi-Fi用）。
//           USB接続(adb reverse)やテザリングでは、スマホ側で 127.0.0.1 や
//           PCのIPを直接入力して接続する。
//  ・安全 : 家庭内LAN・ループバック(プライベートアドレス)からの接続のみ受け付ける。
//
//  スマホ → PC  {"cmd":"play"|"pause"|"toggle"|"stop"|"next"|"prev"}
//               {"cmd":"volume","value":0-100}
//               {"cmd":"seek","value":秒}
//               {"cmd":"browse","path":フォルダ}   → folders（""で「今のフォルダの親」から、"::drives"でドライブ一覧）
//               {"cmd":"folderArt","path":フォルダ} → folderArt（一覧のサムネイル）
//               {"cmd":"openFolder","path":フォルダ} そのフォルダを読み込んで再生
//  ── v10 で追加（proto=2）──────────────────────────────────────────────
//               {"cmd":"outputs"}                     → outputs（出力方式の一覧）
//               {"cmd":"setOutput","key":"wasapi"|"asio:<ドライバ名>"}
//               {"cmd":"bitperfect","rate":0}         自動（モード連動）
//               {"cmd":"bitperfect","rate":44100,"bits":24} 16種類の手動指定
//               {"cmd":"dop","on":true|false}         DoP出力（今の出力デバイスに記憶）
//                  ※ PC画面の警告ダイアログは出さない（確認はスマホ側で行う）
//               {"cmd":"chain","on":bool}             中密度チェーン
//               {"cmd":"soundField","value":""|"wowflutter"|"halltone"}
//               {"cmd":"dspOff","on":bool}            DSP完全バイパス
//               {"cmd":"coverOnline","on":bool}       ジャケット画像のネット取得
//               {"cmd":"artistInfo","artist":名前}     → artistInfo（""で今の曲の1人目）
//  PC → スマホ  {"type":"hello","app":"Always Player","version":"10.0.0","proto":2}
//               {"type":"status","state":"playing"|"paused"|"stopped",
//                "title":..,"artist":..,"index":n,"total":n,
//                "pos":秒,"dur":秒,"volume":0-100,"cd":bool,"artId":n}
//               {"type":"art","id":n,"mime":"image/jpeg","data":"<base64>"}
//               （artId が変わった時だけ art を送る。画像なしは id=0・data空）
//               {"type":"folders","path":..,"parent":..,"title":..,
//                "items":[{"name":..,"path":..,"tracks":曲数,"sub":サブフォルダ有無}]}
//               {"type":"folderArt","path":..,"data":"<base64 JPEG>"}（画像なしは空）
//               v10: status に以下を追加
//                "info":"FLAC | 2116 kbps | 96 kHz / 24bit"（PC画面の情報欄と同じ）
//                "output":"BitPerfect 96kHz/24"｜"Native DSD64"｜"共有モード 48kHz" 等
//                "backend":"wasapi"|"asio:<名前>", "bp":"auto"|"<rate>/<bits>",
//                "dop":bool（設定）, "dopActive":bool, "nativeDsd":bool（実際の出力）
//               {"type":"outputs","current":key,"items":[{"key":..,"label":..}]}
//               {"type":"artistInfo","artists":[..],"artist":..,"loading":bool,"ok":bool,
//                "name":..,"description":..,"body":本文(文字のみ),"url":..,"lang":..,
//                "imageSource":..,"image":"<base64 JPEG>"}
//               status に "chain","soundField","dspOff","coverOnline","hasArtist" も追加
// ============================================================================
#include <QObject>
#include <QJsonObject>
#include <QByteArray>
#include <QList>

class QTcpServer;
class QTcpSocket;
class QUdpSocket;

class RemoteServer : public QObject
{
    Q_OBJECT
public:
    static constexpr quint16 kTcpPort       = 50505;
    static constexpr quint16 kDiscoveryPort = 50506;
    static constexpr int     kProtocolVersion = 2;   // v9=1（proto無し）, v10=2

    explicit RemoteServer(QObject *parent = nullptr);
    ~RemoteServer() override;

    bool start();                 // 待ち受け開始（失敗時false：ポート使用中など）
    void stop();
    bool isRunning() const;
    int  clientCount() const { return m_clients.size(); }

    // 状態を全クライアントへ送る（前回と同じ内容なら送らない）
    void publishStatus(const QJsonObject &status);
    // 任意のメッセージを全クライアントへ送る（フォルダ一覧・サムネイルの返信用）
    void send(const QJsonObject &obj) { broadcast(obj); }
    // ジャケット画像（JPEG）を全クライアントへ送る。id は画像ごとに一意な値
    void publishArt(qint64 id, const QByteArray &jpeg);

signals:
    void commandReceived(const QString &cmd, const QJsonObject &obj);
    void clientConnected();       // 新しいスマホが接続した（最新の状態・画像を送り直す合図）

private:
    void onNewConnection();
    void onReadyRead(QTcpSocket *s);
    void onDiscovery();
    void sendLine(QTcpSocket *s, const QJsonObject &obj);
    void broadcast(const QJsonObject &obj);
    static bool isAllowedPeer(const class QHostAddress &addr);

    QTcpServer         *m_server = nullptr;
    QUdpSocket         *m_udp    = nullptr;
    QList<QTcpSocket*>  m_clients;
    QByteArray          m_lastStatus;   // 重複送信防止
    qint64              m_lastArtId = -1;
    QByteArray          m_lastArtLine;  // 新規接続時に送り直す
};
