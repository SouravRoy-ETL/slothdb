#pragma once

#include "slothdb/ask/nl_to_sql.hpp"
#include <string>

namespace slothdb {
namespace ask {

enum class Provider {
    None,       // no LLM available / not configured
    Ollama,     // localhost Ollama HTTP API
    OpenAI,     // api.openai.com (HTTPS — Windows only right now)
    Anthropic,  // api.anthropic.com (HTTPS — Windows only right now)
};

struct LLMConfig {
    Provider provider = Provider::None;
    std::string host = "localhost:11434"; // for Ollama
    std::string model;                    // e.g. "llama3:8b-instruct"
    std::string api_key;                  // for OpenAI / Anthropic
    std::string reason;                   // why provider is None, if so
};

// Read SLOTHDB_ASK_PROVIDER / SLOTHDB_ASK_MODEL / SLOTHDB_ASK_HOST +
// OPENAI_API_KEY / ANTHROPIC_API_KEY and pick the first provider that
// looks reachable. Never throws.
LLMConfig ConfigFromEnvironment();

struct LLMResult {
    bool ok = false;
    std::string sql;       // generated query (stripped of markdown)
    std::string message;   // provider error, if not ok
};

// Render a prompt from the schema + question, POST to the provider,
// parse the SQL out of the response. No side effects beyond the
// HTTP call. Caller still owns validation + [Y/n] + execution.
LLMResult GenerateSQL(const LLMConfig &cfg, const Schema &schema,
                     const std::string &question);

} // namespace ask
} // namespace slothdb
