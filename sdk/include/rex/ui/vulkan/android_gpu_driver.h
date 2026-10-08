// Android custom Vulkan loading. Caller owns the returned dlopen handle.
#pragma once
extern "C" __attribute__((visibility("default"))) void* rex_android_open_vulkan(const char* hooks, const char* temp,
                                        const char* directory, const char* library);
