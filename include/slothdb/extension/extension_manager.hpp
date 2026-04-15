#pragma once

#include "slothdb/extension/extension_api.h"
#include "slothdb/common/types/value.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace slothdb {

// Registered extension function.
struct ExtensionFunction {
    std::string name;
    int num_args;
    slothdb_ext_scalar_func c_func;     // C ABI function pointer.
    slothdb_ext_type return_type;
    std::string description;
};

// Loaded extension.
struct LoadedExtension {
    std::string name;
    std::string path;
    void *handle = nullptr;  // OS-level library handle.
    int api_version_major = 0;
    int api_version_minor = 0;
};

// Extension Manager: loads/unloads extensions, manages registered functions.
class ExtensionManager {
public:
    ExtensionManager();
    ~ExtensionManager();

    // Load an extension from a shared library file.
    // The library must export slothdb_extension_init().
    bool LoadExtension(const std::string &path);

    // Load an extension by name (searches standard paths).
    bool LoadExtensionByName(const std::string &name);

    // Unload an extension.
    void UnloadExtension(const std::string &name);

    // Register a scalar function (called by extensions via C ABI).
    void RegisterScalarFunction(const ExtensionFunction &func);

    // Look up a registered function by name.
    const ExtensionFunction *FindFunction(const std::string &name) const;

    // Execute a registered extension function.
    Value ExecuteFunction(const std::string &name,
                           const std::vector<Value> &args) const;

    // List all loaded extensions.
    std::vector<std::string> ListExtensions() const;

    // List all registered functions.
    std::vector<std::string> ListFunctions() const;

    // Get the singleton instance.
    static ExtensionManager &GetInstance();

private:
    void *LoadDynLib(const std::string &path);
    void *GetDynSymbol(void *handle, const std::string &name);
    void FreeDynLib(void *handle);

    std::vector<LoadedExtension> loaded_extensions_;
    std::unordered_map<std::string, ExtensionFunction> functions_;
};

} // namespace slothdb
