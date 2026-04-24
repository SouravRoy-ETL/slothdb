#include "slothdb/extension/extension_manager.hpp"
#include "slothdb/common/exception.hpp"
#include "slothdb/common/string_util.hpp"
#include <cstring>

#ifdef _MSC_VER
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// Value wrapper for the C ABI - defined at global scope to match the C header.
struct slothdb_ext_value {
    slothdb::Value val;
    std::string str_cache;
};

// Extension info - stores a pointer back to the ExtensionManager.
struct slothdb_ext_info {
    slothdb::ExtensionManager *manager;
    std::string extension_name;
};

namespace slothdb {

// ============================================================================
// C ABI implementation (called by extensions)
// ============================================================================

extern "C" {

slothdb_ext_value *slothdb_ext_value_null() {
    auto *v = new slothdb_ext_value();
    return v;
}

slothdb_ext_value *slothdb_ext_value_int32(int32_t val) {
    auto *v = new slothdb_ext_value();
    v->val = Value::INTEGER(val);
    return v;
}

slothdb_ext_value *slothdb_ext_value_int64(int64_t val) {
    auto *v = new slothdb_ext_value();
    v->val = Value::BIGINT(val);
    return v;
}

slothdb_ext_value *slothdb_ext_value_double(double val) {
    auto *v = new slothdb_ext_value();
    v->val = Value::DOUBLE(val);
    return v;
}

slothdb_ext_value *slothdb_ext_value_varchar(const char *val) {
    auto *v = new slothdb_ext_value();
    v->val = Value::VARCHAR(val);
    return v;
}

slothdb_ext_value *slothdb_ext_value_boolean(int val) {
    auto *v = new slothdb_ext_value();
    v->val = Value::BOOLEAN(val != 0);
    return v;
}

int slothdb_ext_value_is_null(const slothdb_ext_value *val) {
    return val->val.IsNull() ? 1 : 0;
}

int32_t slothdb_ext_value_get_int32(const slothdb_ext_value *val) {
    return val->val.GetValue<int32_t>();
}

int64_t slothdb_ext_value_get_int64(const slothdb_ext_value *val) {
    if (val->val.type().id() == LogicalTypeId::INTEGER)
        return val->val.GetValue<int32_t>();
    return val->val.GetValue<int64_t>();
}

double slothdb_ext_value_get_double(const slothdb_ext_value *val) {
    return val->val.GetValue<double>();
}

const char *slothdb_ext_value_get_varchar(const slothdb_ext_value *val) {
    auto *mval = const_cast<slothdb_ext_value *>(val);
    mval->str_cache = val->val.ToString();
    return mval->str_cache.c_str();
}

int slothdb_ext_value_get_boolean(const slothdb_ext_value *val) {
    return val->val.GetValue<bool>() ? 1 : 0;
}

void slothdb_ext_value_free(slothdb_ext_value *val) {
    delete val;
}

int slothdb_ext_register_scalar_function(
    slothdb_ext_info *info, const char *name, int num_args,
    slothdb_ext_scalar_func func, slothdb_ext_type return_type) {

    ExtensionFunction ef;
    ef.name = StringUtil::Upper(name);
    ef.num_args = num_args;
    ef.c_func = func;
    ef.return_type = return_type;
    info->manager->RegisterScalarFunction(ef);
    return SLOTHDB_EXT_OK;
}

int slothdb_ext_register_scalar_function_with_desc(
    slothdb_ext_info *info, const char *name, int num_args,
    slothdb_ext_scalar_func func, slothdb_ext_type return_type,
    const char *description) {

    ExtensionFunction ef;
    ef.name = StringUtil::Upper(name);
    ef.num_args = num_args;
    ef.c_func = func;
    ef.return_type = return_type;
    ef.description = description;
    info->manager->RegisterScalarFunction(ef);
    return SLOTHDB_EXT_OK;
}

} // extern "C"

// ============================================================================
// Extension Manager
// ============================================================================

ExtensionManager::ExtensionManager() = default;

ExtensionManager::~ExtensionManager() {
    for (auto &ext : loaded_extensions_) {
        if (ext.handle) FreeDynLib(ext.handle);
    }
}

void *ExtensionManager::LoadDynLib(const std::string &path) {
#ifdef _MSC_VER
    return ::LoadLibraryA(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW);
#endif
}

void *ExtensionManager::GetDynSymbol(void *handle, const std::string &name) {
#ifdef _MSC_VER
    return reinterpret_cast<void *>(
        ::GetProcAddress(static_cast<HMODULE>(handle), name.c_str()));
#else
    return dlsym(handle, name.c_str());
#endif
}

void ExtensionManager::FreeDynLib(void *handle) {
#ifdef _MSC_VER
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

bool ExtensionManager::LoadExtension(const std::string &path) {
    // Security: reject absolute paths and path traversal.
    if (path.find("..") != std::string::npos)
        throw IOException("Extension path cannot contain '..'");
    // Only allow loading from current directory or 'extensions/' subdirectory.
    bool is_relative = (path.find(':') == std::string::npos && path[0] != '/' && path[0] != '\\');
    if (!is_relative)
        throw IOException("Extension loading restricted to relative paths only");

    auto handle = LoadDynLib(path);
    if (!handle) {
        throw IOException("Cannot load extension: " + path);
    }

    // Look for the init function.
    auto init_func = reinterpret_cast<slothdb_ext_init_func>(
        GetDynSymbol(handle, SLOTHDB_EXTENSION_INIT_FUNC));
    if (!init_func) {
        // Try name-based init: slothdb_extension_init_<name>
        auto base = path.substr(path.find_last_of("/\\") + 1);
        auto dot = base.find('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        auto named_init = "slothdb_extension_init_" + base;
        init_func = reinterpret_cast<slothdb_ext_init_func>(
            GetDynSymbol(handle, named_init));
    }

    if (!init_func) {
        FreeDynLib(handle);
        throw IOException("Extension has no init function: " + path);
    }

    // Call init.
    slothdb_ext_info info;
    info.manager = this;
    info.extension_name = path;

    int result = init_func(&info);
    if (result != SLOTHDB_EXT_OK) {
        FreeDynLib(handle);
        throw IOException("Extension init failed: " + path);
    }

    LoadedExtension ext;
    ext.name = path;
    ext.path = path;
    ext.handle = handle;
    ext.api_version_major = SLOTHDB_EXT_API_VERSION_MAJOR;
    ext.api_version_minor = SLOTHDB_EXT_API_VERSION_MINOR;
    loaded_extensions_.push_back(std::move(ext));

    return true;
}

bool ExtensionManager::LoadExtensionByName(const std::string &name) {
    // Search standard paths.
    std::vector<std::string> search_paths = {
        ".",
        "./extensions",
#ifdef _MSC_VER
        name + ".dll",
        "extensions/" + name + ".dll",
#elif defined(__APPLE__)
        name + ".dylib",
        "extensions/" + name + ".dylib",
        "lib" + name + ".dylib",
#else
        name + ".so",
        "extensions/" + name + ".so",
        "lib" + name + ".so",
#endif
    };

    for (auto &path : search_paths) {
        try {
            return LoadExtension(path);
        } catch (...) {
            continue;
        }
    }
    throw IOException("Extension not found: " + name);
}

void ExtensionManager::UnloadExtension(const std::string &name) {
    for (auto it = loaded_extensions_.begin(); it != loaded_extensions_.end(); ++it) {
        if (it->name == name || it->path == name) {
            if (it->handle) FreeDynLib(it->handle);
            loaded_extensions_.erase(it);
            return;
        }
    }
}

void ExtensionManager::RegisterScalarFunction(const ExtensionFunction &func) {
    functions_[func.name] = func;
}

const ExtensionFunction *ExtensionManager::FindFunction(const std::string &name) const {
    auto upper = StringUtil::Upper(name);
    auto it = functions_.find(upper);
    return (it != functions_.end()) ? &it->second : nullptr;
}

Value ExtensionManager::ExecuteFunction(const std::string &name,
                                          const std::vector<Value> &args) const {
    auto *func = FindFunction(name);
    if (!func) throw InternalException("Extension function not found: " + name);

    // Convert args to C ABI values.
    std::vector<slothdb_ext_value> c_values(args.size());
    std::vector<const slothdb_ext_value *> c_ptrs(args.size());
    for (size_t i = 0; i < args.size(); i++) {
        c_values[i].val = args[i];
        c_ptrs[i] = &c_values[i];
    }

    slothdb_ext_func_args c_args;
    c_args.argc = static_cast<int>(args.size());
    c_args.argv = c_ptrs.data();

    // Call the function.
    auto *result = func->c_func(&c_args);
    if (!result) return Value();

    Value ret = result->val;
    slothdb_ext_value_free(result);
    return ret;
}

std::vector<std::string> ExtensionManager::ListExtensions() const {
    std::vector<std::string> names;
    for (auto &ext : loaded_extensions_) names.push_back(ext.name);
    return names;
}

std::vector<std::string> ExtensionManager::ListFunctions() const {
    std::vector<std::string> names;
    for (auto &[name, _] : functions_) names.push_back(name);
    return names;
}

static std::unique_ptr<ExtensionManager> global_ext_manager;

ExtensionManager &ExtensionManager::GetInstance() {
    if (!global_ext_manager) {
        global_ext_manager = std::make_unique<ExtensionManager>();
    }
    return *global_ext_manager;
}

} // namespace slothdb
