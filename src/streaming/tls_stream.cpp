#include "streaming/tls_stream.hpp"

#include <algorithm>

namespace hb::streaming {

using hb::rtsp::IoStatus;

TlsStream::TlsStream(std::unique_ptr<hb::rtsp::ITcpStream> inner, std::unique_ptr<ITlsEngine> engine)
    : inner_(std::move(inner)), engine_(std::move(engine)) {}

TlsStream::~TlsStream() { close(); }

IoStatus TlsStream::fail(const std::string& msg) {
    error_ = msg;
    state_ = State::Failed;
    return IoStatus::Error;
}

IoStatus TlsStream::flushOut() {
    while (outOff_ < out_.size()) {
        std::size_t n = 0;
        const IoStatus st = inner_->send(out_.data() + outOff_, out_.size() - outOff_, n);
        if (st == IoStatus::Ok) {
            outOff_ += n;
        } else if (st == IoStatus::WouldBlock) {
            return IoStatus::WouldBlock;
        } else {
            return fail(st == IoStatus::Closed ? "a conexão foi fechada pelo servidor." : "falha de rede: " + inner_->lastError());
        }
    }
    out_.clear();
    outOff_ = 0;
    return IoStatus::Ok;
}

IoStatus TlsStream::readMore() {
    std::uint8_t buf[16384];
    std::size_t n = 0;
    const IoStatus st = inner_->recv(buf, sizeof buf, n);
    if (st == IoStatus::Ok) {
        in_.insert(in_.end(), buf, buf + n);
        return IoStatus::Ok;
    }
    if (st == IoStatus::WouldBlock) return IoStatus::WouldBlock;
    if (st == IoStatus::Closed) {
        peerClosed_ = true;
        return IoStatus::Closed;
    }
    return IoStatus::Error;
}

IoStatus TlsStream::connect(const std::string& host, std::uint16_t port) {
    resetSession();   // fresh session state (does not touch the transport: a stale close would hurt a pipe)
    if (!engine_) return fail("TLS indisponível neste sistema.");
    host_ = host;
    error_.clear();
    certError_ = false;
    if (!engine_->begin(host)) {
        certError_ = engine_->certificateError();
        return fail(engine_->lastError().empty() ? "não foi possível iniciar o TLS." : engine_->lastError());
    }
    const IoStatus st = inner_->connect(host, port);
    if (st == IoStatus::Error || st == IoStatus::Closed) return fail("não foi possível conectar: " + inner_->lastError());
    state_ = State::TcpConnecting;
    // Even when TCP connected instantly, report "in progress": the caller must go through
    // pollConnect(), which is what runs the TLS handshake.
    return IoStatus::WouldBlock;
}

IoStatus TlsStream::pollConnect() {
    if (state_ == State::Established) return IoStatus::Ok;
    if (state_ != State::TcpConnecting && state_ != State::Handshaking) return IoStatus::Error;

    if (state_ == State::TcpConnecting) {
        const IoStatus st = inner_->pollConnect();
        if (st == IoStatus::WouldBlock) return IoStatus::WouldBlock;
        if (st != IoStatus::Ok) return fail("não foi possível conectar: " + inner_->lastError());
        state_ = State::Handshaking;
        needRead_ = false;   // first handshake call (ClientHello) needs no input
    }

    for (;;) {
        const IoStatus fl = flushOut();
        if (fl != IoStatus::Ok) return fl;   // WouldBlock (retry later) or Error (already failed)
        if (engineDone_) {
            state_ = State::Established;
            return IoStatus::Ok;
        }
        if (needRead_) {
            const IoStatus r = readMore();
            if (r == IoStatus::WouldBlock) return IoStatus::WouldBlock;
            if (r == IoStatus::Closed) return fail("o servidor encerrou a conexão durante o handshake TLS.");
            if (r == IoStatus::Error) return fail("falha de rede durante o handshake TLS: " + inner_->lastError());
            needRead_ = false;
        }
        const TlsStatus hs = engine_->handshake(in_, out_);
        if (hs == TlsStatus::Ok) {
            engineDone_ = true;   // flush the final flight (if any), then report Established
        } else if (hs == TlsStatus::NeedMoreInput) {
            needRead_ = true;
        } else {
            certError_ = engine_->certificateError();
            return fail(engine_->lastError().empty() ? "falha no handshake TLS." : engine_->lastError());
        }
    }
}

IoStatus TlsStream::send(const std::uint8_t* data, std::size_t len, std::size_t& sent) {
    sent = 0;
    if (state_ != State::Established) return IoStatus::Error;
    // Keep records in order: anything still queued goes out first.
    const IoStatus fl = flushOut();
    if (fl == IoStatus::WouldBlock) return IoStatus::WouldBlock;
    if (fl != IoStatus::Ok) return fl;
    if (len == 0) return IoStatus::Ok;
    const std::size_t chunk = std::min(len, std::max<std::size_t>(1, engine_->maxPlaintext()));
    if (engine_->encrypt(data, chunk, out_) != TlsStatus::Ok) {
        return fail(engine_->lastError().empty() ? "falha ao cifrar dados TLS." : engine_->lastError());
    }
    // Accepted: the record now lives in out_. If the socket took only part of it, the rest goes out
    // before anything newer: on the next send()/waitReadable()/close() (media arrives every few ms).
    sent = chunk;
    const IoStatus f2 = flushOut();
    if (f2 == IoStatus::Error) return IoStatus::Error;
    return IoStatus::Ok;
}

IoStatus TlsStream::recv(std::uint8_t* buf, std::size_t cap, std::size_t& got) {
    got = 0;
    if (state_ != State::Established) return IoStatus::Error;
    if (cap == 0) return IoStatus::Ok;
    auto serve = [&]() {
        const std::size_t n = std::min(cap, plain_.size() - plainOff_);
        std::copy_n(plain_.begin() + static_cast<std::ptrdiff_t>(plainOff_), n, buf);
        plainOff_ += n;
        if (plainOff_ >= plain_.size()) {
            plain_.clear();
            plainOff_ = 0;
        }
        got = n;
        return IoStatus::Ok;
    };
    if (plainOff_ < plain_.size()) return serve();
    if (peerClosed_ && in_.empty()) return IoStatus::Closed;

    for (;;) {
        if (!in_.empty()) {
            const std::size_t before = in_.size();
            const TlsStatus st = engine_->decrypt(in_, plain_, out_);
            if (!out_.empty()) (void)flushOut();   // e.g. a key-update reply; WouldBlock is fine, send() retries
            if (st == TlsStatus::Ok) {
                if (plainOff_ < plain_.size()) return serve();
                if (in_.size() < before) continue;   // consumed a record with no payload: try the next one
                // no progress: treat as "need more input" to avoid spinning
            } else if (st == TlsStatus::Closed) {
                peerClosed_ = true;
                return IoStatus::Closed;
            } else if (st == TlsStatus::Error) {
                certError_ = false;
                return fail(engine_->lastError().empty() ? "falha ao decifrar dados TLS." : engine_->lastError());
            }
        }
        const IoStatus r = readMore();
        if (r == IoStatus::Ok) continue;
        if (r == IoStatus::WouldBlock) return IoStatus::WouldBlock;
        if (r == IoStatus::Closed) return IoStatus::Closed;
        return fail("falha de rede: " + inner_->lastError());
    }
}

bool TlsStream::waitReadable(int timeoutMs) {
    if (state_ == State::Established && outOff_ < out_.size()) (void)flushOut();   // push a record left half-sent
    if (plainOff_ < plain_.size()) return true;
    if (state_ == State::Established && !in_.empty()) return true;
    return inner_->waitReadable(timeoutMs);
}

void TlsStream::close() {
    if (state_ == State::Established && engine_) {
        engine_->shutdown(out_);
        (void)flushOut();   // best effort close_notify; never blocks
    }
    if (inner_) inner_->close();
    resetSession();
}

void TlsStream::resetSession() {
    state_ = State::Idle;
    engineDone_ = false;
    needRead_ = false;
    peerClosed_ = false;
    in_.clear();
    out_.clear();
    plain_.clear();
    plainOff_ = 0;
    outOff_ = 0;
}

}  // namespace hb::streaming
