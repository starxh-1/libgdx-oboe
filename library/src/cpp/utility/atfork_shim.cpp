#include <unistd.h>

/**
 * __register_atfork was introduced in API 23.
 * This stub implementation allows loading libraries that depend on this symbol
 * on older Android versions (like API 21/22).
 * Since Android apps usually don't use fork() directly, returning 0 (success) is safe.
 * We use __attribute__((visibility("default"))) to ensure the symbol is exported
 * even if -fvisibility=hidden is used.
 */
extern "C" __attribute__((visibility("default"))) int __register_atfork(void* prepare, void* parent, void* child, void* dso) {
    return 0;
}
