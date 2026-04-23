#include "slothdb/ask/embedded_llm.hpp"
#include "slothdb/storage/http_client.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef SLOTHDB_ASK_MODEL
#include "llama.h"
#endif

namespace slothdb {
namespace ask {

namespace {

std::string HomeDir() {
#ifdef _MSC_VER
    char *buf = nullptr; size_t sz = 0;
    if (_dupenv_s(&buf, &sz, "USERPROFILE") == 0 && buf) {
        std::string v(buf); free(buf); return v;
    }
    return ".";
#else
    const char *h = std::getenv("HOME");
    return h ? std::string(h) : std::string(".");
#endif
}

// Render a DDL block for the prompt. Capped at 20 tables — enough for
// realistic shell sessions, prevents the prompt from exploding if the
// user has an enormous catalog attached.
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

// Build the chat-formatted prompt. Qwen2.5 uses the ChatML format
// (<|im_start|>...<|im_end|>). We render that directly — llama.cpp's
// chat-template API would work too but this is explicit and easier to
// debug when output looks wrong.
std::string RenderPromptQwen(const std::string &ddl, const std::string &question) {
    std::ostringstream ss;
    ss << "<|im_start|>system\n"
       << "You are a SQL generator for SlothDB (DuckDB-compatible dialect). "
       << "Given the schema below and a natural-language question, output ONE "
       << "SQL SELECT statement that answers the question. No commentary. "
       << "Use only tables and columns from the schema. Prefer explicit column "
       << "names over SELECT *. Wrap identifiers in double quotes when they "
       << "could clash with keywords.\n\nSchema:\n"
       << ddl
       << "<|im_end|>\n"
       << "<|im_start|>user\n"
       << question
       << "<|im_end|>\n"
       << "<|im_start|>assistant\n";
    return ss.str();
}

// Strip markdown fences + trim + drop trailing semicolons. Mirrors the
// extraction in llm_provider.cpp for consistency across both paths.
std::string ExtractSQL(const std::string &text) {
    std::string s = text;
    auto fence = s.find("```");
    if (fence != std::string::npos) {
        size_t start = fence + 3;
        while (start < s.size() && s[start] != '\n') start++;
        if (start < s.size()) start++;
        auto end = s.find("```", start);
        if (end != std::string::npos) {
            s = s.substr(start, end - start);
        }
    }
    auto is_ws = [](char c){ return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && is_ws(s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    while (!s.empty() && s.back() == ';') s.pop_back();
    return s;
}

} // namespace

std::string ModelDir() {
    auto p = std::filesystem::path(HomeDir()) / ".slothdb" / "models";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.string();
}

const ModelSpec &DefaultModel() {
    // Pinned GGUF. Qwen2.5-Coder-0.5B-Instruct, Q4_K_M quantization,
    // ~310 MB on disk. Apache 2.0. Coding-tuned — better SQL quality
    // at this size than the generic Instruct variant.
    //
    // The SHA256 below is left empty for now; on first release land the
    // actual hash of the file we ship against so users get verified
    // downloads. The CLI will warn rather than fail if sha256 is empty,
    // to make the first release easy.
    static const ModelSpec spec = {
        /*name=*/          "Qwen2.5-Coder-0.5B-Instruct-Q4_K_M",
        /*url=*/           "https://huggingface.co/Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF/"
                           "resolve/main/qwen2.5-coder-0.5b-instruct-q4_k_m.gguf",
        /*sha256=*/        "", // TODO: pin on first release
        /*local_file=*/    "qwen2.5-coder-0.5b-instruct.Q4_K_M.gguf",
        /*chat_template=*/ "qwen",
        /*expected_bytes=*/ 310ULL * 1024ULL * 1024ULL
    };
    return spec;
}

std::string DefaultModelPath() {
    return (std::filesystem::path(ModelDir()) / DefaultModel().local_file).string();
}

bool EmbeddedAvailable() {
#ifdef SLOTHDB_ASK_MODEL
    return true;
#else
    return false;
#endif
}

bool EnsureModelDownloaded(bool verbose, std::string &err) {
    const auto &spec = DefaultModel();
    auto path = DefaultModelPath();
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        auto sz = std::filesystem::file_size(path, ec);
        if (!ec && sz > spec.expected_bytes / 2) {
            // Size sanity check passed — trust the file.
            return true;
        }
        // Partial / empty file — retry.
        if (verbose) {
            fprintf(stderr, "  (existing model file looks truncated; re-downloading)\n");
        }
    }
    if (verbose) {
        fprintf(stderr, "  Downloading %s (~%zu MB) from:\n    %s\n  to: %s\n",
                spec.name,
                static_cast<size_t>(spec.expected_bytes / (1024ULL * 1024ULL)),
                spec.url, path.c_str());
        fprintf(stderr, "  (one-time; lives in ~/.slothdb/models/)\n");
    }
    if (!HTTPClient::DownloadToFile(spec.url, path)) {
        err = "Download failed. Check network and retry, or grab the file "
              "manually and put it at: " + path;
        return false;
    }
    if (verbose) fprintf(stderr, "  Model downloaded.\n");
    return true;
}

#ifdef SLOTHDB_ASK_MODEL

EmbeddedResult GenerateSQLLocal(const Schema &schema,
                                 const std::string &question) {
    EmbeddedResult r;
    std::string err;
    if (!EnsureModelDownloaded(/*verbose=*/true, err)) {
        r.message = err; return r;
    }
    auto path = DefaultModelPath();

    // Initialise llama.cpp backend (idempotent; cheap to call each time).
    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0; // CPU-only; GPU would require platform-specific
                              // backends which we don't link.
    llama_model *model = llama_model_load_from_file(path.c_str(), mparams);
    if (!model) {
        r.message = "llama_model_load_from_file failed for: " + path;
        return r;
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx     = 2048;
    cparams.n_batch   = 512;
    cparams.n_threads = 4;
    llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) {
        llama_model_free(model);
        r.message = "llama_init_from_model failed";
        return r;
    }

    const std::string prompt = RenderPromptQwen(RenderDDL(schema), question);

    // Tokenize.
    const llama_vocab *vocab = llama_model_get_vocab(model);
    const int max_tokens = 4096;
    std::vector<llama_token> tokens(max_tokens);
    int n_tokens = llama_tokenize(vocab, prompt.c_str(), (int)prompt.size(),
                                  tokens.data(), max_tokens,
                                  /*add_special=*/true, /*parse_special=*/true);
    if (n_tokens < 0) {
        llama_free(ctx); llama_model_free(model);
        r.message = "tokenize failed";
        return r;
    }
    tokens.resize(n_tokens);

    // Decode prompt.
    llama_batch batch = llama_batch_init((int)tokens.size(), 0, 1);
    batch.n_tokens = (int)tokens.size();
    for (int i = 0; i < batch.n_tokens; i++) {
        batch.token[i]    = tokens[i];
        batch.pos[i]      = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i]   = (i == batch.n_tokens - 1) ? 1 : 0;
    }
    if (llama_decode(ctx, batch) != 0) {
        llama_batch_free(batch);
        llama_free(ctx); llama_model_free(model);
        r.message = "llama_decode (prompt) failed";
        return r;
    }

