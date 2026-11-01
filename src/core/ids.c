#define TB_CURRENT_GROUP TB_G_CORE /* зона ответственности файла */
/* ==========================================================================
 *  tb/core/ids.c — имена групп-владельцев (один источник истины).
 * ========================================================================== */
#include "tb/core/ids.h"

#include <stdio.h>

const char *tb_group_name(uint16_t group)
{
    switch (group) {
    case TB_G_UNASSIGNED: return "unassigned";
    case TB_G_CORE:       return "core";
    case TB_G_REGISTRY:   return "registry";
    case TB_G_ARCH:       return "arch";
    case TB_G_SCHED:      return "sched";
    case TB_G_POOL:       return "pool";
    case TB_G_BACKOFF:    return "backoff";
    case TB_G_RATELIMIT:  return "ratelimit";
    case TB_G_BREAKER:    return "breaker";
    case TB_G_EVENTS:     return "events";
    case TB_G_BUILTINS:   return "builtins";
    case TB_G_LEDGER:     return "ledger";
    case TB_G_JSON:       return "json";
    case TB_G_CONFIG:     return "config";
    case TB_G_NET:        return "net";
    case TB_G_EXEC:       return "exec";
    case TB_G_SIM:        return "sim";
    case TB_G_DEX:        return "dex";
    case TB_G_FILTERS:    return "filters";
    case TB_G_DECISION:   return "decision";
    case TB_G_ARB:        return "arb";
    case TB_G_DEMO:       return "demo";
    default: {
        static char buf[8];
        snprintf(buf, sizeof(buf), "g%u", (unsigned)group);
        return buf;
    }
    }
}
