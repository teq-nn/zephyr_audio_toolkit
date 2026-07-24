/*
 * Template shell command set for Zephyr shell subsystem.
 */

#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "app status: OK");
    return 0;
}

static int cmd_reset_counter(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    /* Replace with your module reset logic. */
    shell_print(sh, "counter reset");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(app_cmds,
    SHELL_CMD(status, NULL, "Show application status", cmd_status),
    SHELL_CMD(reset_counter, NULL, "Reset module counter", cmd_reset_counter),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(app, &app_cmds, "Application control commands", NULL);
