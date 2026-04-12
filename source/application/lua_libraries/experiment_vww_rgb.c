/*
 * VWW RGB Experiment Implementation (Placeholder)
 *
 * RGB person detection experiment - future implementation.
 * This file is compiled when ML_EXPERIMENT=VWW_RGB is set.
 */

#include "experiment_common.h"
#include "error_logging.h"
#include "lauxlib.h"

/*-----------------------------------------------*/
/* Placeholder Implementation                    */
/*-----------------------------------------------*/

static int lua_experiment_not_implemented(lua_State *L)
{
    luaL_error(L, "VWW_RGB experiment not implemented yet");
    return 0;
}

/*-----------------------------------------------*/
/* Experiment Interface Implementation           */
/*-----------------------------------------------*/

const char* experiment_get_name(void)
{
    return "VWW_RGB";
}

void experiment_register_lua_functions(lua_State *L, int experiment_table)
{
    (void)experiment_table;

    /* TODO: Implement RGB person detection functions */
    lua_pushcfunction(L, lua_experiment_not_implemented);
    lua_setfield(L, -2, "run_person_detection");

    LOG("VWW_RGB experiment registered (placeholder)");
}
