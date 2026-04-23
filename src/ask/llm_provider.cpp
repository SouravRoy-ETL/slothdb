#include "slothdb/ask/llm_provider.hpp"
#include "slothdb/storage/http_client.hpp"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <string>

namespace slothdb {
namespace ask {

#ifdef SLOTHDB_EDGE
// Edge builds (Cloudflare Workers / browser WASM) can't make outbound
// HTTP calls to a user's Ollama / OpenAI anyway, so compile .ask --ai
// out entirely. Rules-only NL stays available.
LLMConfig ConfigFromEnvironment() { LLMConfig c; c.reason = "edge build — AI fallback disabled"; return c; }
LLMResult GenerateSQL(const LLMConfig&, const Schema&, const std::string&) {
    LLMResult r; r.message = "edge build — AI fallback disabled"; return r;
}
} // namespace ask
} // namespace slothdb
#else
// (Non-edge implementation follows.)

namespace {

// Cross-platform getenv — MSVC deprecates getenv, so route through _dupenv_s.
std::string GetEnv(const char *name) {
#ifdef _MSC_VER
    char *buf = nullptr; size_t sz = 0;
    if (_dupenv_s(&buf, &sz, name) != 0 || !buf) return "";
    std::string v(buf);
    free(buf);
    return v;
#else
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

// Render a DDL-style schema description for the prompt. Only the first
// ~20 tables to cap prompt size on large catalogs — the realistic case
// is a few tables in a shell session anyway.
std::string RenderDDL(const Schema &schema) {
    std::ostringstream ss;
    size_t limit = schema.tables.size() < 20 ? schema.tables.size() : 20;
    for (size_t i = 0; i < limit; i++) {
        const auto &t = schema.tables[i];
        ss << "CREATE TABLE " << t.name << " (";
        for (size_t j = 0; j < t.columns.size(); j++) {
            if (j > 0) ss << ", ";
            ss << t.columns[j].name << " " << t.columns[j].type;
        }
        ss << ");\n";
    }
    return ss.str();
}

// Minimal JSON string escape — enough for LLM prompts where most text
// is ASCII with occasional backslashes or quotes. Not a general-purpose
// JSON encoder.
std::string JsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned>(c) & 0xff);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// Yank a string-valued field from a flat JSON object, no escapes handled
// in the value beyond the common ones. Works for Ollama's `{"response":
// "..."}` and OpenAI's `{"choices":[{"message":{"content":"..."}}]}`
// if you pass the key like `"content"`.
std::string ExtractJsonString(const std::string &json, const std::string &key) {
    std::string needle = "\"" + key + "\":";
    size_t p = json.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    if (p >= json.size() || json[p] != '"') return "";
    p++; // past opening quote
    std::string out;
    while (p < json.size()) {
        char c = json[p++];
        if (c == '"') return out;
        if (c == '\\' && p < json.size()) {
            char n = json[p++];
            switch (n) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'u': {
                if (p + 4 > json.size()) return out;
                // Only handle ASCII range cleanly.
                unsigned code = 0;
                for (int i = 0; i < 4; i++) {
                    char h = json[p++];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= (h - '0');
                    else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                }
                if (code < 0x80) out += static_cast<char>(code);
                // Multi-byte UTF-8 left for future work; rare in SQL.
                break;
            }
            default: out += n;
            }
        } else {
            out += c;
        }
    }
    return out;
}

// Strip markdown fences + leading/trailing whitespace + leading comments,
// pull out just the SQL. LLMs often return ```sql ... ``` blocks or
// prose around the SQL — we want the statement and nothing else.
std::string ExtractSQL(const std::string &text) {
    std::string s = text;
    // Prefer a fenced code block if present.
    auto fence = s.find("```");
    if (fence != std::string::npos) {
        size_t start = fence + 3;
        // Skip optional language tag ('sql\n').
        while (start < s.size() && s[start] != '\n') start++;
        if (start < s.size()) start++;
        auto end = s.find("```", start);
        if (end != std::string::npos) {
            s = s.substr(start, end - start);
        }
    }
    // Trim whitespace.
    auto is_ws = [](char c){ return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    // Drop trailing semicolons (the shell's query path adds its own).
    while (!s.empty() && s.back() == ';') s.pop_back();
    return s;
}

} // namespace

LLMConfig ConfigFromEnvironment() {
    LLMConfig cfg;
    std::string provider = GetEnv("SLOTHDB_ASK_PROVIDER");
    // Lowercase compare.
    for (auto &c : provider) c = static_cast<char>(std::tolower(
        static_cast<unsigned char>(c)));

    std::string model = GetEnv("SLOTHDB_ASK_MODEL");
    std::string host  = GetEnv("SLOTHDB_ASK_HOST");

    if (provider == "ollama" || (provider.empty() && !model.empty())) {
        cfg.provider = Provider::Ollama;
        cfg.host = host.empty() ? std::string("localhost:11434") : host;
        cfg.model = model.empty() ? std::string("llama3:8b-instruct") : model;
        return cfg;
    }
    if (provider == "openai") {
        std::string key = GetEnv("OPENAI_API_KEY");
        if (key.empty()) {
            cfg.reason = "SLOTHDB_ASK_PROVIDER=openai but OPENAI_API_KEY not set";
            return cfg;
        }
        cfg.provider = Provider::OpenAI;
        cfg.host = "api.openai.com";
        cfg.model = model.empty() ? std::string("gpt-4o-mini") : model;
        cfg.api_key = key;
        return cfg;
    }
    if (provider == "anthropic") {
        std::string key = GetEnv("ANTHROPIC_API_KEY");
        if (key.empty()) {
            cfg.reason = "SLOTHDB_ASK_PROVIDER=anthropic but ANTHROPIC_API_KEY not set";
            return cfg;
        }
        cfg.provider = Provider::Anthropic;
        cfg.host = "api.anthropic.com";
        cfg.model = model.empty() ? std::string("claude-3-5-haiku-20241022") : model;
        cfg.api_key = key;
        return cfg;
    }

    // Auto-detect: if no provider set but common env markers exist, guess.
    if (provider.empty()) {
        if (!GetEnv("OPENAI_API_KEY").empty()) {
            cfg.provider = Provider::OpenAI;
            cfg.host = "api.openai.com";
            cfg.model = model.empty() ? std::string("gpt-4o-mini") : model;
            cfg.api_key = GetEnv("OPENAI_API_KEY");
            return cfg;
        }
        // Default: assume Ollama might be running locally.
        cfg.provider = Provider::Ollama;
        cfg.host = "localhost:11434";
        cfg.model = "llama3:8b-instruct";
        cfg.reason = "(auto-detected Ollama default — may fail if not running)";
        return cfg;
    }

    cfg.reason = "SLOTHDB_ASK_PROVIDER='" + provider + "' not recognized";
    return cfg;
}

