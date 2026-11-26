// Windows TLS client engine on Schannel (SSPI). Sans-IO: see tls_engine.hpp / tls_stream.cpp for the
// socket loop. NOT validated against a real server in CI (there is no Windows runner with internet
// in the test job): reviewed carefully and syntax-checked only. Safety properties by construction:
//  - the credential is created WITHOUT SCH_CRED_MANUAL_CRED_VALIDATION, so Schannel validates the
//    chain against the Windows root store and matches the host name (pszTargetName);
//  - after the handshake the certificate chain is validated AGAIN explicitly with
//    CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, server name) - belt and braces;
//  - only TLS 1.2 is enabled (all current RTMPS ingests accept it; avoids TLS 1.3 post-handshake
//    SEC_I_RENEGOTIATE ticket handling, which this minimal engine does not implement);
//  - there is no option anywhere to skip verification.
#ifdef _WIN32

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <security.h>
#include <schannel.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "streaming/tls_engine.hpp"

namespace hb::streaming {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string hex32(unsigned long v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lX", v);
    return b;
}

class SchannelEngine final : public ITlsEngine {
public:
    ~SchannelEngine() override { reset(); }

    bool begin(const std::string& serverName) override {
        reset();
        server_ = widen(serverName);
        if (server_.empty()) {
            error_ = "nome do servidor inválido para TLS.";
            return false;
        }
        SCHANNEL_CRED sc;
        std::memset(&sc, 0, sizeof sc);
        sc.dwVersion = SCHANNEL_CRED_VERSION;
        // No client certificate, and (deliberately) NOT SCH_CRED_MANUAL_CRED_VALIDATION: Schannel itself
        // validates the server chain and name. TLS 1.2 only (see file comment).
        sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_AUTO_CRED_VALIDATION;
        sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
        TimeStamp expiry;
        const SECURITY_STATUS ss = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
                                                             nullptr, &sc, nullptr, nullptr, &cred_, &expiry);
        if (ss != SEC_E_OK) {
            error_ = "não foi possível iniciar o TLS do Windows (Schannel, erro " + hex32(static_cast<unsigned long>(ss)) + ").";
            return false;
        }
        haveCred_ = true;
        first_ = true;
        return true;
    }

