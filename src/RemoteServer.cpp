#include "RemoteServer.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QDebug>

RemoteServer::RemoteServer(QObject *parent) : QObject(parent) {}

RemoteServer::~RemoteServer() { stop(); }

bool RemoteServer::start()
{
    if (isRunning()) return true;

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &RemoteServer::onNewConnection);
    if (!m_server->listen(QHostAddress::Any, kTcpPort)) {
        qDebug() << "[Remote] listen failed:" << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }

    // 自動検出(UDP)は失敗してもリモコン自体は使える(IP直接入力)ので致命的にしない
    m_udp = new QUdpSocket(this);
    if (m_udp->bind(QHostAddress::AnyIPv4, kDiscoveryPort, QUdpSocket::ShareAddress)) {
        connect(m_udp, &QUdpSocket::readyRead, this, &RemoteServer::onDiscovery);
    } else {
        qDebug() << "[Remote] discovery bind failed:" << m_udp->errorString();
    }
    qDebug() << "[Remote] listening on" << kTcpPort;
    return true;
}

void RemoteServer::stop()
{
    for (QTcpSocket *s : m_clients) {
        s->disconnect(this);
        s->abort();
        s->deleteLater();
    }
    m_clients.clear();
    if (m_server) { m_server->close(); m_server->deleteLater(); m_server = nullptr; }
    if (m_udp)    { m_udp->close();    m_udp->deleteLater();    m_udp = nullptr; }
    m_lastStatus.clear();
}

bool RemoteServer::isRunning() const
{
    return m_server && m_server->isListening();
}

// 家庭内LAN・ループバックのみ許可（インターネット側からの操作を防ぐ）
bool RemoteServer::isAllowedPeer(const QHostAddress &addrIn)
{
    QHostAddress addr = addrIn;
    bool ok = false;
    quint32 v4 = addr.toIPv4Address(&ok);   // ::ffff:192.168.x.x 形式も IPv4 として扱う
    if (ok) {
        if ((v4 >> 24) == 127) return true;                       // 127.0.0.0/8 (adb reverse)
        if ((v4 >> 24) == 10) return true;                        // 10.0.0.0/8
        if ((v4 >> 20) == (172u << 4 | 1)) return true;           // 172.16.0.0/12
        if ((v4 >> 16) == (192u << 8 | 168)) return true;         // 192.168.0.0/16
        if ((v4 >> 16) == (169u << 8 | 254)) return true;         // 169.254.0.0/16
        return false;
    }
    if (addr.isLoopback()) return true;
    const Q_IPV6ADDR a6 = addr.toIPv6Address();
    if (a6[0] == 0xfe && (a6[1] & 0xc0) == 0x80) return true;     // fe80::/10 リンクローカル
    if ((a6[0] & 0xfe) == 0xfc) return true;                      // fc00::/7 ユニークローカル
    return false;
}

void RemoteServer::onNewConnection()
{
    while (QTcpSocket *s = m_server->nextPendingConnection()) {
        if (!isAllowedPeer(s->peerAddress())) {
            qDebug() << "[Remote] rejected:" << s->peerAddress().toString();
            s->abort();
            s->deleteLater();
            continue;
        }
        m_clients.append(s);
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadyRead(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] {
            m_clients.removeAll(s);
            s->deleteLater();
        });

        sendLine(s, QJsonObject{
            {"type", "hello"},
            {"app", "Always Player"},
            {"version", "9.0.0"},
        });
        // 最新の状態と画像をこの端末へ送り直す
        if (!m_lastStatus.isEmpty()) s->write(m_lastStatus);
        if (!m_lastArtLine.isEmpty()) s->write(m_lastArtLine);
        emit clientConnected();
    }
}

void RemoteServer::onReadyRead(QTcpSocket *s)
{
    // 1行 = 1コマンド。異常に長い行(64KB超)は切断して身を守る
    while (s->canReadLine()) {
        const QByteArray line = s->readLine().trimmed();
        if (line.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) continue;
        const QJsonObject o = doc.object();
        const QString cmd = o.value("cmd").toString();
        if (cmd.isEmpty()) continue;
        emit commandReceived(cmd, o.value("value").toDouble());
    }
    if (s->bytesAvailable() > 64 * 1024) s->abort();
}

void RemoteServer::onDiscovery()
{
    while (m_udp && m_udp->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_udp->receiveDatagram(512);
        if (!isAllowedPeer(dg.senderAddress())) continue;
        if (!dg.data().startsWith("ALWAYS_DISCOVER")) continue;
        const QByteArray reply = "ALWAYS_HERE " + QByteArray::number(kTcpPort) + " "
                                 + QHostInfo::localHostName().toUtf8();
        m_udp->writeDatagram(reply, dg.senderAddress(), dg.senderPort());
    }
}

void RemoteServer::sendLine(QTcpSocket *s, const QJsonObject &obj)
{
    s->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
}

void RemoteServer::broadcast(const QJsonObject &obj)
{
    const QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    for (QTcpSocket *s : m_clients) s->write(line);
}

void RemoteServer::publishStatus(const QJsonObject &status)
{
    QJsonObject o = status;
    o.insert("type", "status");
    const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    if (line == m_lastStatus) return;
    m_lastStatus = line;
    for (QTcpSocket *s : m_clients) s->write(line);
}

void RemoteServer::publishArt(qint64 id, const QByteArray &jpeg)
{
    if (id == m_lastArtId) return;
    m_lastArtId = id;
    QJsonObject o{
        {"type", "art"},
        {"id", double(id)},
        {"mime", "image/jpeg"},
        {"data", QString::fromLatin1(jpeg.toBase64())},
    };
    m_lastArtLine = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    for (QTcpSocket *s : m_clients) s->write(m_lastArtLine);
}
