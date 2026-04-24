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

// The two tiers always exist. SmallModel() is ~310 MB, fast to download,
// good for COUNT / GROUP BY / filter / TOP-N. LargeModel() is ~986 MB,
// better at window functions / joins / LAG-LEAD / complex analytic SQL.
// The router (PickModelForQuestion) chooses at inference time; both are
// downloaded in parallel on first .ask so analytic questions don't
// suffer a cold download when they arrive.
const ModelSpec &SmallModel();
const ModelSpec &LargeModel();

// Pick the model best suited for this question. Returns LargeModel() for
// analytic / window / join shapes; SmallModel() otherwise. Deterministic.
const ModelSpec &PickModelForQuestion(const std::string &question);

// Start background (detached thread) downloads for any tier that is not
// yet on disk. Idempotent - already-downloaded tiers return immediately,
// already-in-flight downloads aren't duplicated. Non-blocking: call this
// at the top of .ask so the 1.5B starts streaming while the 0.5B serves
// the first simple question.
void StartBackgroundDownloads();

// True iff SLOTHDB_ASK_MODEL=ON was set at build time AND third_party/
// llama.cpp was present. Consumers call this before offering --model.
bool EmbeddedAvailable();

// Ensure a specific model is present on disk. Waits for an in-flight
// background download if there is one, otherwise downloads inline.
// Streams progress to stderr when verbose. Returns false + reason on
// failure.
bool EnsureModelAvailable(const ModelSpec &spec, bool verbose,
                          std::string &err);

// Backwards-compatible wrapper: ensure the DefaultModel() (small tier).
bool EnsureModelDownloaded(bool verbose, std::string &err);

// Generate SQL from the schema + question using the local model.
// Blocking call. Single response, not streaming (output size is small).
EmbeddedResult GenerateSQLLocal(const Schema &schema,
                                const std::string &question);

} // namespace ask
} // namespace slothdb
