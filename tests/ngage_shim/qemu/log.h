#ifndef NGAGE_SHIM_LOG_H
#define NGAGE_SHIM_LOG_H
#define LOG_UNIMP 1
extern GString *shim_log;            /* every qemu_log_mask line, for the tests to search */
#define qemu_log_mask(mask, ...) g_string_append_printf(shim_log, __VA_ARGS__)
#endif
