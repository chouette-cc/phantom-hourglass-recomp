// Native C++ test mod #1: exports TestMathService + verifies ResourceService, SaveService, and ConfigService.
#include <cstring>

#include "ph_mods/service.hpp"
#include "ph_mods/svc/config.h"
#include "ph_mods/svc/host.h"
#include "ph_mods/svc/log.h"
#include "ph_mods/svc/memory.h"
#include "ph_mods/svc/resource.h"
#include "ph_mods/svc/save.h"
#include "test_math_service.h"

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HostService, svc_host);
IMPORT_SERVICE(ResourceService, svc_res);
IMPORT_SERVICE(ConfigService, svc_cfg);
IMPORT_SERVICE(MemoryService, svc_mem);
IMPORT_SERVICE(SaveService, svc_save);

static int g_offset = 100;
static ConfigVarHandle g_offset_var = 0;

static int add_offset_impl(ModContext*, int x) { return x + g_offset; }

static constexpr TestMathService g_math_service{
    SERVICE_HEADER(TestMathService, TEST_MATH_SERVICE_MAJOR, TEST_MATH_SERVICE_MINOR),
    add_offset_impl,
};
EXPORT_SERVICE(g_math_service);

static void on_offset_changed(ModContext*, ConfigVarHandle, const ConfigVarValue* val, const ConfigVarValue*, void*) {
    if (val) g_offset = int(val->int_value);
}

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* err) {
    svc_log->info(mod_ctx, "provider_mod initializing");

    // Verify bundled resource from res/hello.txt inside the .phmod archive
    ResourceBuffer buf = RESOURCE_BUFFER_INIT;
    if (svc_res->load(mod_ctx, "hello.txt", &buf) != MOD_OK) {
        return ph_mods::set_error(err, MOD_ERROR, "failed to load res/hello.txt");
    }
    const bool ok_res = buf.size >= 5 && std::memcmp(buf.data, "HELLO", 5) == 0;
    svc_res->free(mod_ctx, &buf);
    if (!ok_res) return ph_mods::set_error(err, MOD_ERROR, "unexpected res/hello.txt content");

    // Register typed config var + change subscription
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = "offset";
    desc.label = "Math Offset";
    desc.type = CONFIG_VAR_INT;
    desc.default_int = 100;
    desc.min_value = 0;
    desc.max_value = 500;
    desc.step = 25;
    if (svc_cfg->register_var(mod_ctx, &desc, &g_offset_var) != MOD_OK)
        return ph_mods::set_error(err, MOD_ERROR, "register_var failed");
    int64_t cur = 100;
    svc_cfg->get_int(mod_ctx, g_offset_var, &cur);
    g_offset = int(cur);
    svc_cfg->subscribe(mod_ctx, g_offset_var, on_offset_changed, nullptr, nullptr);

    // Verify SaveService blob write & read
    const uint32_t magic = 0xCAFEBABEu;
    if (svc_save->set_blob(mod_ctx, "provider_state", &magic, sizeof(magic)) != MOD_OK)
        return ph_mods::set_error(err, MOD_ERROR, "save set_blob failed");
    uint32_t read_back = 0;
    size_t sz = sizeof(read_back);
    if (svc_save->get_blob(mod_ctx, "provider_state", &read_back, &sz) != MOD_OK || read_back != magic)
        return ph_mods::set_error(err, MOD_ERROR, "save get_blob mismatch");

    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    svc_mem->poke32(mod_ctx, 0x02000400u, uint32_t(g_offset));
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(void) {
    svc_mem->poke32(mod_ctx, 0x02000404u, 0x55555555u);
    return MOD_OK;
}

}  // extern "C"
