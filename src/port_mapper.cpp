#include "port_mapper.h"
namespace bm {
static constexpr int kLeaseSeconds = 3600;
PortMapper::PortMapper(std::unique_ptr<PortMapperBackend> backend, int renewMs, QObject *parent)
    : QObject(parent), backend_(std::move(backend)) {
    pool_.setMaxThreadCount(1);
    renew_.setInterval(renewMs);
    connect(&renew_, &QTimer::timeout, this, &PortMapper::request);
}
PortMapper::~PortMapper() {
    stop();
    pool_.waitForDone(3000);
}
void PortMapper::start(quint16 port) {
    if (state_ != State::Off && port == port_)
        return;
    stop();
    port_ = port;
    state_ = State::Searching;
    emit changed();
    renew_.start();
    request();
}
void PortMapper::request() {
    const int generation = generation_;
    const quint16 port = port_;
    auto backend = backend_.get();
    pool_.start([this, backend, port, generation] {
        const auto result = backend->map(port, kLeaseSeconds);
        // Dropped by Qt if the mapper is gone by then.
        QMetaObject::invokeMethod(
            this, [this, result, generation] { finished(result, generation); },
            Qt::QueuedConnection);
    });
}
void PortMapper::finished(const PortMapperBackend::Result &result, int generation) {
    if (generation != generation_)
        return;
    state_ = result.ok ? State::Mapped : result.noGateway ? State::NoGateway : State::Failed;
    externalIp_ = result.externalIp;
    error_ = result.error;
    emit changed();
}
void PortMapper::stop() {
    ++generation_;
    renew_.stop();
    if (state_ == State::Off)
        return;
    // Queued after any mapping still in flight, so it is removed after it is made.
    auto backend = backend_.get();
    const quint16 port = port_;
    pool_.start([backend, port] { backend->unmap(port); });
    state_ = State::Off;
    externalIp_.clear();
    error_.clear();
    emit changed();
}
} // namespace bm
