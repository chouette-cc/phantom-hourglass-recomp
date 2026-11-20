#ifndef PH_MODS_SVC_LOG_H
#define PH_MODS_SVC_LOG_H

#include "ph_mods/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_LOG_SERVICE_ID "ph.core.log"
#define PH_LOG_SERVICE_MAJOR 1u
#define PH_LOG_SERVICE_MINOR 0u

typedef enum PhLogLevel {
    PH_LOG_TRACE = 0,
    PH_LOG_DEBUG = 1,
    PH_LOG_INFO = 2,
    PH_LOG_WARN = 3,
    PH_LOG_ERROR = 4
} PhLogLevel;

typedef struct LogService {
    ServiceHeader header;
    void (*write)(ModContext* ctx, PhLogLevel level, const char* message);
    void (*trace)(ModContext* ctx, const char* message);
    void (*debug)(ModContext* ctx, const char* message);
    void (*info)(ModContext* ctx, const char* message);
    void (*warn)(ModContext* ctx, const char* message);
    void (*error)(ModContext* ctx, const char* message);
} LogService;

#ifdef __cplusplus
}
#include "ph_mods/service.hpp"
template <>
struct ph_mods::ServiceTraits<LogService> {
    static constexpr const char* id = PH_LOG_SERVICE_ID;
    static constexpr uint16_t major_version = PH_LOG_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = PH_LOG_SERVICE_MINOR;
};
#endif

#endif /* PH_MODS_SVC_LOG_H */
