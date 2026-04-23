#include "slothdb/storage/http_client.hpp"
#include "slothdb/common/exception.hpp"
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _MSC_VER
// Windows: use WinHTTP for HTTPS support.
#include <windows.h>
#undef GetObject
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#elif defined(__EMSCRIPTEN__)
// WASM: HTTP not supported. Playground loads files from MEMFS instead.
#else
// POSIX: use raw sockets for HTTP.
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#endif

namespace slothdb {

HTTPClient::URLParts HTTPClient::ParseURL(const std::string &url) {
    URLParts parts;
    size_t pos = 0;

    // Scheme.
    auto scheme_end = url.find("://");
    if (scheme_end != std::string::npos) {
        parts.scheme = url.substr(0, scheme_end);
        pos = scheme_end + 3;
    } else {
        parts.scheme = "http";
    }

    parts.port = (parts.scheme == "https") ? 443 : 80;

    // Host (and optional port).
    auto path_start = url.find('/', pos);
    auto host_str = url.substr(pos, path_start - pos);
    auto colon = host_str.find(':');
    if (colon != std::string::npos) {
        parts.host = host_str.substr(0, colon);
        parts.port = std::stoi(host_str.substr(colon + 1));
    } else {
        parts.host = host_str;
    }

    // Path.
    if (path_start != std::string::npos) {
        parts.path = url.substr(path_start);
    } else {
        parts.path = "/";
    }

    return parts;
}

#ifdef _MSC_VER

HTTPResponse HTTPClient::DoWinHTTPGet(const std::string &url) {
    HTTPResponse response;

    // Convert URL to wide string.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wurl(wlen);
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl.data(), wlen);

    // Crack URL.
    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwSchemeLength = (DWORD)-1;
    urlComp.dwHostNameLength = (DWORD)-1;
    urlComp.dwUrlPathLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.data(), 0, 0, &urlComp)) {
        response.error = "Failed to parse URL";
        return response;
    }

    std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
    std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
    bool is_https = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(L"SlothDB/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        response.error = "WinHttpOpen failed";
        return response;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
        urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        response.error = "WinHttpConnect failed";
        return response;
    }

    DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
        path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.error = "WinHttpOpenRequest failed";
        return response;
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.error = "WinHttpSendRequest failed";
        return response;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.error = "WinHttpReceiveResponse failed";
        return response;
    }

    // Read status code.
    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
        WINHTTP_NO_HEADER_INDEX);
    response.status_code = static_cast<int>(status);

    // Read body.
    DWORD bytes_available;
    while (WinHttpQueryDataAvailable(hRequest, &bytes_available) && bytes_available > 0) {
        std::vector<char> buf(bytes_available);
        DWORD bytes_read = 0;
        WinHttpReadData(hRequest, buf.data(), bytes_available, &bytes_read);
        response.body.append(buf.data(), bytes_read);
    }

    response.success = (response.status_code >= 200 && response.status_code < 300);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

#endif

