// Phantom Hourglass Recomp — Native Mod SDK (C++20 ergonomic helpers)
//
// Usage in one translation unit (e.g. mod.cpp):
//   #include <ph_mods/service.hpp>
//   #include <ph_mods/svc/log.h>
//
//   DEFINE_MOD();
//   IMPORT_SERVICE(LogService, svc_log);
//
//   extern "C" MOD_EXPORT ModResult mod_initialize(ModError* err) {
//       svc_log->info(mod_ctx, "Hello from native mod!");
//       return MOD_OK;
//   }
#pragma once

#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

#include "ph_mods/api.h"

namespace ph_mods {

template <typename T>
struct ServiceTraits;

inline ModResult set_error(ModError* err, ModResult code, const char* msg) {
    if (err) {
        err->code = code;
        if (msg) {
            std::snprintf(err->message, sizeof(err->message), "%s", msg);
        } else {
            err->message[0] = '\0';
        }
    }
    return code;
}

namespace detail {

inline std::vector<ModServiceImport>& imports_registry() {
    static std::vector<ModServiceImport> reg;
    return reg;
}

inline std::vector<ModServiceExport>& exports_registry() {
    static std::vector<ModServiceExport> reg;
    return reg;
}

struct ImportRegistrar {
    ImportRegistrar(const char* id, uint16_t maj, uint16_t min_minor, bool optional, const void** slot) {
        imports_registry().push_back({id, maj, min_minor, optional, slot});
    }
};

struct ExportRegistrar {
    ExportRegistrar(const char* id, uint16_t maj, uint16_t min_ver, const void* svc) {
        exports_registry().push_back({id, maj, min_ver, svc});
    }
};

}  // namespace detail
}  // namespace ph_mods

extern ModContext* mod_ctx;

// Weak/optional lifecycle hooks: override any subset in your mod.cpp!
// Define PH_MOD_HAS_UPDATE / PH_MOD_HAS_INPUT / PH_MOD_HAS_DRAW / PH_MOD_HAS_POST_DRAW / PH_MOD_HAS_SHUTDOWN
// or use DEFINE_MOD_CALLBACKS(...) for explicit binding.
#define PH_MOD_CONCAT_IMPL(a, b) a##b
#define PH_MOD_CONCAT(a, b) PH_MOD_CONCAT_IMPL(a, b)

#define IMPORT_SERVICE_VERSION(Type, var, min_minor)                                                           \
    const Type* var = nullptr;                                                                                 \
    static ::ph_mods::detail::ImportRegistrar PH_MOD_CONCAT(_ph_imp_, __LINE__)(                               \
        ::ph_mods::ServiceTraits<Type>::id, ::ph_mods::ServiceTraits<Type>::major_version,                     \
        static_cast<uint16_t>(min_minor), false, reinterpret_cast<const void**>(&var))

#define IMPORT_SERVICE(Type, var)                                                                              \
    IMPORT_SERVICE_VERSION(Type, var, ::ph_mods::ServiceTraits<Type>::minor_version)

#define IMPORT_OPTIONAL_SERVICE_VERSION(Type, var, min_minor)                                                  \
    const Type* var = nullptr;                                                                                 \
    static ::ph_mods::detail::ImportRegistrar PH_MOD_CONCAT(_ph_imp_opt_, __LINE__)(                           \
        ::ph_mods::ServiceTraits<Type>::id, ::ph_mods::ServiceTraits<Type>::major_version,                     \
        static_cast<uint16_t>(min_minor), true, reinterpret_cast<const void**>(&var))

#define IMPORT_OPTIONAL_SERVICE(Type, var)                                                                     \
    IMPORT_OPTIONAL_SERVICE_VERSION(Type, var, ::ph_mods::ServiceTraits<Type>::minor_version)

#define EXPORT_SERVICE(instance)                                                                               \
    static ::ph_mods::detail::ExportRegistrar PH_MOD_CONCAT(_ph_exp_, __LINE__)(                               \
        ::ph_mods::ServiceTraits<::std::remove_cv_t<::std::remove_reference_t<decltype(instance)>>>::id,       \
        ::ph_mods::ServiceTraits<::std::remove_cv_t<::std::remove_reference_t<decltype(instance)>>>::major_version, \
        ::ph_mods::ServiceTraits<::std::remove_cv_t<::std::remove_reference_t<decltype(instance)>>>::minor_version, \
        static_cast<const void*>(&(instance)))

#define EXPORT_DEFERRED_SERVICE(Type)                                                                          \
    static ::ph_mods::detail::ExportRegistrar PH_MOD_CONCAT(_ph_exp_def_, __LINE__)(                           \
        ::ph_mods::ServiceTraits<Type>::id, ::ph_mods::ServiceTraits<Type>::major_version,                     \
        ::ph_mods::ServiceTraits<Type>::minor_version, nullptr)

// Place DEFINE_MOD() once in your main translation unit (implements `mod_ctx` and `ph_mod_descriptor`).
// You can provide `mod_initialize`, `mod_update`, `mod_input`, `mod_draw`, `mod_post_draw`, `mod_shutdown`
// as `extern "C" MOD_EXPORT` functions; the loader discovers them both via the descriptor and dlsym/GetProcAddress.
#define DEFINE_MOD()                                                                                           \
    ModContext* mod_ctx = nullptr;                                                                             \
    extern "C" MOD_EXPORT const ModDescriptor* ph_mod_descriptor(void) {                                       \
        static ModDescriptor desc{};                                                                           \
        desc.abi_version = PH_MOD_SDK_ABI_VERSION;                                                             \
        desc.ctx_slot = &mod_ctx;                                                                              \
        desc.imports = ::ph_mods::detail::imports_registry().empty()                                           \
                           ? nullptr                                                                           \
                           : ::ph_mods::detail::imports_registry().data();                                     \
        desc.import_count = ::ph_mods::detail::imports_registry().size();                                      \
        desc.exports = ::ph_mods::detail::exports_registry().empty()                                           \
                           ? nullptr                                                                           \
                           : ::ph_mods::detail::exports_registry().data();                                     \
        desc.export_count = ::ph_mods::detail::exports_registry().size();                                      \
        return &desc;                                                                                          \
    }
