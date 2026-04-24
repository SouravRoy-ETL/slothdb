#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace slothdb {

struct HTTPResponse {
    int status_code = 0;
    std::string body;
    std::string error;
    bool success = false;
};

// Minimal zero-dependency HTTP client.
// Supports HTTP (not HTTPS - TLS requires OpenSSL or platform API).
// For HTTPS, we'll use platform-specific APIs where available.
class HTTPClient {
public:
    // Download a URL to memory.
    static HTTPResponse Get(const std::string &url);

    // POST a body with a given content-type. Used by `.ask --ai` to call
    // a local Ollama server (or any HTTP-speaking LLM endpoint). HTTPS
    // is supported on Windows via WinHTTP; POSIX currently supports HTTP
    // only - sufficient for the default (localhost Ollama) use case.
    static HTTPResponse Post(const std::string &url,
                             const std::string &body,
                             const std::string &content_type = "application/json",
                             const std::vector<std::string> &extra_headers = {});

    // Download a URL to a local file.
    static bool DownloadToFile(const std::string &url, const std::string &local_path);

    // Parse a URL into components.
    struct URLParts {
        std::string scheme;   // http or https
        std::string host;
        int port = 80;
        std::string path;
    };
    static URLParts ParseURL(const std::string &url);

private:
    static HTTPResponse DoHTTPGet(const std::string &host, int port,
                                   const std::string &path);
#ifdef _MSC_VER
    static HTTPResponse DoWinHTTPGet(const std::string &url);
#endif
};

// S3 client (simplified - anonymous access to public buckets).
class S3Client {
public:
    // Download an S3 object. URL format: s3://bucket/key
    static HTTPResponse GetObject(const std::string &s3_url);

    // Convert s3:// URL to HTTPS URL.
    static std::string S3ToHTTPS(const std::string &s3_url);
};

} // namespace slothdb