    TlsStatus handshake(std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out) override {
        if (!haveCred_) return failMsg("sessão TLS não iniciada.");
        const DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                            ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
        for (;;) {
            SecBuffer outBuf;
            outBuf.BufferType = SECBUFFER_TOKEN;
            outBuf.cbBuffer = 0;
            outBuf.pvBuffer = nullptr;
            SecBufferDesc outDesc;
            outDesc.ulVersion = SECBUFFER_VERSION;
            outDesc.cBuffers = 1;
            outDesc.pBuffers = &outBuf;

            SecBuffer inBufs[2];
            SecBufferDesc inDesc;
            inDesc.ulVersion = SECBUFFER_VERSION;
            inDesc.cBuffers = 2;
            inDesc.pBuffers = inBufs;

            DWORD attrs = 0;
            TimeStamp expiry;
            SECURITY_STATUS ss;
            const bool wasFirst = first_;
            if (first_) {
                ss = InitializeSecurityContextW(&cred_, nullptr, const_cast<SEC_WCHAR*>(server_.c_str()), flags, 0, 0, nullptr, 0,
                                                &ctx_, &outDesc, &attrs, &expiry);
                if (ss == SEC_I_CONTINUE_NEEDED) {
                    haveCtx_ = true;
                    first_ = false;
                }
            } else {
                if (in.empty()) return TlsStatus::NeedMoreInput;
                inBufs[0].BufferType = SECBUFFER_TOKEN;
                inBufs[0].cbBuffer = static_cast<unsigned long>(in.size());
                inBufs[0].pvBuffer = in.data();
                inBufs[1].BufferType = SECBUFFER_EMPTY;
                inBufs[1].cbBuffer = 0;
                inBufs[1].pvBuffer = nullptr;
                ss = InitializeSecurityContextW(&cred_, &ctx_, const_cast<SEC_WCHAR*>(server_.c_str()), flags, 0, 0, &inDesc, 0,
                                                nullptr, &outDesc, &attrs, &expiry);
            }

            if (outBuf.pvBuffer) {
                if (outBuf.cbBuffer > 0) {
                    const auto* p = static_cast<const std::uint8_t*>(outBuf.pvBuffer);
                    out.insert(out.end(), p, p + outBuf.cbBuffer);
                }
                FreeContextBuffer(outBuf.pvBuffer);
            }

            if (ss == SEC_E_INCOMPLETE_MESSAGE) return TlsStatus::NeedMoreInput;   // nothing consumed

            if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) {
                // Consume what Schannel used; SECBUFFER_EXTRA marks bytes it left untouched.
                if (!wasFirst && !in.empty()) {
                    std::size_t extra = 0;
                    if (inBufs[1].BufferType == SECBUFFER_EXTRA) extra = std::min<std::size_t>(inBufs[1].cbBuffer, in.size());
                    in.erase(in.begin(), in.end() - static_cast<std::ptrdiff_t>(extra));
                }
                if (ss == SEC_E_OK) return finishHandshake();
                if (!in.empty()) continue;   // more handshake bytes already buffered: keep going
                return TlsStatus::NeedMoreInput;
            }
            if (ss == SEC_I_INCOMPLETE_CREDENTIALS) return failMsg("o servidor exigiu certificado de cliente (não suportado).");
            return failSecurity("falha no handshake TLS", ss);
        }
    }

    std::size_t maxPlaintext() const override { return sizesOk_ ? sizes_.cbMaximumMessage : 16384; }

    TlsStatus encrypt(const std::uint8_t* plain, std::size_t n, std::vector<std::uint8_t>& out) override {
        if (!sizesOk_ || n > sizes_.cbMaximumMessage) return failMsg("uso inválido do TLS ao cifrar.");
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(sizes_.cbHeader) + n + sizes_.cbTrailer);
        if (n) std::memcpy(buf.data() + sizes_.cbHeader, plain, n);
        SecBuffer b[4];
        b[0].BufferType = SECBUFFER_STREAM_HEADER;
        b[0].cbBuffer = sizes_.cbHeader;
        b[0].pvBuffer = buf.data();
        b[1].BufferType = SECBUFFER_DATA;
        b[1].cbBuffer = static_cast<unsigned long>(n);
        b[1].pvBuffer = buf.data() + sizes_.cbHeader;
        b[2].BufferType = SECBUFFER_STREAM_TRAILER;
        b[2].cbBuffer = sizes_.cbTrailer;
        b[2].pvBuffer = buf.data() + sizes_.cbHeader + n;
        b[3].BufferType = SECBUFFER_EMPTY;
        b[3].cbBuffer = 0;
        b[3].pvBuffer = nullptr;
        SecBufferDesc desc;
        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = b;
        const SECURITY_STATUS ss = EncryptMessage(&ctx_, 0, &desc, 0);
        if (ss != SEC_E_OK) return failSecurity("falha ao cifrar dados TLS", ss);
        const std::size_t total = static_cast<std::size_t>(b[0].cbBuffer) + b[1].cbBuffer + b[2].cbBuffer;
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(std::min(total, buf.size())));
        return TlsStatus::Ok;
    }

    TlsStatus decrypt(std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& plain, std::vector<std::uint8_t>& /*out*/) override {
        if (in.empty()) return TlsStatus::NeedMoreInput;
        SecBuffer b[4];
        b[0].BufferType = SECBUFFER_DATA;
        b[0].cbBuffer = static_cast<unsigned long>(in.size());
        b[0].pvBuffer = in.data();
        for (int i = 1; i < 4; ++i) {
            b[i].BufferType = SECBUFFER_EMPTY;
            b[i].cbBuffer = 0;
            b[i].pvBuffer = nullptr;
        }
        SecBufferDesc desc;
        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = b;
        const SECURITY_STATUS ss = DecryptMessage(&ctx_, &desc, 0, nullptr);
        if (ss == SEC_E_INCOMPLETE_MESSAGE) return TlsStatus::NeedMoreInput;
        if (ss == SEC_I_CONTEXT_EXPIRED) return TlsStatus::Closed;   // close_notify
        if (ss == SEC_I_RENEGOTIATE) return failMsg("o servidor pediu renegociação TLS (não suportada).");
        if (ss != SEC_E_OK) return failSecurity("falha ao decifrar dados TLS", ss);
        std::size_t extra = 0;
        for (int i = 1; i < 4; ++i) {
            if (b[i].BufferType == SECBUFFER_DATA && b[i].pvBuffer && b[i].cbBuffer) {
                const auto* p = static_cast<const std::uint8_t*>(b[i].pvBuffer);
                plain.insert(plain.end(), p, p + b[i].cbBuffer);   // copy BEFORE erasing: p points into `in`
            } else if (b[i].BufferType == SECBUFFER_EXTRA) {
                extra = std::min<std::size_t>(b[i].cbBuffer, in.size());
            }
        }
        in.erase(in.begin(), in.end() - static_cast<std::ptrdiff_t>(extra));
        return TlsStatus::Ok;
    }

    void shutdown(std::vector<std::uint8_t>& out) override {
        if (!haveCtx_ || !sizesOk_) return;
        DWORD type = SCHANNEL_SHUTDOWN;
        SecBuffer tok;
        tok.BufferType = SECBUFFER_TOKEN;
        tok.cbBuffer = sizeof type;
        tok.pvBuffer = &type;
        SecBufferDesc td;
        td.ulVersion = SECBUFFER_VERSION;
        td.cBuffers = 1;
        td.pBuffers = &tok;
        if (ApplyControlToken(&ctx_, &td) != SEC_E_OK) return;
        SecBuffer ob;
        ob.BufferType = SECBUFFER_TOKEN;
        ob.cbBuffer = 0;
        ob.pvBuffer = nullptr;
        SecBufferDesc od;
        od.ulVersion = SECBUFFER_VERSION;
        od.cBuffers = 1;
        od.pBuffers = &ob;
        DWORD attrs = 0;
        TimeStamp expiry;
        const DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                            ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
        const SECURITY_STATUS ss = InitializeSecurityContextW(&cred_, &ctx_, nullptr, flags, 0, 0, nullptr, 0, &ctx_, &od, &attrs, &expiry);
        if (ob.pvBuffer) {
            if ((ss == SEC_E_OK || ss == SEC_I_CONTEXT_EXPIRED) && ob.cbBuffer > 0) {
                const auto* p = static_cast<const std::uint8_t*>(ob.pvBuffer);
                out.insert(out.end(), p, p + ob.cbBuffer);
            }
            FreeContextBuffer(ob.pvBuffer);
        }
    }

    std::string lastError() const override { return error_; }
    bool certificateError() const override { return certError_; }

