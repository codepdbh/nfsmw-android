// Based on the adrenotools integration in victorgbd/NFSMW-Recompiled-Mobile.
// No KGSL turbo: it changes device-wide power policy and does not apply to Mali.
#include <rex/ui/vulkan/android_gpu_driver.h>
#include <adrenotools/driver.h>
#include <android/log.h>
#include <dlfcn.h>
#include <string>

extern "C" void* rex_android_open_vulkan(const char* hooks, const char* temp,
                                        const char* directory, const char* library) {
  if (!library || !*library) return dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!hooks || !*hooks || !directory || !*directory) return nullptr;
  std::string path(directory);
  if (path.back() != '/') path += '/';
  // The same loader is used by the separate probe process and by the game.
  void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
      temp && *temp ? temp : nullptr, hooks, path.c_str(), library, nullptr, nullptr);
  __android_log_print(handle ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "NFSMW-driver",
      "Custom Vulkan loader %s: %s%s", handle ? "opened" : "failed", path.c_str(), library);
  return handle;  // Fail explicitly; never claim a system fallback is the selected custom driver.
}
