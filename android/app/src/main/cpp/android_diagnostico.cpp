#include <jni.h>
#include <vulkan/vulkan.h>
#include <rex/ui/vulkan/android_gpu_driver.h>
#include <dlfcn.h>
#include <cstring>

#include <sstream>
#include <string>
#include <vector>

namespace {
std::string Json(const std::string& value) {
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') result += '\\';
    if (c >= 32) result += char(c);
  }
  return result + '"';
}

std::string Probe(const char* hooks, const char* temp, const char* directory, const char* library) {
  struct Loader {
    void* handle;
    ~Loader() { if (handle) dlclose(handle); }
  } loader{rex_android_open_vulkan(hooks, temp, directory, library)};
  if (!loader.handle)
    return "{\"driverLoadFailed\":true,\"probeError\":\"No se pudo abrir el driver seleccionado\"}";
  auto vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader.handle, "vkGetInstanceProcAddr"));
  if (!vkGetInstanceProcAddr)
    return "{\"driverLoadFailed\":true,\"probeError\":\"El paquete no ofrece vkGetInstanceProcAddr\"}";
  auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
  if (!vkCreateInstance)
    return "{\"driverLoadFailed\":true,\"probeError\":\"El paquete no ofrece vkCreateInstance\"}";
  uint32_t loader_version = VK_API_VERSION_1_0;
  auto version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
  if (version) version(&loader_version);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "NFSMW diagnostics";
  app.apiVersion = loader_version >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2
      : loader_version >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  VkInstance instance{};
  VkResult result = vkCreateInstance(&ci, nullptr, &instance);
  if (result != VK_SUCCESS) {
    return "{\"driverLoadFailed\":true,\"compatible\":false,\"missing\":[\"No se pudo iniciar Vulkan (" +
        std::to_string(result) + ")\"]}";
  }
  // Resolve every entry point through the chosen loader, never through the system libvulkan link.
#define LOAD_INSTANCE(name) \
  auto name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name)); \
  if (!name) return "{\"driverLoadFailed\":true,\"probeError\":\"Driver con funciones Vulkan incompletas\"}"
  LOAD_INSTANCE(vkDestroyInstance);
  LOAD_INSTANCE(vkEnumeratePhysicalDevices);
  LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
  LOAD_INSTANCE(vkGetPhysicalDeviceFeatures);
  LOAD_INSTANCE(vkGetPhysicalDeviceFormatProperties);
  LOAD_INSTANCE(vkEnumerateDeviceExtensionProperties);
  LOAD_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
  LOAD_INSTANCE(vkCreateDevice);