private:
    void reset() {
        if (haveCtx_) DeleteSecurityContext(&ctx_);
        if (haveCred_) FreeCredentialsHandle(&cred_);
        haveCtx_ = haveCred_ = false;
        sizesOk_ = false;
        first_ = true;
        error_.clear();
        certError_ = false;
        std::memset(&ctx_, 0, sizeof ctx_);
        std::memset(&cred_, 0, sizeof cred_);
        std::memset(&sizes_, 0, sizeof sizes_);
    }

    TlsStatus failMsg(const std::string& m) {
        error_ = m;
        return TlsStatus::Error;
    }

    TlsStatus failSecurity(const std::string& what, SECURITY_STATUS ss) {
        const unsigned long code = static_cast<unsigned long>(ss);
        switch (static_cast<unsigned long>(ss)) {
            case 0x80090325UL:   // SEC_E_UNTRUSTED_ROOT
                certError_ = true;
                error_ = "certificado do servidor não confiável (autoridade raiz desconhecida).";
                break;
            case 0x80090328UL:   // SEC_E_CERT_EXPIRED
                certError_ = true;
                error_ = "certificado do servidor expirado.";
                break;
            case 0x80090322UL:   // SEC_E_WRONG_PRINCIPAL
                certError_ = true;
                error_ = "o certificado do servidor não corresponde ao nome do servidor.";
                break;
            case 0x80090327UL:   // SEC_E_CERT_UNKNOWN
            case 0x800B0109UL:   // CERT_E_UNTRUSTEDROOT
            case 0x800B010AUL:   // CERT_E_CHAINING
            case 0x800B0101UL:   // CERT_E_EXPIRED
            case 0x800B010FUL:   // CERT_E_CN_NO_MATCH
            case 0x800B0110UL:   // CERT_E_WRONG_USAGE
                certError_ = true;
                error_ = "certificado do servidor rejeitado (" + hex32(code) + ").";
                break;
            default:
                error_ = what + " (" + hex32(code) + ").";
                break;
        }
        return TlsStatus::Error;
    }

    TlsStatus finishHandshake() {
        if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK)
            return failMsg("não foi possível obter os tamanhos de registro TLS.");
        sizesOk_ = true;
        return verifyServerCertificate();
    }

    // Explicit second validation of the server certificate (chain to a trusted root + name match).
    TlsStatus verifyServerCertificate() {
        PCCERT_CONTEXT cert = nullptr;
        if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK || !cert) {
            certError_ = true;
            return failMsg("o servidor não apresentou certificado.");
        }
        static const char* const kUsages[] = {szOID_PKIX_KP_SERVER_AUTH};
        CERT_CHAIN_PARA chainPara;
        std::memset(&chainPara, 0, sizeof chainPara);
        chainPara.cbSize = sizeof chainPara;
        chainPara.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
        chainPara.RequestedUsage.Usage.cUsageIdentifier = 1;
        chainPara.RequestedUsage.Usage.rgpszUsageIdentifier = const_cast<LPSTR*>(kUsages);

        PCCERT_CHAIN_CONTEXT chain = nullptr;
        const BOOL gotChain = CertGetCertificateChain(nullptr, cert, nullptr, cert->hCertStore, &chainPara, 0, nullptr, &chain);
        if (!gotChain || !chain) {
            CertFreeCertificateContext(cert);
            certError_ = true;
            return failMsg("não foi possível montar a cadeia do certificado do servidor.");
        }
        HTTPSPolicyCallbackData https;
        std::memset(&https, 0, sizeof https);
        https.cbStruct = sizeof https;
        https.dwAuthType = AUTHTYPE_SERVER;
        https.fdwChecks = 0;   // no SECURITY_FLAG_IGNORE_* flags, ever
        std::wstring name = server_;
        https.pwszServerName = &name[0];
        CERT_CHAIN_POLICY_PARA policyPara;
        std::memset(&policyPara, 0, sizeof policyPara);
        policyPara.cbSize = sizeof policyPara;
        policyPara.pvExtraPolicyPara = &https;
        CERT_CHAIN_POLICY_STATUS policyStatus;
        std::memset(&policyStatus, 0, sizeof policyStatus);
        policyStatus.cbSize = sizeof policyStatus;
        const BOOL ran = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policyPara, &policyStatus);
        const DWORD policyErr = policyStatus.dwError;
        CertFreeCertificateChain(chain);
        CertFreeCertificateContext(cert);
        if (!ran || policyErr != 0) {
            certError_ = true;
            if (policyErr == static_cast<DWORD>(0x800B010FUL)) return failMsg("o certificado do servidor não corresponde ao nome do servidor.");
            return failMsg("certificado do servidor rejeitado na validação (" + hex32(policyErr) + ").");
        }
        return TlsStatus::Ok;
    }

    CredHandle cred_{};
    CtxtHandle ctx_{};
    bool haveCred_ = false, haveCtx_ = false, first_ = true, sizesOk_ = false, certError_ = false;
    SecPkgContext_StreamSizes sizes_{};
    std::wstring server_;
    std::string error_;
};

}  // namespace

std::unique_ptr<ITlsEngine> makeSystemTlsEngine() { return std::make_unique<SchannelEngine>(); }

}  // namespace hb::streaming

#endif  // _WIN32
