#include "portable_relay.h"
#include "pow.h"
#include "protocol.h"
#include "protocol_wire.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkProxy>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QtEndian>
#include <algorithm>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>

namespace bm {
namespace {
constexpr qsizetype MaxFrame = 1600009, MaxObject = 262144;
constexpr int MaxPeers = 8, MaxInventory = 65536, MaxAddresses = 2048;
constexpr qsizetype MaxPendingWrite = 2 * 1024 * 1024;
qint64 now() { return QDateTime::currentSecsSinceEpoch(); }
void require(bool value) { if (!value) throw std::runtime_error("Invalid relay packet"); }
void number(QByteArray &b, quint64 value, int size) {
    for (int i=size-1;i>=0;--i) b.append(char(value >> (i*8)));
}
void varint(QByteArray &b, quint64 value) {
    if(value<253) number(b,value,1);
    else if(value<=65535) {b.append(char(253));number(b,value,2);}
    else if(value<=0xffffffff) {b.append(char(254));number(b,value,4);}
    else {b.append(char(255));number(b,value,8);}
}
struct Reader {
    const QByteArray &data; qsizetype pos=0;
    QByteArray take(qsizetype n) {require(n>=0 && n<=data.size()-pos);auto b=data.mid(pos,n);pos+=n;return b;}
    quint64 num(int n) {auto b=take(n);quint64 value=0;for(unsigned char c:b)value=(value<<8)|c;return value;}
    quint64 var() {auto n=num(1);if(n<253)return n;auto v=num(n==253?2:n==254?4:8);require(v>=(n==253?253:n==254?65536:quint64(0x100000000)));return v;}
    void end() {require(pos==data.size());}
};
QByteArray frame(const QByteArray &command, const QByteArray &payload) {
    QByteArray result=QByteArray::fromHex("e9beb4d9");result+=command.leftJustified(12,'\0');
    number(result,payload.size(),4);
    result+=QCryptographicHash::hash(payload,QCryptographicHash::Sha512).left(4);
    return result+payload;
}
bool writeFile(const QString &path,const QByteArray &bytes) {
    QSaveFile f(path);return f.open(QIODevice::WriteOnly) && f.write(bytes)==bytes.size() && f.commit();
}
struct Endpoint {QHostAddress host;quint16 port=8444;};
std::optional<Endpoint> endpoint(QString text,quint16 fallback=8444) {
    QUrl url("tcp://"+text);QHostAddress host(url.host());int port=url.port(fallback);
    if(host.isNull() || port<1 || port>65535 || !url.userInfo().isEmpty() || !url.path().isEmpty())return {};
    return Endpoint{host,quint16(port)};
}
QString name(const Endpoint &e) {return (e.host.protocol()==QAbstractSocket::IPv6Protocol?"["+e.host.toString()+"]":e.host.toString())+":"+QString::number(e.port);}
bool publicAddress(const QHostAddress &a) {
    return !a.isNull() && !a.isLoopback() && !a.isLinkLocal() && !a.isMulticast() &&
        !a.isInSubnet(QHostAddress("10.0.0.0"),8) && !a.isInSubnet(QHostAddress("172.16.0.0"),12) &&
        !a.isInSubnet(QHostAddress("192.168.0.0"),16) && !a.isInSubnet(QHostAddress("0.0.0.0"),8) &&
        !a.isInSubnet(QHostAddress("fc00::"),7);
}
void wireAddress(QByteArray &b,const Endpoint &e) {
    number(b,1,8);auto ip=e.host.toIPv6Address();b.append(reinterpret_cast<const char *>(ip.c),16);number(b,e.port,2);
}
struct Peer {
    QPointer<QTcpSocket> socket;
    QByteArray input;
    QSet<QByteArray> requested;
    QList<QByteArray> wanted, serving, announcing;
    QString address;
    qint64 connectedAt=now(),lastActivity=now(),requestAt=0;
    bool gotVersion=false,gotVerack=false,ready=false;
};
class Relay : public QObject {
    QString root_;
    QLockFile lock_;
    QTimer timer_;
    QTcpServer listener_;
    QList<std::shared_ptr<Peer>> peers_;
    QHash<QByteArray,qint64> inventory_;
    QHash<QString,Endpoint> addresses_;
    QHash<QString,qint64> attempted_;
    std::unique_ptr<QDirIterator> startup_;
    quint64 nonce_=QRandomGenerator::system()->generate64();
    bool allowPrivate_=false, explicitOnly_=false;
    QNetworkProxy proxy_{QNetworkProxy::NoProxy};
    qint64 lastCleanup_=0;

