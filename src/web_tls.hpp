#pragma once
#include <winsock2.h>
#include <windows.h>
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <string>
#include <vector>

namespace web {
std::string randomToken();
std::vector<std::string> hostAddresses();
std::string base64(const void* data, unsigned size);
std::string websocketAccept(const std::string& key);
class Credentials {
public:
    ~Credentials();
    void create(const std::wstring& folder);
    CredHandle handle{};
    std::string certificate, fingerprint, appleProfile;
private:
    bool ready_ = false;
    PCCERT_CONTEXT leaf_ = nullptr, root_ = nullptr;
};
// One connection thread owns the Schannel context and its record buffers.
class Stream {
public:
    explicit Stream(SOCKET socket) : socket_(socket) { SecInvalidateHandle(&context_); }
    ~Stream();
    bool negotiate(Credentials& credentials);
    bool secure() const { return secure_; }
    const std::string& error() const { return error_; }
    bool send(const std::string& data);
    int read(std::string& data, int timeoutMs); // -1 closed, 0 timeout, 1 data.
    bool beginBufferedWrites();
    bool flush();
    std::size_t pendingBytes() const { return outgoing_.size() - outgoingOffset_; }
private:
    SOCKET socket_;
    CtxtHandle context_{};
    SecPkgContext_StreamSizes sizes_{};
    bool contextReady_ = false, secure_ = false;
    std::string encrypted_;
    std::string error_;
    bool bufferedWrites_ = false;
    std::string outgoing_;
    std::size_t outgoingOffset_ = 0;
    ULONGLONG writeProgress_ = 0;
    bool rawSend(const char* data, unsigned size);
    int receive(int timeoutMs);
};
}