HTTPResponse HTTPClient::DoHTTPGet(const std::string &host, int port,
                                     const std::string &path) {
    HTTPResponse response;

#ifdef _MSC_VER
    // On Windows, use WinHTTP instead of raw sockets.
    response = DoWinHTTPGet("http://" + host + ":" + std::to_string(port) + path);
#elif defined(__EMSCRIPTEN__)
    (void)host; (void)port; (void)path;
    response.error = "HTTP not supported in WASM build; load files from the browser instead";
#else
    // POSIX socket-based HTTP.
    struct hostent *server = gethostbyname(host.c_str());
    if (!server) {
        response.error = "Cannot resolve host: " + host;
        return response;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        response.error = "Cannot create socket";
        return response;
    }

    struct sockaddr_in serv_addr = {};
    serv_addr.sin_family = AF_INET;
    std::memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(static_cast<uint16_t>(port));

    if (connect(sockfd, reinterpret_cast<struct sockaddr *>(&serv_addr),
                sizeof(serv_addr)) < 0) {
        close(sockfd);
        response.error = "Cannot connect to " + host;
        return response;
    }

    // Validate host and path against header injection.
    if (host.find('\r') != std::string::npos || host.find('\n') != std::string::npos ||
        path.find('\r') != std::string::npos || path.find('\n') != std::string::npos) {
        close(sockfd);
        response.error = "Invalid characters in URL (possible header injection)";
        return response;
    }

    // Send HTTP request.
    std::string request = "GET " + path + " HTTP/1.1\r\n"
                          "Host: " + host + "\r\n"
                          "Connection: close\r\n"
                          "User-Agent: SlothDB/0.1\r\n\r\n";
    send(sockfd, request.c_str(), request.size(), 0);

    // Read response (with size limit).
    static constexpr size_t MAX_RESPONSE_SIZE = 100 * 1024 * 1024; // 100 MB
    char buf[4096];
    std::string raw_response;
    while (true) {
        auto n = recv(sockfd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw_response.append(buf, n);
        if (raw_response.size() > MAX_RESPONSE_SIZE) {
            close(sockfd);
            response.error = "Response exceeds maximum size (100 MB)";
            return response;
        }
    }
    close(sockfd);

    // Parse HTTP response.
    auto header_end = raw_response.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        auto status_line = raw_response.substr(0, raw_response.find("\r\n"));
        auto space1 = status_line.find(' ');
        auto space2 = status_line.find(' ', space1 + 1);
        if (space1 != std::string::npos) {
            response.status_code = std::stoi(
                status_line.substr(space1 + 1, space2 - space1 - 1));
        }
        response.body = raw_response.substr(header_end + 4);
        response.success = (response.status_code >= 200 && response.status_code < 300);
    }
#endif

    return response;
}

HTTPResponse HTTPClient::Get(const std::string &url) {
#ifdef _MSC_VER
    return DoWinHTTPGet(url);
#elif defined(__EMSCRIPTEN__)
    (void)url;
    HTTPResponse r;
    r.error = "HTTP not supported in WASM build; load files from the browser instead";
    return r;
#else
    auto parts = ParseURL(url);
    if (parts.scheme == "https") {
        HTTPResponse r;
        r.error = "HTTPS not supported on this platform without OpenSSL";
        return r;
    }
    return DoHTTPGet(parts.host, parts.port, parts.path);
#endif
}

// POST a body to `url` with the given content-type and extra headers.
// Used by `.ask --ai` for LLM calls; supports localhost HTTP everywhere
// and HTTPS on Windows (WinHTTP). On POSIX, HTTPS fails clean — the
// user is expected to run a local Ollama on HTTP for the default flow.
HTTPResponse HTTPClient::Post(const std::string &url,
                               const std::string &body,
                               const std::string &content_type,
                               const std::vector<std::string> &extra_headers) {
    HTTPResponse response;

#ifdef _MSC_VER
    // WinHTTP path — supports both HTTP and HTTPS.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wurl(wlen);
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl.data(), wlen);

    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwSchemeLength = (DWORD)-1;
    urlComp.dwHostNameLength = (DWORD)-1;
    urlComp.dwUrlPathLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.data(), 0, 0, &urlComp)) {
        response.error = "Failed to parse URL"; return response;
    }
    std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
    std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
    bool is_https = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(L"SlothDB/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { response.error = "WinHttpOpen failed"; return response; }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
        urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        response.error = "WinHttpConnect failed"; return response;
    }

    DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST",
        path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.error = "WinHttpOpenRequest failed"; return response;
    }

    // Assemble header block — Content-Type + any extras.
    std::wstring hdrs = L"Content-Type: ";
    for (char c : content_type) hdrs += static_cast<wchar_t>(c);
    hdrs += L"\r\n";
    for (const auto &h : extra_headers) {
        for (char c : h) hdrs += static_cast<wchar_t>(c);
        hdrs += L"\r\n";
    }

    if (!WinHttpSendRequest(hRequest, hdrs.c_str(),
            static_cast<DWORD>(-1L),
            const_cast<char *>(body.data()),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()), 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.error = "WinHttpSendRequest failed"; return response;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.error = "WinHttpReceiveResponse failed"; return response;
    }

    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
        WINHTTP_NO_HEADER_INDEX);
    response.status_code = static_cast<int>(status);

    DWORD bytes_available;
    while (WinHttpQueryDataAvailable(hRequest, &bytes_available) && bytes_available > 0) {
        std::vector<char> buf(bytes_available);
        DWORD bytes_read = 0;
        WinHttpReadData(hRequest, buf.data(), bytes_available, &bytes_read);
        response.body.append(buf.data(), bytes_read);
    }

    response.success = (response.status_code >= 200 && response.status_code < 300);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;

