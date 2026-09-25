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
//  PC → スマホ  {"type":"hello","app":"Always Player","version":"9.0.0"}
//               {"type":"status","state":"playing"|"paused"|"stopped",
//                "title":..,"artist":..,"index":n,"total":n,
//                "pos":秒,"dur":秒,"volume":0-100,"cd":bool,"artId":n}
//               {"type":"art","id":n,"mime":"image/jpeg","data":"<base64>"}
//               （artId が変わった時だけ art を送る。画像なしは id=0・data空）
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

    explicit RemoteServer(QObject *parent = nullptr);
    ~RemoteServer() override;

    bool start();                 // 待ち受け開始（失敗時false：ポート使用中など）
    void stop();
    bool isRunning() const;
    int  clientCount() const { return m_clients.size(); }

    // 状態を全クライアントへ送る（前回と同じ内容なら送らない）
    void publishStatus(const QJsonObject &status);
    // ジャケット画像（JPEG）を全クライアントへ送る。id は画像ごとに一意な値
    void publishArt(qint64 id, const QByteArray &jpeg);

signals:
    void commandReceived(const QString &cmd, double value);
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
