#include "port_mapper.h"
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>
namespace bm {
namespace {
class Miniupnpc : public PortMapperBackend {
    UPNPUrls urls_{};
    IGDdatas data_{};
    bool haveRouter_ = false;
    char lan_[64] = {}; // this machine's address as the router sees it
    void forget() {
        if (haveRouter_)
            FreeUPNPUrls(&urls_);
        haveRouter_ = false;
    }
    bool find(Result &result) {
        if (haveRouter_)
            return true;
        int error = 0;
        UPNPDev *devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
        char wan[64] = {};
        const int found = devices ? UPNP_GetValidIGD(devices, &urls_, &data_, lan_, sizeof lan_,
                                                     wan, sizeof wan)
                                  : UPNP_NO_IGD;
        freeUPNPDevlist(devices);
        if (found == UPNP_CONNECTED_IGD || found == UPNP_PRIVATEIP_IGD) {
            haveRouter_ = true;
            return true;
        }
        if (found != UPNP_NO_IGD) // the structures are filled in every other case
            FreeUPNPUrls(&urls_);
        result.noGateway = true;
        result.error = found == UPNP_NO_IGD ? QStringLiteral("no UPnP router found")
                                            : QStringLiteral("the router has no internet link");
        return false;
    }

  public:
    ~Miniupnpc() override {
        forget();
    }
    Result map(quint16 port, int leaseSeconds) override {
        Result result;
        if (!find(result))
            return result;
        const auto p = QByteArray::number(port), lease = QByteArray::number(leaseSeconds);
        const int rc = UPNP_AddPortMapping(urls_.controlURL, data_.first.servicetype,
                                           p.constData(), p.constData(), lan_, "ynotbit", "TCP",
                                           nullptr, lease.constData());
        if (rc != UPNPCOMMAND_SUCCESS) {
            result.error = QString::fromLatin1(strupnperror(rc));
            forget(); // the router may have changed: look again next time
            return result;
        }
        char ip[40] = {};
        if (UPNP_GetExternalIPAddress(urls_.controlURL, data_.first.servicetype, ip) ==
            UPNPCOMMAND_SUCCESS)
            result.externalIp = QString::fromLatin1(ip);
        result.ok = true;
        return result;
    }
    void unmap(quint16 port) override {
        if (!haveRouter_)
            return;
        const auto p = QByteArray::number(port);
        UPNP_DeletePortMapping(urls_.controlURL, data_.first.servicetype, p.constData(), "TCP",
                               nullptr);
    }
};
} // namespace
std::unique_ptr<PortMapperBackend> miniupnpcBackend() {
    return std::make_unique<Miniupnpc>();
}
} // namespace bm