#elif defined(__EMSCRIPTEN__)
    (void)url; (void)body; (void)content_type; (void)extra_headers;
    response.error = "HTTP not supported in WASM build";
    return response;
#else
    // POSIX socket POST. HTTP only — HTTPS needs OpenSSL which we don't link.
    auto parts = ParseURL(url);
    if (parts.scheme == "https") {
        response.error = "HTTPS POST not supported on this platform without OpenSSL. "
                         "Use a local HTTP Ollama (SLOTHDB_ASK_PROVIDER=ollama) "
                         "or run on Windows where WinHTTP handles TLS.";
        return response;
    }

    struct hostent *server = gethostbyname(parts.host.c_str());
    if (!server) { response.error = "Cannot resolve host: " + parts.host; return response; }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { response.error = "Cannot create socket"; return response; }

    struct sockaddr_in serv_addr = {};
    serv_addr.sin_family = AF_INET;
    std::memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(static_cast<uint16_t>(parts.port));

    if (connect(sockfd, reinterpret_cast<struct sockaddr *>(&serv_addr),
                sizeof(serv_addr)) < 0) {
        close(sockfd);
        response.error = "Cannot connect to " + parts.host;
        return response;
    }

    std::string request = "POST " + parts.path + " HTTP/1.1\r\n"
                          "Host: " + parts.host + "\r\n"
                          "Content-Type: " + content_type + "\r\n"
                          "Content-Length: " + std::to_string(body.size()) + "\r\n"
                          "Connection: close\r\n"
                          "User-Agent: SlothDB/0.1\r\n";
    for (const auto &h : extra_headers) request += h + "\r\n";
    request += "\r\n";
    request += body;
    send(sockfd, request.c_str(), request.size(), 0);

    static constexpr size_t MAX_RESPONSE_SIZE = 100 * 1024 * 1024;
    char buf[4096];
    std::string raw_response;
    while (true) {
        auto n = recv(sockfd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw_response.append(buf, n);
        if (raw_response.size() > MAX_RESPONSE_SIZE) {
            close(sockfd);
            response.error = "Response exceeds maximum size (100 MB)";
            return response;
        }
    }
    close(sockfd);

    auto header_end = raw_response.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        auto status_line = raw_response.substr(0, raw_response.find("\r\n"));
        auto space1 = status_line.find(' ');
        auto space2 = status_line.find(' ', space1 + 1);
        if (space1 != std::string::npos) {
            response.status_code = std::stoi(
                status_line.substr(space1 + 1, space2 - space1 - 1));
        }
        response.body = raw_response.substr(header_end + 4);
        response.success = (response.status_code >= 200 && response.status_code < 300);
    }
    return response;
#endif
}

bool HTTPClient::DownloadToFile(const std::string &url, const std::string &local_path) {
    auto response = Get(url);
    if (!response.success) return false;

    std::ofstream file(local_path, std::ios::binary);
    if (!file.is_open()) return false;
    file.write(response.body.data(), response.body.size());
    return true;
}

// ============================================================================
// S3 Client
// ============================================================================

std::string S3Client::S3ToHTTPS(const std::string &s3_url) {
    // s3://bucket/key -> https://bucket.s3.amazonaws.com/key
    if (s3_url.substr(0, 5) != "s3://") return s3_url;
    auto rest = s3_url.substr(5);
    auto slash = rest.find('/');
    if (slash == std::string::npos) return "https://" + rest + ".s3.amazonaws.com/";
    auto bucket = rest.substr(0, slash);
    auto key = rest.substr(slash);
    return "https://" + bucket + ".s3.amazonaws.com" + key;
}

HTTPResponse S3Client::GetObject(const std::string &s3_url) {
    auto https_url = S3ToHTTPS(s3_url);
    return HTTPClient::Get(https_url);
}

} // namespace slothdb
