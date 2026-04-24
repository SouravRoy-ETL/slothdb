#pragma once

#include "slothdb/ask/nl_to_sql.hpp"
#include <string>

namespace slothdb {
namespace ask {

struct EmbeddedResult {
    bool ok = false;
    std::string sql;        // generated SQL, markdown-stripped
    std::string message;    // error explanation when !ok
    std::string message_verbose; // download progress / inference notes
};

// Where the default model GGUF lives on disk. Created lazily.
std::string ModelDir();
std::string DefaultModelPath();

// The model we ship by default when `.ask --model` is used. Qwen2.5-
// Coder-0.5B-Instruct quantized to Q4_K_M on HuggingFace - ~310 MB on
// disk, Apache 2.0 license. Pinned by SHA256.
struct ModelSpec {
    const char *name;          // human-readable
    const char *url;           // download URL
    const char *sha256;        // lowercase hex
    const char *local_file;    // filename under ~/.slothdb/models/
    const char *chat_template; // "qwen" / "llama3" / "phi" etc.
    size_t expected_bytes;     // sanity check for download progress
};
const ModelSpec &DefaultModel();

// True iff SLOTHDB_ASK_MODEL=ON was set at build time AND third_party/
// llama.cpp was present. Consumers call this before offering --model.
bool EmbeddedAvailable();

// Ensure the default model is present on disk. Downloads via our
// HTTPClient on miss. Streams progress to stderr when verbose.
// Returns false with a reason on failure.
bool EnsureModelDownloaded(bool verbose, std::string &err);

// Generate SQL from the schema + question using the local model.
// Blocking call. Single response, not streaming (output size is small).
EmbeddedResult GenerateSQLLocal(const Schema &schema,
                                const std::string &question);

} // namespace ask
} // namespace slothdb