    bool send(const std::shared_ptr<Peer> &p,const QByteArray &command,const QByteArray &payload={}) {
        if(!p->socket || p->socket->state()!=QAbstractSocket::ConnectedState)return false;
        const auto packet=frame(command,payload);
        if(p->socket->bytesToWrite()+packet.size()>MaxPendingWrite)return false;
        return p->socket->write(packet)==packet.size();
    }
    void announce(const std::shared_ptr<Peer> &p,const QList<QByteArray> &hashes) {
        for(qsizetype offset=0;offset<hashes.size();offset+=1000) {
            QByteArray b;const int size=std::min<qsizetype>(1000,hashes.size()-offset);varint(b,size);
            for(int i=0;i<size;++i)b+=hashes[offset+i];
            if(!send(p,"inv",b))break;
        }
    }
    int offer(const QByteArray &hash) {
        int count=0;QByteArray b;varint(b,1);b+=hash;
        for(auto p:peers_)if(p->ready && send(p,"inv",b))++count;
        return count;
    }
    void ready(const std::shared_ptr<Peer> &p) {
        if(p->ready || !p->gotVersion || !p->gotVerack)return;
        p->ready=true;p->announcing=inventory_.keys();
        QByteArray b;varint(b,0);send(p,"addr",b);
    }
    void request(const std::shared_ptr<Peer> &p) {
        if(!p->requested.isEmpty() || p->wanted.isEmpty())return;
        QByteArray b;QList<QByteArray> batch;
        while(!p->wanted.isEmpty() && batch.size()<32) {
            auto h=p->wanted.takeFirst();if(!inventory_.contains(h))batch<<h;
        }
        if(batch.isEmpty())return;
        varint(b,batch.size());for(const auto &h:batch)b+=h;
        if(send(p,"getdata",b)) {for(const auto &h:batch)p->requested.insert(h);p->requestAt=now();}
    }
    bool accept(const QByteArray &object) {
        if(!ProofOfWork::valid(object,now()))return false;
        const auto h=Wire::header(object);if(!h || h->stream!=1)return false;
        auto hash=QByteArray::fromHex(Protocol::inventoryHash(object).toLatin1());
        if(inventory_.contains(hash))return true;
        if(inventory_.size()>=MaxInventory)return false;
        if(!writeFile(root_+"/objects/"+QString::fromLatin1(hash.toHex()),object))return false;
        inventory_.insert(hash,h->expires);offer(hash);return true;
    }
    void version(const std::shared_ptr<Peer> &p,const QByteArray &b) {
        require(!p->gotVersion);Reader r{b};require(r.num(4)==3);r.num(8);auto time=r.num(8);
        require(time>=quint64(now()-3600) && time<=quint64(now()+3600));r.take(52);require(r.num(8)!=nonce_);
        auto agentLength=r.var();require(agentLength<=5000);r.take(agentLength);
        auto streams=r.var();require(streams>0 && streams<=160000);bool streamOne=false;
        for(quint64 i=0;i<streams;++i)streamOne|=r.var()==1;r.end();require(streamOne);
        p->gotVersion=true;send(p,"verack");ready(p);
    }
    void handle(const std::shared_ptr<Peer> &p,const QByteArray &command,const QByteArray &b) {
        if(command=="version") {version(p,b);return;}
        if(command=="verack") {require(b.isEmpty());p->gotVerack=true;ready(p);return;}
        // notbit-compatible peers can pipeline inventory before the final verack.
        if(command=="inv" || command=="getdata") {
            require(p->gotVersion || p->gotVerack);Reader r{b};auto count=r.var();require(count<=50000 && count*32==quint64(b.size()-r.pos));
            if(command=="inv") {
                QSet<QByteArray> queued;
                for (const auto &wanted : p->wanted)
                    queued.insert(wanted);
                for(quint64 i=0;i<count;++i) {auto h=r.take(32);if(p->wanted.size()<50000 && !inventory_.contains(h) && !p->requested.contains(h) && !queued.contains(h)) {p->wanted<<h;queued.insert(h);}}
                request(p);
            } else for(quint64 i=0;i<count;++i) {auto h=r.take(32);if(p->serving.size()<1024 && inventory_.contains(h) && !p->serving.contains(h))p->serving<<h;}
        } else if(command=="object") {
            require(b.size()<=MaxObject && (p->gotVersion || p->gotVerack));
            auto hash=QByteArray::fromHex(Protocol::inventoryHash(b).toLatin1());p->requested.remove(hash);
            accept(b);request(p);
        } else if(command=="addr" && p->ready && !explicitOnly_) {
            Reader r{b};auto count=r.var();require(count<=1000 && count*38==quint64(b.size()-r.pos));
            for(quint64 i=0;i<count;++i) {
                auto time=r.num(8),stream=r.num(4);r.num(8);auto bytes=r.take(16);quint16 port=r.num(2);
                Q_IPV6ADDR ip;std::copy(bytes.begin(),bytes.end(),ip.c);Endpoint e{QHostAddress(ip),port};
                if(port && stream==1 && time>=quint64(now()-3*3600) && time<=quint64(now()+600))addAddress(e);
            }
        } else if(command=="ping")send(p,"pong",b.left(8));
    }
    void receive(const std::shared_ptr<Peer> &p) {
        if(!p->socket)return;
        try {
            auto &b=p->input;
            b+=p->socket->read(std::max<qsizetype>(0,MaxFrame+24-b.size()));
            int packets=0;
            while(b.size()>=24 && packets++<16) {
                require(b.first(4)==QByteArray::fromHex("e9beb4d9"));
                auto raw=b.mid(4,12);auto zero=raw.indexOf('\0');require(zero>0 && raw.mid(zero)==QByteArray(12-zero,0));
                auto command=raw.left(zero);for(char c:command)require((c>='a' && c<='z') || (c>='0' && c<='9'));
                auto length=qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(b.constData()+16));
                require(length<=MaxFrame && (command!="object" || length<=MaxObject));
                if(b.size()<24+length)break;
                auto payload=b.mid(24,length);require(QCryptographicHash::hash(payload,QCryptographicHash::Sha512).left(4)==b.mid(20,4));
                b.remove(0,24+length);p->lastActivity=now();handle(p,command,payload);
            }
            if(packets>=16 || (p->socket && p->socket->bytesAvailable()>0))QTimer::singleShot(0,this,[this,p]{receive(p);});
        } catch(...) {if(p->socket)p->socket->abort();}
    }
    void attach(QTcpSocket *socket,const QString &address,bool connected=false) {
        auto p=std::make_shared<Peer>();p->socket=socket;p->address=address;peers_<<p;socket->setParent(this);
        socket->setReadBufferSize(MaxFrame+24);
        connect(socket,&QTcpSocket::readyRead,this,[this,p]{receive(p);});
        connect(socket,&QTcpSocket::disconnected,this,[this,p]{if(p->socket)p->socket->deleteLater();peers_.removeOne(p);});
        connect(socket,&QTcpSocket::errorOccurred,this,[this,p](QAbstractSocket::SocketError){if(p->socket)p->socket->deleteLater();peers_.removeOne(p);});
        auto greet=[this,p] {
            QByteArray b;number(b,3,4);number(b,1,8);number(b,now(),8);
            wireAddress(b,{p->socket->peerAddress(),p->socket->peerPort()});wireAddress(b,{QHostAddress::AnyIPv4,8444});number(b,nonce_,8);
            QByteArray agent="/ynotbit:0.3/";varint(b,agent.size());b+=agent;varint(b,1);varint(b,1);send(p,"version",b);
        };
        connect(socket,&QTcpSocket::connected,this,greet);if(connected)greet();
    }
    void addAddress(const Endpoint &e) {
        if(addresses_.size()<MaxAddresses && (allowPrivate_ || publicAddress(e.host)))addresses_.insert(name(e),e);
    }
    void recover() {
        int count=0;
        while(startup_ && startup_->hasNext() && count++<32 && inventory_.size()<MaxInventory) {
            auto path=startup_->next();auto info=startup_->fileInfo();auto hash=QByteArray::fromHex(info.fileName().toLatin1());
            if(info.isSymLink() || hash.size()!=32 || info.size()>MaxObject)continue;
            QFile file(path);if(!file.open(QIODevice::ReadOnly))continue;auto object=file.read(MaxObject+1);
            auto header=Wire::header(object);
            if(header && ProofOfWork::valid(object,now()) && Protocol::inventoryHash(object)==info.fileName()) {inventory_.insert(hash,header->expires);offer(hash);}
        }
        if(startup_ && (!startup_->hasNext() || inventory_.size()>=MaxInventory))startup_.reset();
    }
    void publish() {
        QDirIterator entries(root_+"/publish",{"*.object"},QDir::Files|QDir::NoSymLinks);int count=0;
        while(entries.hasNext() && count++<8) {
            auto path=entries.next();auto id=entries.fileInfo().completeBaseName();
            if(!QRegularExpression("^[a-f0-9-]{36}$").match(id).hasMatch())continue;
            QFile f(path);if(!f.open(QIODevice::ReadOnly))continue;auto b=f.read(MaxObject+1);f.close();
            auto hash=Protocol::inventoryHash(b);bool accepted=accept(b);int offered=accepted?offer(QByteArray::fromHex(hash.toLatin1())):0;
            QString state=!accepted?"rejected":offered?"offered":"accepted";
            if(writeFile(root_+"/receipts/"+id+".json",QJsonDocument(QJsonObject{{"state",state},{"hash",hash},{"time",now()}}).toJson()) && (!accepted || offered))QFile::remove(path);
        }
    }
    void tick() {
        recover();publish();int readyPeers=0,pending=0;
        for(auto p:peers_) {
            if(!p->socket)continue;
            if((!p->ready && now()-p->connectedAt>30) || now()-p->lastActivity>600) {p->socket->abort();continue;}
            readyPeers+=p->ready;pending+=p->wanted.size()+p->requested.size();
            if(p->ready && !p->announcing.isEmpty() && p->socket->bytesToWrite()<MaxPendingWrite-33000) {
                QList<QByteArray> batch;
                while(!p->announcing.isEmpty() && batch.size()<1000)batch<<p->announcing.takeFirst();
                announce(p,batch);
            }
            if(!p->requested.isEmpty() && now()-p->requestAt>60) {p->requested.clear();request(p);}
            if(!p->serving.isEmpty() && p->socket->bytesToWrite()<MaxPendingWrite-MaxObject-24) {
                auto hash=p->serving.takeFirst();QFile f(root_+"/objects/"+QString::fromLatin1(hash.toHex()));
                if(f.open(QIODevice::ReadOnly))send(p,"object",f.read(MaxObject));else inventory_.remove(hash);
            }
        }
        writeFile(root_+"/status.json",QJsonDocument(QJsonObject{{"peers",readyPeers},{"pending",pending},{"time",now()},{"backend","qt"},{"inventory",inventory_.size()}}).toJson());
        if(now()-lastCleanup_>60) {
            for(auto it=inventory_.begin();it!=inventory_.end();)if(it.value()<now() || !QFile::exists(root_+"/objects/"+QString::fromLatin1(it.key().toHex())))it=inventory_.erase(it);else ++it;
            QJsonArray addresses;for(const auto &e:addresses_)addresses.append(name(e));writeFile(root_+"/qt-peers.json",QJsonDocument(addresses).toJson());lastCleanup_=now();
        }
        if(peers_.size()<MaxPeers) for(auto it=addresses_.begin();it!=addresses_.end();++it) {
            bool connected=false;for(auto p:peers_)connected|=p->address==it.key();
            if(connected || now()-attempted_.value(it.key())<60)continue;
            attempted_[it.key()]=now();auto socket=new QTcpSocket;socket->setProxy(proxy_);attach(socket,it.key());socket->connectToHost(it->host,it->port);break;
        }
    }
  public:
    Relay(QString root,QStringList args):root_(std::move(root)),lock_(root_+"/qt-relay.lock") {
        require(QDir().mkpath(root_+"/objects") && QDir().mkpath(root_+"/publish") && QDir().mkpath(root_+"/receipts"));
        require(lock_.tryLock());allowPrivate_=args.contains("-L");explicitOnly_=args.contains("-e");
        auto option=[&](QString flag)->QString {auto i=args.indexOf(flag);return i>=0 && i+1<args.size()?args[i+1]:QString();};
        auto proxy=option("-r");if(args.contains("-T"))proxy="127.0.0.1:9050";
        if(!proxy.isEmpty()) {auto e=endpoint(proxy,9050);require(bool(e));proxy_=QNetworkProxy(QNetworkProxy::Socks5Proxy,e->host.toString(),e->port);}
        for(int i=0;i<args.size()-1;++i)if(args[i]=="-P") {auto e=endpoint(args[i+1]);require(bool(e));addresses_.insert(name(*e),*e);}
        if(!explicitOnly_) {
            QFile saved(root_+"/qt-peers.json");if(saved.open(QIODevice::ReadOnly))for(auto value:QJsonDocument::fromJson(saved.read(256*1024)).array())if(auto e=endpoint(value.toString()))addAddress(*e);
            if(!args.contains("-b"))for(auto text:{"176.31.246.114:8444","109.229.197.133:8444","184.75.69.2:8444"})if(auto e=endpoint(text))addAddress(*e);
            if(!args.contains("-b") && !args.contains("-B") && proxy.isEmpty())for(int port:{8080,8444})
                QHostInfo::lookupHost(QString("bootstrap%1.bitmessage.org").arg(port),this,[this,port](const QHostInfo &info){for(auto address:info.addresses())addAddress({address,quint16(port)});});
        }
        if(!args.contains("-i") && proxy.isEmpty()) {
            auto e=endpoint(option("-p").isEmpty()?"[::]:8444":option("-p"));require(bool(e));require(listener_.listen(e->host,e->port));
            connect(&listener_,&QTcpServer::newConnection,this,[this]{while(listener_.hasPendingConnections()){auto s=listener_.nextPendingConnection();if(peers_.size()>=MaxPeers){s->abort();s->deleteLater();}else attach(s,name({s->peerAddress(),s->peerPort()}),true);}});
        }
        startup_=std::make_unique<QDirIterator>(root_+"/objects",QDir::Files|QDir::NoSymLinks);
        connect(&timer_,&QTimer::timeout,this,[this]{tick();});timer_.start(1000);tick();
    }
};
}
int runPortableRelay(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        auto args=app.arguments();int i=args.indexOf("-D");
        require(i>=0 && i+1<args.size());Relay relay(args[i+1],args);return app.exec();
    } catch(const std::exception &e) {std::cerr<<"Native relay: "<<e.what()<<'\n';return 1;}
}
}
