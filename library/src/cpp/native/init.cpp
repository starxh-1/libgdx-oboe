#include "../jni/jni_context.hpp"
#include "../jni/jvm_class.hpp"
#include "external_classes.hpp"

// Force the linker to keep atfork_shim as a NEEDED dependency so
// __register_atfork is available when libavutil.so is loaded.
extern "C" int __register_atfork(void*, void*, void*, void*);
extern "C" void __attribute__((used)) __force_atfork_shim_ref() {
    __register_atfork(0, 0, 0, 0);
}

JavaVM *jni_context::s_jvm = nullptr;
jvm_class::class_cache_map jvm_class::g_class_cache;
jvm_class::field_cache_map jvm_class::g_field_cache;
jvm_class::method_cache_map jvm_class::g_method_cache;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *jvm, void *reserved) {
    jni_context::init(jvm);
    jvm_class::cache_class(external::gdx_music_listener::cls.data());
    jvm_class::cache_class(external::gdx_music::cls.data());
    jvm_class::cache_class(external::gdx_runtime_exception::cls.data());
    return JNI_VERSION_1_6;
}
