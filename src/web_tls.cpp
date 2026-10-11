#include "web_tls.hpp"
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <cstring>

namespace web {
namespace {
void require(bool value, const char* operation) { if (!value) throw std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")"); }
std::vector<BYTE> encode(LPCSTR type, const void* value) {
    DWORD size = 0; require(CryptEncodeObjectEx(X509_ASN_ENCODING, type, value, 0, nullptr, nullptr, &size), "Encode web certificate");
    std::vector<BYTE> data(size); require(CryptEncodeObjectEx(X509_ASN_ENCODING, type, value, 0, nullptr, data.data(), &size), "Encode web certificate"); return data;
}
std::vector<BYTE> name(const wchar_t* text) {
    DWORD size = 0; require(CertStrToNameW(X509_ASN_ENCODING, text, CERT_X500_NAME_STR, nullptr, nullptr, &size, nullptr), "Encode certificate name");
    std::vector<BYTE> data(size); require(CertStrToNameW(X509_ASN_ENCODING, text, CERT_X500_NAME_STR, nullptr, data.data(), &size, nullptr), "Encode certificate name"); return data;
}
std::vector<BYTE> hash(const void* input, unsigned size, LPCWSTR algorithm, unsigned bytes) {
    BCRYPT_ALG_HANDLE alg = nullptr; require(BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0) >= 0, "Open hash provider");
    std::vector<BYTE> digest(bytes);
    auto result = BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(static_cast<const BYTE*>(input)), size, digest.data(), bytes);
    BCryptCloseAlgorithmProvider(alg, 0); require(result >= 0, "Hash web data"); return digest;
}
std::string hex(const BYTE* data, unsigned size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < size; ++i) { result += digits[data[i] >> 4]; result += digits[data[i] & 15]; }
    return result;
}
FILETIME shiftTime(FILETIME time, LONGLONG seconds) {
    ULARGE_INTEGER value{}; value.LowPart = time.dwLowDateTime; value.HighPart = time.dwHighDateTime;
    value.QuadPart += seconds * 10000000LL;
    return {value.LowPart, value.HighPart};
}
std::string profileUuid() {
    auto value = randomToken(); value[12] = '4'; value[16] = 'a';
    return value.substr(0,8) + '-' + value.substr(8,4) + '-' + value.substr(12,4) + '-' + value.substr(16,4) + '-' + value.substr(20);
}
std::string certificateProfile(const std::string& certificate, const std::string& fingerprint) {
    const auto identifier = "uk.m0nxd.ft710.station." + fingerprint.substr(0,16);
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>"
        "<key>PayloadType</key><string>Configuration</string>"
        "<key>PayloadVersion</key><integer>1</integer>"
        "<key>PayloadIdentifier</key><string>" + identifier + "</string>"
        "<key>PayloadUUID</key><string>" + profileUuid() + "</string>"
        "<key>PayloadDisplayName</key><string>FT-710 Station Certificate</string>"
        "<key>PayloadDescription</key><string>Installs the public root certificate for this FT-710 dashboard. After installation, enable M0NXD FT710 Local Web Root in Certificate Trust Settings to use HTTPS and microphone audio.</string>"
        "<key>PayloadOrganization</key><string>M0NXD</string>"
        "<key>PayloadContent</key><array><dict>"
        "<key>PayloadType</key><string>com.apple.security.root</string>"
        "<key>PayloadVersion</key><integer>1</integer>"
        "<key>PayloadIdentifier</key><string>" + identifier + ".root</string>"
        "<key>PayloadUUID</key><string>" + profileUuid() + "</string>"
        "<key>PayloadDisplayName</key><string>M0NXD FT710 Local Web Root</string>"
        "<key>PayloadCertificateFileName</key><string>ft710-web-root.cer</string>"
        "<key>PayloadContent</key><data>" + base64(certificate.data(),static_cast<unsigned>(certificate.size())) + "</data>"
        "</dict></array></dict></plist>\n";
}
}
std::string randomToken() {
    std::array<BYTE, 16> data{}; require(BCryptGenRandom(nullptr, data.data(), data.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "Create web access token");
    return hex(data.data(), data.size());
}
std::string base64(const void* data, unsigned size) {
    DWORD length = 0; require(CryptBinaryToStringA(static_cast<const BYTE*>(data), size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length), "Encode web token");
    std::string result(length, 0); require(CryptBinaryToStringA(static_cast<const BYTE*>(data), size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &length), "Encode web token");
    result.resize(length); if (!result.empty() && result.back() == 0) result.pop_back(); return result;
}
std::string websocketAccept(const std::string& key) {
    auto input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    auto digest = hash(input.data(), static_cast<unsigned>(input.size()), BCRYPT_SHA1_ALGORITHM, 20); return base64(digest.data(), digest.size());
}
std::vector<std::string> hostAddresses() {
    std::set<std::string> names{"localhost", "127.0.0.1", "::1"};
    char hostname[256]{}; if (gethostname(hostname, sizeof hostname) == 0) names.insert(hostname);
    ULONG size = 16384; std::vector<BYTE> storage(size);
    auto result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()), &size);
    if (result == ERROR_BUFFER_OVERFLOW) { storage.resize(size); result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()), &size); }
    if (result == NO_ERROR) for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        for (auto* item = adapter->FirstUnicastAddress; item; item = item->Next) {
            char address[INET6_ADDRSTRLEN]{}; auto* socket = item->Address.lpSockaddr;
            if (socket->sa_family == AF_INET) inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(socket)->sin_addr, address, sizeof address);
            else if (socket->sa_family == AF_INET6) {
                auto* v6 = reinterpret_cast<sockaddr_in6*>(socket);
                if (IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr)) continue;
                inet_ntop(AF_INET6, &v6->sin6_addr, address, sizeof address);
            }
            if (*address) names.insert(address);
        }
    }
    return {names.begin(), names.end()};
}
Credentials::~Credentials() {
    if (ready_) FreeCredentialsHandle(&handle);
    if (leaf_) CertFreeCertificateContext(leaf_);
    if (root_) CertFreeCertificateContext(root_);
}
void Credentials::create(const std::wstring& folder) {
    auto addresses = hostAddresses();
    HCRYPTPROV provider = 0; HCRYPTKEY key = 0;
    const wchar_t* container = L"M0NXD.FT710Dashboard.WebTLS.v1";
    const wchar_t* providerName = MS_ENH_RSA_AES_PROV_W;
    require(CryptAcquireContextW(&provider, container, providerName, PROV_RSA_AES, 0) || CryptAcquireContextW(&provider, container, providerName, PROV_RSA_AES, CRYPT_NEWKEYSET), "Open web certificate key");
    struct KeyCleanup { HCRYPTPROV provider; HCRYPTKEY key = 0; ~KeyCleanup() { if (key) CryptDestroyKey(key); CryptReleaseContext(provider, 0); } } cleanup{provider};
    require(CryptGetUserKey(provider, AT_KEYEXCHANGE, &key) || CryptGenKey(provider, AT_KEYEXCHANGE, 2048 << 16, &key), "Create web certificate key"); cleanup.key = key;
    CRYPT_KEY_PROV_INFO keyInfo{}; keyInfo.pwszContainerName = const_cast<wchar_t*>(container); keyInfo.pwszProvName = const_cast<wchar_t*>(providerName); keyInfo.dwProvType = PROV_RSA_AES; keyInfo.dwKeySpec = AT_KEYEXCHANGE;
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"MY"); require(store != nullptr, "Open current-user certificate store");
    struct StoreCleanup { HCERTSTORE value; ~StoreCleanup() { CertCloseStore(value, 0); } } storeCleanup{store};
    DWORD publicSize = 0;
    require(CryptExportPublicKeyInfo(provider, AT_KEYEXCHANGE, X509_ASN_ENCODING, nullptr, &publicSize), "Read web certificate public key");
    std::vector<BYTE> publicData(publicSize);
    auto* publicKey = reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(publicData.data());
    require(CryptExportPublicKeyInfo(provider, AT_KEYEXCHANGE, X509_ASN_ENCODING, publicKey, &publicSize), "Read web certificate public key");
    PCCERT_CONTEXT candidate = nullptr;
    while ((candidate = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, L"M0NXD FT710 Local Web Root", candidate))) {
        if (CertVerifyTimeValidity(nullptr, candidate->pCertInfo) == 0 && CertComparePublicKeyInfo(X509_ASN_ENCODING, &candidate->pCertInfo->SubjectPublicKeyInfo, publicKey)) {
            root_ = CertDuplicateCertificateContext(candidate); CertFreeCertificateContext(candidate); break;
        }
    }
    CRYPT_ALGORITHM_IDENTIFIER algorithm{}; algorithm.pszObjId = const_cast<char*>(szOID_RSA_SHA256RSA);
    if (!root_) {
        auto subjectData = name(L"CN=M0NXD FT710 Local Web Root"); CERT_NAME_BLOB subject{static_cast<DWORD>(subjectData.size()), subjectData.data()};
        CERT_BASIC_CONSTRAINTS2_INFO basic{}; basic.fCA = TRUE; basic.fPathLenConstraint = TRUE; basic.dwPathLenConstraint = 0;
        auto basicData = encode(X509_BASIC_CONSTRAINTS2, &basic);
        BYTE usageBits = CERT_KEY_CERT_SIGN_KEY_USAGE | CERT_CRL_SIGN_KEY_USAGE;
        CRYPT_BIT_BLOB usage{1, &usageBits, 1}; auto usageData = encode(X509_KEY_USAGE, &usage);
        auto identifierBytes = hash(publicKey->PublicKey.pbData,publicKey->PublicKey.cbData,BCRYPT_SHA1_ALGORITHM,20);
        CRYPT_DATA_BLOB identifier{static_cast<DWORD>(identifierBytes.size()),identifierBytes.data()}; auto identifierData = encode(X509_OCTET_STRING,&identifier);
        std::array<CERT_EXTENSION,3> rootExtensions{{
            {const_cast<char*>(szOID_BASIC_CONSTRAINTS2), TRUE, {static_cast<DWORD>(basicData.size()), basicData.data()}},
            {const_cast<char*>(szOID_KEY_USAGE), TRUE, {static_cast<DWORD>(usageData.size()), usageData.data()}},
            {const_cast<char*>(szOID_SUBJECT_KEY_IDENTIFIER), FALSE, {static_cast<DWORD>(identifierData.size()), identifierData.data()}}
        }};
        CERT_EXTENSIONS extensions{static_cast<DWORD>(rootExtensions.size()), rootExtensions.data()}; SYSTEMTIME begin{}, end{};
        FILETIME current{}; GetSystemTimeAsFileTime(&current); auto earlier = shiftTime(current,-300); FileTimeToSystemTime(&earlier,&begin); GetSystemTime(&end); end.wYear += 10;
        if (end.wMonth == 2 && end.wDay == 29) end.wDay = 28;
        root_ = CertCreateSelfSignCertificate(provider, &subject, 0, &keyInfo, &algorithm, &begin, &end, &extensions);
        require(root_ != nullptr, "Create local web root certificate");
        require(CertAddCertificateContextToStore(store, root_, CERT_STORE_ADD_REPLACE_EXISTING, nullptr), "Save local web root certificate");
    }
    // A stable local root signs a fresh leaf with current NIC addresses. Trust
    // the exported root once; DHCP changes do not replace that trust anchor.
    // Keep the server key distinct from the CA key for normal chain building.
    const wchar_t* leafContainer = L"M0NXD.FT710Dashboard.WebTLS.Leaf.v1";
    HCRYPTPROV leafProvider = 0; HCRYPTKEY leafKey = 0;
    require(CryptAcquireContextW(&leafProvider,leafContainer,providerName,PROV_RSA_AES,0) || CryptAcquireContextW(&leafProvider,leafContainer,providerName,PROV_RSA_AES,CRYPT_NEWKEYSET),"Open HTTPS server key");
    KeyCleanup leafCleanup{leafProvider};
    require(CryptGetUserKey(leafProvider,AT_KEYEXCHANGE,&leafKey) || CryptGenKey(leafProvider,AT_KEYEXCHANGE,2048 << 16,&leafKey),"Create HTTPS server key"); leafCleanup.key = leafKey;
    CRYPT_KEY_PROV_INFO leafKeyInfo = keyInfo; leafKeyInfo.pwszContainerName = const_cast<wchar_t*>(leafContainer);
    DWORD leafPublicSize = 0; require(CryptExportPublicKeyInfo(leafProvider,AT_KEYEXCHANGE,X509_ASN_ENCODING,nullptr,&leafPublicSize),"Read HTTPS public key");
    std::vector<BYTE> leafPublicData(leafPublicSize); auto* leafPublicKey = reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(leafPublicData.data());
    require(CryptExportPublicKeyInfo(leafProvider,AT_KEYEXCHANGE,X509_ASN_ENCODING,leafPublicKey,&leafPublicSize),"Read HTTPS public key");
    std::vector<std::wstring> dns; std::vector<std::array<BYTE, 16>> ip; std::vector<DWORD> ipSizes;
    for (const auto& address : addresses) {
        std::array<BYTE, 16> bytes{};
        if (inet_pton(AF_INET, address.c_str(), bytes.data()) == 1) { ip.push_back(bytes); ipSizes.push_back(4); }
        else if (inet_pton(AF_INET6, address.c_str(), bytes.data()) == 1) { ip.push_back(bytes); ipSizes.push_back(16); }
        else dns.emplace_back(address.begin(), address.end());
    }
    std::vector<CERT_ALT_NAME_ENTRY> alternatives;
    for (auto& entry : dns) { CERT_ALT_NAME_ENTRY item{}; item.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME; item.pwszDNSName = entry.data(); alternatives.push_back(item); }
    for (unsigned i = 0; i < ip.size(); ++i) { CERT_ALT_NAME_ENTRY item{}; item.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS; item.IPAddress = {ipSizes[i], ip[i].data()}; alternatives.push_back(item); }
    CERT_ALT_NAME_INFO names{static_cast<DWORD>(alternatives.size()), alternatives.data()}; auto san = encode(X509_ALTERNATE_NAME, &names);
    CERT_BASIC_CONSTRAINTS2_INFO basic{}; auto constraints = encode(X509_BASIC_CONSTRAINTS2, &basic);
    LPSTR usageOid = const_cast<char*>(szOID_PKIX_KP_SERVER_AUTH); CERT_ENHKEY_USAGE usage{1, &usageOid}; auto eku = encode(X509_ENHANCED_KEY_USAGE, &usage);
    BYTE usageBits = CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_ENCIPHERMENT_KEY_USAGE;
    CRYPT_BIT_BLOB keyUsage{1,&usageBits,5}; auto keyUsageData = encode(X509_KEY_USAGE,&keyUsage);
    auto leafIdentifierBytes = hash(leafPublicKey->PublicKey.pbData,leafPublicKey->PublicKey.cbData,BCRYPT_SHA1_ALGORITHM,20);
    CRYPT_DATA_BLOB leafIdentifier{static_cast<DWORD>(leafIdentifierBytes.size()),leafIdentifierBytes.data()}; auto leafIdentifierData = encode(X509_OCTET_STRING,&leafIdentifier);
    auto rootIdentifierBytes = hash(root_->pCertInfo->SubjectPublicKeyInfo.PublicKey.pbData,root_->pCertInfo->SubjectPublicKeyInfo.PublicKey.cbData,BCRYPT_SHA1_ALGORITHM,20);
    CERT_AUTHORITY_KEY_ID2_INFO authority{}; authority.KeyId = {static_cast<DWORD>(rootIdentifierBytes.size()),rootIdentifierBytes.data()}; auto authorityData = encode(X509_AUTHORITY_KEY_ID2,&authority);
    std::array<CERT_EXTENSION, 6> extensions{{
        {const_cast<char*>(szOID_SUBJECT_ALT_NAME2), FALSE, {static_cast<DWORD>(san.size()), san.data()}},
        {const_cast<char*>(szOID_BASIC_CONSTRAINTS2), TRUE, {static_cast<DWORD>(constraints.size()), constraints.data()}},
        {const_cast<char*>(szOID_ENHANCED_KEY_USAGE), FALSE, {static_cast<DWORD>(eku.size()), eku.data()}},
        {const_cast<char*>(szOID_KEY_USAGE), TRUE, {static_cast<DWORD>(keyUsageData.size()), keyUsageData.data()}},
        {const_cast<char*>(szOID_SUBJECT_KEY_IDENTIFIER), FALSE, {static_cast<DWORD>(leafIdentifierData.size()),leafIdentifierData.data()}},
        {const_cast<char*>(szOID_AUTHORITY_KEY_IDENTIFIER2), FALSE, {static_cast<DWORD>(authorityData.size()),authorityData.data()}}
    }};
    auto subject = name(L"CN=FT710 Dashboard"); auto serial = hash(addresses.front().data(), addresses.front().size(), BCRYPT_SHA256_ALGORITHM, 32);
    require(BCryptGenRandom(nullptr, serial.data(), 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "Create certificate serial"); serial[15] &= 0x7f;
    CERT_INFO info{}; info.dwVersion = CERT_V3; info.SerialNumber = {16, serial.data()}; info.SignatureAlgorithm = algorithm;
    info.Issuer = root_->pCertInfo->Subject; info.Subject = {static_cast<DWORD>(subject.size()), subject.data()}; info.SubjectPublicKeyInfo = *leafPublicKey;
    FILETIME current{}; GetSystemTimeAsFileTime(&current); info.NotBefore = shiftTime(current,-300); info.NotAfter = shiftTime(current,365LL * 86400);
    if (CompareFileTime(&info.NotBefore,&root_->pCertInfo->NotBefore) < 0) info.NotBefore = root_->pCertInfo->NotBefore;
    if (CompareFileTime(&info.NotAfter,&root_->pCertInfo->NotAfter) > 0) info.NotAfter = root_->pCertInfo->NotAfter;
    info.cExtension = extensions.size(); info.rgExtension = extensions.data();
    DWORD bytes = 0; require(CryptSignAndEncodeCertificate(provider, AT_KEYEXCHANGE, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &info, &algorithm, nullptr, nullptr, &bytes), "Sign web certificate");
    std::vector<BYTE> encoded(bytes); require(CryptSignAndEncodeCertificate(provider, AT_KEYEXCHANGE, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &info, &algorithm, nullptr, encoded.data(), &bytes), "Sign web certificate");
    leaf_ = CertCreateCertificateContext(X509_ASN_ENCODING, encoded.data(), bytes); require(leaf_ != nullptr, "Load web certificate");
    require(CertSetCertificateContextProperty(leaf_, CERT_KEY_PROV_INFO_PROP_ID, 0, &leafKeyInfo), "Attach web private key");
    certificate.assign(reinterpret_cast<const char*>(root_->pbCertEncoded), root_->cbCertEncoded);
    auto digest = hash(certificate.data(), certificate.size(), BCRYPT_SHA256_ALGORITHM, 32); fingerprint = hex(digest.data(), digest.size());
    appleProfile = certificateProfile(certificate,fingerprint);
    std::filesystem::create_directories(folder);
    std::ofstream exported{std::filesystem::path(folder) / L"web-certificate.cer", std::ios::binary}; exported.write(certificate.data(), certificate.size()); require(static_cast<bool>(exported), "Export web certificate");
    std::ofstream profileExport{std::filesystem::path(folder) / L"ft710-station.mobileconfig", std::ios::binary}; profileExport.write(appleProfile.data(),appleProfile.size()); require(static_cast<bool>(profileExport),"Export iPhone certificate profile");
    SCHANNEL_CRED credentials{}; credentials.dwVersion = SCHANNEL_CRED_VERSION; credentials.cCreds = 1; credentials.paCred = &leaf_;
    credentials.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER; credentials.dwFlags = SCH_CRED_NO_DEFAULT_CREDS;
    TimeStamp expiry{}; auto result = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_INBOUND, nullptr, &credentials, nullptr, nullptr, &handle, &expiry);
    if (result != SEC_E_OK) throw std::runtime_error("Initialize HTTPS credentials (Schannel " + std::to_string(static_cast<unsigned long>(result)) + ")");
    ready_ = true;
}
Stream::~Stream() { if (contextReady_) DeleteSecurityContext(&context_); }
bool Stream::rawSend(const char* data, unsigned size) {
    for (unsigned sent = 0; sent < size;) { int count = ::send(socket_, data + sent, size - sent, 0); if (count <= 0) return false; sent += count; } return true;
}
int Stream::receive(int timeoutMs) {
    fd_set readable; FD_ZERO(&readable); FD_SET(socket_, &readable); timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int ready = select(0, &readable, nullptr, nullptr, &timeout); if (ready <= 0) return ready < 0 ? -1 : 0;
    char buffer[16384]; int bytes = recv(socket_, buffer, sizeof buffer, 0); if (bytes <= 0) return -1;
    encrypted_.append(buffer, bytes); return encrypted_.size() > 131072 ? -1 : 1;
}
bool Stream::negotiate(Credentials& credentials) {
    auto deadline = GetTickCount64() + 10000;
    auto failure = [this](const std::string& message) { error_ = message; return false; };
    while (encrypted_.empty()) if (receive(1000) < 0 || GetTickCount64() > deadline) return false;
    if (static_cast<unsigned char>(encrypted_[0]) != 22) return true; // Plain HTTP offers certificate/setup only.
    while (GetTickCount64() < deadline) {
        SecBuffer input[2]{{static_cast<ULONG>(encrypted_.size()), SECBUFFER_TOKEN, encrypted_.data()}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBuffer output[2]{{0, SECBUFFER_TOKEN, nullptr}, {0, SECBUFFER_ALERT, nullptr}};
        SecBufferDesc in{SECBUFFER_VERSION, 2, input}, out{SECBUFFER_VERSION, 2, output}; ULONG attributes = 0;
        auto status = AcceptSecurityContext(&credentials.handle, contextReady_ ? &context_ : nullptr, &in,
            ASC_REQ_STREAM | ASC_REQ_CONFIDENTIALITY | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_EXTENDED_ERROR | ASC_REQ_REPLAY_DETECT | ASC_REQ_SEQUENCE_DETECT,
            SECURITY_NATIVE_DREP, &context_, &out, &attributes, nullptr);
        if (status != SEC_E_INCOMPLETE_MESSAGE && (status >= 0 || SecIsValidHandle(&context_))) contextReady_ = true;
        bool sent = true;
        if (output[0].pvBuffer) { sent = rawSend(static_cast<char*>(output[0].pvBuffer), output[0].cbBuffer); FreeContextBuffer(output[0].pvBuffer); }
        if (output[1].pvBuffer) FreeContextBuffer(output[1].pvBuffer);
        if (!sent) return failure("client disconnected during TLS negotiation");
        if (status == SEC_E_INCOMPLETE_MESSAGE) { if (receive(1000) < 0) return failure("client disconnected during TLS negotiation"); continue; }
        unsigned extra = input[1].BufferType == SECBUFFER_EXTRA ? input[1].cbBuffer : 0;
        encrypted_ = extra ? encrypted_.substr(encrypted_.size() - extra) : std::string{};
        if (status == SEC_E_OK) {
            if (!(attributes & ASC_RET_CONFIDENTIALITY)) return failure("TLS confidentiality was not negotiated");
            secure_ = QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_) == SEC_E_OK;
            return secure_ || failure("could not obtain TLS record sizes");
        }
        if (status != SEC_I_CONTINUE_NEEDED) return failure("Schannel status " + std::to_string(static_cast<unsigned long>(status)));
        if (encrypted_.empty() && receive(1000) < 0) return failure("client disconnected during TLS negotiation; check station certificate trust");
    }
    return failure("TLS negotiation timed out");
}
int Stream::read(std::string& data, int timeoutMs) {
    if (!secure_) {
        if (encrypted_.empty()) { int result = receive(timeoutMs); if (result <= 0) return result; }
        data += encrypted_; encrypted_.clear(); return 1;
    }
    bool attempted = false; auto deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (encrypted_.empty() || attempted) {
            auto now = GetTickCount64(); if (now >= deadline) return 0;
            int result = receive(static_cast<int>(deadline - now)); if (result <= 0) return result;
        }
        SecBuffer buffers[4]{{static_cast<ULONG>(encrypted_.size()), SECBUFFER_DATA, encrypted_.data()}, {0,SECBUFFER_EMPTY,nullptr}, {0,SECBUFFER_EMPTY,nullptr}, {0,SECBUFFER_EMPTY,nullptr}};
        SecBufferDesc message{SECBUFFER_VERSION, 4, buffers}; auto status = DecryptMessage(&context_, &message, 0, nullptr);
        if (status == SEC_E_INCOMPLETE_MESSAGE) { attempted = true; continue; }
        if (status != SEC_E_OK) return -1; // Close on renegotiation or peer close.
        unsigned extra = 0;
        for (const auto& buffer : buffers) {
            if (buffer.BufferType == SECBUFFER_DATA) data.append(static_cast<const char*>(buffer.pvBuffer), buffer.cbBuffer);
            if (buffer.BufferType == SECBUFFER_EXTRA) extra = buffer.cbBuffer;
        }
        encrypted_ = extra ? encrypted_.substr(encrypted_.size() - extra) : std::string{};
        return 1;
    }
}
bool Stream::send(const std::string& data) {
    if (!secure_) return rawSend(data.data(), data.size());
    for (std::size_t offset = 0; offset < data.size();) {
        unsigned count = static_cast<unsigned>(std::min<std::size_t>(sizes_.cbMaximumMessage, data.size() - offset));
        std::vector<char> record(sizes_.cbHeader + count + sizes_.cbTrailer);
        std::memcpy(record.data() + sizes_.cbHeader, data.data() + offset, count);
        SecBuffer buffers[4]{{sizes_.cbHeader,SECBUFFER_STREAM_HEADER,record.data()}, {count,SECBUFFER_DATA,record.data() + sizes_.cbHeader}, {sizes_.cbTrailer,SECBUFFER_STREAM_TRAILER,record.data() + sizes_.cbHeader + count}, {0,SECBUFFER_EMPTY,nullptr}};
        SecBufferDesc message{SECBUFFER_VERSION,4,buffers}; if (EncryptMessage(&context_, 0, &message, 0) != SEC_E_OK) return false;
        if (!rawSend(record.data(), buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer)) return false;
        offset += count;
    }
    return true;
}
}