LLMResult GenerateSQL(const LLMConfig &cfg, const Schema &schema,
                     const std::string &question) {
    LLMResult r;
    if (cfg.provider == Provider::None) {
        r.message = cfg.reason.empty()
                        ? "no LLM provider configured (set SLOTHDB_ASK_PROVIDER)"
                        : cfg.reason;
        return r;
    }

    const std::string ddl = RenderDDL(schema);
    const std::string system_msg =
        "You are a SQL generator for SlothDB (DuckDB-compatible dialect). "
        "Given the schema below and a natural-language question, output "
        "ONE SQL SELECT statement that answers the question. No commentary, "
        "no markdown unless you fence the SQL in ```sql blocks. Use only "
        "tables and columns from the schema. Prefer explicit column names "
        "over SELECT *.\n\nSchema:\n" + ddl;

    if (cfg.provider == Provider::Ollama) {
        std::string body = "{\"model\":\"" + JsonEscape(cfg.model) +
                           "\",\"prompt\":\"" +
                           JsonEscape(system_msg + "\n\nQuestion: " + question) +
                           "\",\"stream\":false}";
        std::string url = "http://" + cfg.host + "/api/generate";
        auto resp = HTTPClient::Post(url, body);
        if (!resp.success) {
            r.message = "Ollama call failed (" +
                        std::to_string(resp.status_code) + "): " + resp.error;
            if (!resp.body.empty()) r.message += "\n  Body: " + resp.body.substr(0, 200);
            return r;
        }
        std::string text = ExtractJsonString(resp.body, "response");
        if (text.empty()) {
            r.message = "Ollama returned no 'response' field. Raw: " +
                        resp.body.substr(0, 200);
            return r;
        }
        r.sql = ExtractSQL(text);
        if (r.sql.empty()) {
            r.message = "Model returned empty SQL after stripping markdown";
            return r;
        }
        r.ok = true;
        return r;
    }

    if (cfg.provider == Provider::OpenAI) {
        std::string body =
            "{\"model\":\"" + JsonEscape(cfg.model) + "\","
            "\"messages\":["
              "{\"role\":\"system\",\"content\":\"" + JsonEscape(system_msg) + "\"},"
              "{\"role\":\"user\",\"content\":\"" + JsonEscape(question) + "\"}"
            "],\"temperature\":0}";
        std::string url = "https://" + cfg.host + "/v1/chat/completions";
        auto resp = HTTPClient::Post(url, body, "application/json",
            {"Authorization: Bearer " + cfg.api_key});
        if (!resp.success) {
            r.message = "OpenAI call failed (" +
                        std::to_string(resp.status_code) + "): " + resp.error;
            if (!resp.body.empty()) r.message += "\n  Body: " + resp.body.substr(0, 300);
            return r;
        }
        std::string text = ExtractJsonString(resp.body, "content");
        if (text.empty()) {
            r.message = "OpenAI returned no 'content' field. Raw: " +
                        resp.body.substr(0, 300);
            return r;
        }
        r.sql = ExtractSQL(text);
        if (r.sql.empty()) { r.message = "Empty SQL after extraction"; return r; }
        r.ok = true;
        return r;
    }

    if (cfg.provider == Provider::Anthropic) {
        std::string body =
            "{\"model\":\"" + JsonEscape(cfg.model) + "\","
            "\"max_tokens\":1024,"
            "\"system\":\"" + JsonEscape(system_msg) + "\","
            "\"messages\":["
              "{\"role\":\"user\",\"content\":\"" + JsonEscape(question) + "\"}"
            "]}";
        std::string url = "https://" + cfg.host + "/v1/messages";
        auto resp = HTTPClient::Post(url, body, "application/json",
            {"x-api-key: " + cfg.api_key,
             "anthropic-version: 2023-06-01"});
        if (!resp.success) {
            r.message = "Anthropic call failed (" +
                        std::to_string(resp.status_code) + "): " + resp.error;
            if (!resp.body.empty()) r.message += "\n  Body: " + resp.body.substr(0, 300);
            return r;
        }
        std::string text = ExtractJsonString(resp.body, "text");
        if (text.empty()) {
            r.message = "Anthropic returned no 'text' field. Raw: " +
                        resp.body.substr(0, 300);
            return r;
        }
        r.sql = ExtractSQL(text);
        if (r.sql.empty()) { r.message = "Empty SQL after extraction"; return r; }
        r.ok = true;
        return r;
    }

    r.message = "unknown provider";
    return r;
}

} // namespace ask
} // namespace slothdb
#endif // SLOTHDB_EDGE
