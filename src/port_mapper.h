#pragma once
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <memory>
namespace bm {
// One conversation with the home router. Calls block (network round trips),
// so PortMapper makes them on its own thread, one at a time.
class PortMapperBackend {
  public:
    struct Result {
        bool ok = false;
        bool noGateway = false; // no router answered, or it has no internet link
        QString externalIp, error;
    };
    virtual ~PortMapperBackend() = default;
    // Finds the router (first call) and forwards TCP `port` to this machine.
    virtual Result map(quint16 port, int leaseSeconds) = 0;
    virtual void unmap(quint16 port) = 0;
};
// The production backend: UPnP IGD through miniupnpc.
std::unique_ptr<PortMapperBackend> miniupnpcBackend();
// Keeps the node's port forwarded on the router while started: maps it for an
// hour and renews it every `renewMs`; stop() and destruction remove it.
class PortMapper : public QObject {
    Q_OBJECT
  public:
    // CarrierNat: the router mapped the port but its own internet address is a
    // private or carrier-grade NAT one (100.64.0.0/10), so nobody outside can
    // reach it: the provider shares one public address between customers.
    enum class State { Off, Searching, Mapped, NoGateway, CarrierNat, Failed };
    explicit PortMapper(std::unique_ptr<PortMapperBackend> backend, int renewMs = 30 * 60 * 1000,
                        QObject *parent = nullptr);
    ~PortMapper() override; // removes the mapping, waiting up to 3 s
    void start(quint16 port);
    void stop();
    State state() const {
        return state_;
    }
    QString externalIp() const {
        return externalIp_;
    }
    QString error() const {
        return error_;
    }
    quint16 port() const {
        return port_;
    }
    // False for private, carrier-grade NAT, loopback and link-local addresses.
    static bool reachableAddress(const QString &ip);
  signals:
    void changed();

  private:
    void request();
    void finished(const PortMapperBackend::Result &result, int generation);
    std::unique_ptr<PortMapperBackend> backend_;
    QThreadPool pool_; // one thread: backend calls never overlap
    QTimer renew_;
    State state_ = State::Off;
    QString externalIp_, error_;
    quint16 port_ = 0;
    int generation_ = 0; // results from before the last stop() are dropped
};
} // namespace bm