#undef LOAD_INSTANCE
  uint32_t count = 0;
  result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (result != VK_SUCCESS || !count) {
    vkDestroyInstance(instance, nullptr);
    return "{\"driverLoadFailed\":true,\"compatible\":false,\"missing\":[\"No se encontro una GPU Vulkan\"]}";
  }
  std::vector<VkPhysicalDevice> devices(count);
  result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
  if (result != VK_SUCCESS) {
    vkDestroyInstance(instance, nullptr);
    return "{\"compatible\":false,\"missing\":[\"No se pudo consultar la GPU\"]}";
  }
  // Android's native renderer selects the first physical device by default.
  VkPhysicalDevice gpu = devices[0];
  VkPhysicalDeviceProperties props{};
  VkPhysicalDeviceFeatures features{};
  vkGetPhysicalDeviceProperties(gpu, &props);
  vkGetPhysicalDeviceFeatures(gpu, &features);
  // Device extensions: whether descriptor indexing exists on a 1.1 driver, and the list for the report.
  std::vector<std::string> extensions;
  {
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr) == VK_SUCCESS && n) {
      std::vector<VkExtensionProperties> list(n);
      if (vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, list.data()) == VK_SUCCESS) {
        for (uint32_t i = 0; i < n; ++i) extensions.emplace_back(list[i].extensionName);
      }
    }
  }
  const auto has_extension = [&](const char* name) {
    for (const auto& e : extensions) {
      if (e == name) return true;
    }
    return false;
  };
  // The native renderer's texture heaps: Vulkan 1.2 core, or VK_EXT_descriptor_indexing on a 1.1 driver
  // (the SDK enables either; the library's SPIR-V is converted to 1.3 for 1.1).
  VkPhysicalDeviceDescriptorIndexingFeatures indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
  const bool indexing_extension = props.apiVersion < VK_API_VERSION_1_2 &&
                                  has_extension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
  if (props.apiVersion >= VK_API_VERSION_1_1) {
    VkPhysicalDeviceFeatures2 fs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    fs.pNext = &indexing;
    auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
    if (query && (props.apiVersion >= VK_API_VERSION_1_2 || indexing_extension)) query(gpu, &fs);
  }
  std::vector<std::string> missing;
  auto require = [&](bool supported, const char* name) {
    if (!supported) missing.emplace_back(name);
  };
  // Since v0.3.7 the native shaders need neither shaderInt64 nor buffer device address, and Vulkan 1.1 is
  // enough when the driver has VK_EXT_descriptor_indexing.
  require(props.apiVersion >= VK_API_VERSION_1_1, "Vulkan 1.1 o posterior");
  require(props.apiVersion >= VK_API_VERSION_1_2 || indexing_extension,
          "VK_EXT_descriptor_indexing (Vulkan 1.1) o Vulkan 1.2");
  require(props.limits.maxBoundDescriptorSets >= 4, "Cuatro conjuntos de descriptores Vulkan");
  require(features.independentBlend, "independentBlend");
  require(features.shaderSampledImageArrayDynamicIndexing, "shaderSampledImageArrayDynamicIndexing");
  require(indexing.runtimeDescriptorArray, "runtimeDescriptorArray");
  require(indexing.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
  require(indexing.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind");
  require(indexing.descriptorBindingUpdateUnusedWhilePending, "descriptorBindingUpdateUnusedWhilePending");
  VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  if (props.apiVersion >= VK_API_VERSION_1_2 || has_extension(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
    auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
    if (query) {
      VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      properties.pNext = &driver;
      query(gpu, &properties);
    }
  }
  const bool xclipse = driver.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY ||
      (driver.driverID == VkDriverId(0) && std::strstr(props.deviceName, "Xclipse"));
  std::ostringstream formats;
  std::ostringstream conversions;
  conversions << '[';
  bool first_conversion = true;
  formats << '{';
  const VkFormat bc[] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
                         VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK};
  for (unsigned i = 0; i < 5; ++i) {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(gpu, bc[i], &fp);
    constexpr auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    bool supported = (fp.optimalTilingFeatures & required) == required;
    std::string name = "BC" + std::to_string(i + 1);
    if (!supported || (xclipse && i >= 3)) {
      // The native renderer now decodes unsupported BC formats on the CPU.
      // Only reject the device if the corresponding uncompressed format is
      // unavailable too. Keep missing BC in the report as a conversion.
      const VkFormat host = i < 3 ? VK_FORMAT_R8G8B8A8_UNORM
          : i == 3 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
      VkFormatProperties host_props{};
      vkGetPhysicalDeviceFormatProperties(gpu, host, &host_props);
      require((host_props.optimalTilingFeatures & required) == required,
              i < 3 ? "Texturas RGBA8 (conversion BC1-3)"
                    : i == 3 ? "Texturas R8 (conversion BC4)" : "Texturas RG8 (conversion BC5)");
      if (!first_conversion) conversions << ',';
      first_conversion = false;
      conversions << Json(name);
    }
    if (i) formats << ',';
    formats << Json(name) << ':' << (supported ? "true" : "false");
  }
  formats << '}';
  conversions << ']';
  // Physical capabilities alone do not prove that an imported driver can create a logical device.
  VkResult device_result = VK_SUCCESS;
  if (missing.empty()) {
    uint32_t families_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(families_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families_count, families.data());
    uint32_t family = 0;
    while (family < families_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family == families_count) {
      device_result = VK_ERROR_INITIALIZATION_FAILED;
    } else {
      float priority = 1.0f;
      VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queue.queueFamilyIndex = family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
      VkPhysicalDeviceFeatures enabled{};
      enabled.independentBlend = VK_TRUE;
      enabled.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
      VkPhysicalDeviceDescriptorIndexingFeatures wanted{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
      wanted.runtimeDescriptorArray = VK_TRUE;
      wanted.descriptorBindingPartiallyBound = VK_TRUE;
      wanted.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
      wanted.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
      const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME};
      VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      info.pNext = &wanted; info.pEnabledFeatures = &enabled;
      info.queueCreateInfoCount = 1; info.pQueueCreateInfos = &queue;
      info.enabledExtensionCount = indexing_extension ? 2 : 1; info.ppEnabledExtensionNames = exts;
      VkDevice device{};
      device_result = vkCreateDevice(gpu, &info, nullptr, &device);
      if (device_result == VK_SUCCESS) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(vkGetInstanceProcAddr(instance, "vkDestroyDevice"));
        if (destroy) destroy(device, nullptr);
      }
    }
    if (device_result != VK_SUCCESS) missing.emplace_back("vkCreateDevice: " + std::to_string(device_result));
  }
  std::ostringstream output;
  output << "{\"gpu\":" << Json(props.deviceName)
         << ",\"vulkan\":" << Json(std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." +
              std::to_string(VK_VERSION_MINOR(props.apiVersion)) + "." + std::to_string(VK_VERSION_PATCH(props.apiVersion)))
         << ",\"driverVersion\":" << props.driverVersion
         << ",\"vendorId\":" << props.vendorID
         << ",\"requestedDriver\":" << Json(library && *library ? library : "system")
         << ",\"driverName\":" << Json(driver.driverName)
         << ",\"driverInfo\":" << Json(driver.driverInfo)
         << ",\"driverId\":" << int(driver.driverID)
         << ",\"logicalDeviceResult\":" << int(device_result)
         << ",\"driverLoadFailed\":" << (device_result == VK_SUCCESS ? "false" : "true")
         << ",\"maxBoundDescriptorSets\":" << props.limits.maxBoundDescriptorSets
         << ",\"packedDescriptorSets\":" << (props.limits.maxBoundDescriptorSets < 5 ? "true" : "false")
         << ",\"textureFormats\":" << formats.str()
         << ",\"cpuTextureConversions\":" << conversions.str()
         << ",\"shaderInt64\":" << (features.shaderInt64 ? "true" : "false")
         << ",\"descriptorIndexing\":" << Json(props.apiVersion >= VK_API_VERSION_1_2 ? "core"
                                                  : indexing_extension ? "extension" : "none")
         << ",\"vertexPipelineStoresAndAtomics\":" << (features.vertexPipelineStoresAndAtomics ? "true" : "false")
         << ",\"fragmentStoresAndAtomics\":" << (features.fragmentStoresAndAtomics ? "true" : "false")
         << ",\"occlusionQueriesDefault\":" << Json(props.vendorID == 0x13B5 ? "off" : "on")
         << ",\"checkedRenderer\":\"nativo\""
         << ",\"compatible\":" << (missing.empty() ? "true" : "false") << ",\"missing\":[";
  for (size_t i = 0; i < missing.size(); ++i) {
    if (i) output << ',';
    output << Json(missing[i]);
  }
  output << "],\"extensions\":[";
  for (size_t i = 0; i < extensions.size(); ++i) {
    if (i) output << ',';
    output << Json(extensions[i]);
  }
  output << "]}";
  vkDestroyInstance(instance, nullptr);
  return output.str();
}
}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_nfsmw_android_Diagnostics_nativeGpuReport(JNIEnv* env, jclass, jstring hooks,
    jstring temp, jstring directory, jstring library) {
  const char* h = env->GetStringUTFChars(hooks, nullptr);
  const char* t = env->GetStringUTFChars(temp, nullptr);
  const char* d = env->GetStringUTFChars(directory, nullptr);
  const char* l = env->GetStringUTFChars(library, nullptr);
  if (!h || !t || !d || !l) {
    if (h) env->ReleaseStringUTFChars(hooks, h);
    if (t) env->ReleaseStringUTFChars(temp, t);
    if (d) env->ReleaseStringUTFChars(directory, d);
    if (l) env->ReleaseStringUTFChars(library, l);
    return nullptr;
  }
  const std::string report = Probe(h, t, d, l);
  env->ReleaseStringUTFChars(hooks, h);
  env->ReleaseStringUTFChars(temp, t);
  env->ReleaseStringUTFChars(directory, d);
  env->ReleaseStringUTFChars(library, l);
  return env->NewStringUTF(report.c_str());
}
