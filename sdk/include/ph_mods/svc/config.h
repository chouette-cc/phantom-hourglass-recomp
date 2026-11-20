#ifndef PH_MODS_SVC_CONFIG_H
#define PH_MODS_SVC_CONFIG_H

#include "ph_mods/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_CONFIG_SERVICE_ID "ph.core.config"
#define PH_CONFIG_SERVICE_MAJOR 1u
#define PH_CONFIG_SERVICE_MINOR 0u

typedef enum ConfigVarType {
    CONFIG_VAR_BOOL = 0,
    CONFIG_VAR_INT = 1,
    CONFIG_VAR_FLOAT = 2,
    CONFIG_VAR_STRING = 3,
    CONFIG_VAR_CHOICE = 4
} ConfigVarType;

typedef uint64_t ConfigVarHandle;
typedef uint64_t ConfigSubHandle;

typedef struct ConfigVarDesc {
    uint32_t struct_size;
    const char* name;    /* key in config.json */
    const char* label;   /* display label in Mods menu (defaults to name if NULL) */
    ConfigVarType type;
    bool default_bool;
    int64_t default_int;
    double default_float;
    const char* default_string;
    double min_value;
    double max_value;
    double step;
    const char* const* choices;
    size_t choice_count;
} ConfigVarDesc;

#define CONFIG_VAR_DESC_INIT { (uint32_t)sizeof(ConfigVarDesc), NULL, NULL, CONFIG_VAR_BOOL, false, 0, 0.0, NULL, 0.0, 1.0, 1.0, NULL, 0 }

typedef struct ConfigVarValue {
    ConfigVarType type;
    bool bool_value;
    int64_t int_value;
    double float_value;
    const char* string_value;
} ConfigVarValue;

typedef void (*ConfigChangeCallback)(ModContext* ctx, ConfigVarHandle var, const ConfigVarValue* value,
                                     const ConfigVarValue* previous, void* user_data);

typedef struct ConfigService {
    ServiceHeader header;
    ModResult (*register_var)(ModContext* ctx, const ConfigVarDesc* desc, ConfigVarHandle* out_var);
    ModResult (*get_bool)(ModContext* ctx, ConfigVarHandle var, bool* out_val);
    ModResult (*set_bool)(ModContext* ctx, ConfigVarHandle var, bool val);
    ModResult (*get_int)(ModContext* ctx, ConfigVarHandle var, int64_t* out_val);
    ModResult (*set_int)(ModContext* ctx, ConfigVarHandle var, int64_t val);
    ModResult (*get_float)(ModContext* ctx, ConfigVarHandle var, double* out_val);
    ModResult (*set_float)(ModContext* ctx, ConfigVarHandle var, double val);
    ModResult (*get_string)(ModContext* ctx, ConfigVarHandle var, char* out_buf, size_t* io_size);
    ModResult (*set_string)(ModContext* ctx, ConfigVarHandle var, const char* val);
    ModResult (*subscribe)(ModContext* ctx, ConfigVarHandle var, ConfigChangeCallback cb, void* user_data,
                           ConfigSubHandle* out_sub);
    ModResult (*unsubscribe)(ModContext* ctx, ConfigSubHandle sub);
    ModResult (*save)(ModContext* ctx);
} ConfigService;

#ifdef __cplusplus
}
#include "ph_mods/service.hpp"
template <>
struct ph_mods::ServiceTraits<ConfigService> {
    static constexpr const char* id = PH_CONFIG_SERVICE_ID;
    static constexpr uint16_t major_version = PH_CONFIG_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = PH_CONFIG_SERVICE_MINOR;
};
#endif

#endif /* PH_MODS_SVC_CONFIG_H */