    // Greedy decode — simple and deterministic. The question space here
    // (single SQL query) doesn't benefit much from sampling.
    std::string out;
    int n_predict = 512;
    int cur_pos = batch.n_tokens;
    for (int step = 0; step < n_predict; step++) {
        float *logits = llama_get_logits_ith(ctx, batch.n_tokens - 1);
        int vocab_size = llama_vocab_n_tokens(vocab);
        // Argmax.
        int best = 0;
        float best_v = logits[0];
        for (int i = 1; i < vocab_size; i++) {
            if (logits[i] > best_v) { best_v = logits[i]; best = i; }
        }
        llama_token tok = best;
        if (llama_vocab_is_eog(vocab, tok)) break;

        // Detokenize incrementally.
        char piece[256];
        int np = llama_token_to_piece(vocab, tok, piece, sizeof(piece),
                                       /*lstrip=*/0, /*special=*/false);
        if (np > 0) out.append(piece, np);

        // Stop on </assistant> marker or ChatML end.
        if (out.find("<|im_end|>") != std::string::npos) break;

        // Feed back.
        batch.n_tokens = 1;
        batch.token[0]    = tok;
        batch.pos[0]      = cur_pos++;
        batch.n_seq_id[0] = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0]   = 1;
        if (llama_decode(ctx, batch) != 0) break;
    }

    llama_batch_free(batch);
    llama_free(ctx);
    llama_model_free(model);

    // Strip any trailing <|im_end|> and friends.
    auto end_marker = out.find("<|im_end|>");
    if (end_marker != std::string::npos) out = out.substr(0, end_marker);

    r.sql = ExtractSQL(out);
    if (r.sql.empty()) {
        r.message = "model returned empty SQL";
        return r;
    }
    r.ok = true;
    return r;
}

#else // !SLOTHDB_ASK_MODEL

EmbeddedResult GenerateSQLLocal(const Schema &, const std::string &) {
    EmbeddedResult r;
    r.message = "SlothDB was built without -DSLOTHDB_ASK_MODEL=ON. "
                "Rebuild with that flag to enable `.ask --model`. See "
                "docs/ASK.md#embedded-model for details.";
    return r;
}

#endif

} // namespace ask
} // namespace slothdb
