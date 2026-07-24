/*
 * SMF state table template for Zephyr state-machine workflows.
 */

#include <zephyr/smf.h>
#include <zephyr/kernel.h>

struct app_ctx {
    struct smf_ctx smf;
    int retry_count;
};

enum app_state {
    APP_STATE_INIT,
    APP_STATE_READY,
    APP_STATE_ACTIVE,
    APP_STATE_ERROR,
};

static const struct smf_state app_states[];

static void init_entry(void *obj)
{
    struct app_ctx *ctx = obj;
    ctx->retry_count = 0;
}

static void init_run(void *obj)
{
    struct app_ctx *ctx = obj;
    smf_set_state(SMF_CTX(ctx), &app_states[APP_STATE_READY]);
}

static void ready_run(void *obj)
{
    struct app_ctx *ctx = obj;
    ARG_UNUSED(ctx);
    /* Transition condition for active mode goes here. */
}

static void active_run(void *obj)
{
    struct app_ctx *ctx = obj;
    ARG_UNUSED(ctx);
    /* Main processing loop state. */
}

static void error_run(void *obj)
{
    struct app_ctx *ctx = obj;
    ARG_UNUSED(ctx);
    /* Recovery and fallback logic. */
}

static const struct smf_state app_states[] = {
    [APP_STATE_INIT] = SMF_CREATE_STATE(init_entry, init_run, NULL, NULL, NULL),
    [APP_STATE_READY] = SMF_CREATE_STATE(NULL, ready_run, NULL, NULL, NULL),
    [APP_STATE_ACTIVE] = SMF_CREATE_STATE(NULL, active_run, NULL, NULL, NULL),
    [APP_STATE_ERROR] = SMF_CREATE_STATE(NULL, error_run, NULL, NULL, NULL),
};

void app_state_machine_start(struct app_ctx *ctx)
{
    smf_set_initial(SMF_CTX(ctx), &app_states[APP_STATE_INIT]);
}
