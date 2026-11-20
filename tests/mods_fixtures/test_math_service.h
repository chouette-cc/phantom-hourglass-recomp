// Shared custom service header between test_provider_mod and test_consumer_mod (tests inter-mod services!).
#pragma once

#include "ph_mods/api.h"

#define TEST_MATH_SERVICE_ID "com.phrecomp.test.math"
#define TEST_MATH_SERVICE_MAJOR 1u
#define TEST_MATH_SERVICE_MINOR 0u

typedef struct TestMathService {
    ServiceHeader header;
    int (*add_offset)(ModContext* ctx, int x);
} TestMathService;

#ifdef __cplusplus
#include "ph_mods/service.hpp"
template <>
struct ph_mods::ServiceTraits<TestMathService> {
    static constexpr const char* id = TEST_MATH_SERVICE_ID;
    static constexpr uint16_t major_version = TEST_MATH_SERVICE_MAJOR;
    static constexpr uint16_t minor_version = TEST_MATH_SERVICE_MINOR;
};
#endif
