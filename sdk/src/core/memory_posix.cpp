/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay & Rien Gupta, 2026 - Adapted for ReXGlue runtime (POSIX + macOS)
 */

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>

#include <fcntl.h>

#if REX_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#endif  // REX_PLATFORM_MAC

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/string.h>

// macOS off_t is 64-bit with no *64 large-file variants; Linux keeps the
// explicit *64 forms for legacy 32-bit off_t distributions.
#if defined(__APPLE__)
#define rex_mmap64 mmap
#define rex_ftruncate64 ftruncate
#else
#define rex_mmap64 mmap64
#define rex_ftruncate64 ftruncate64
#endif

#if REX_PLATFORM_ANDROID
#include <android/api-level.h>
#include <string.h>

#include <dlfcn.h>
#include <sys/ioctl.h>

#include <linux/ashmem.h>

// TODO(tomc): Android or maybe na. idk
// #include "xenia/base/main_android.h"
#endif

namespace rex {
namespace memory {

// Convert a filesystem path to a valid shm_open name (must start with /, no other slashes).
// macOS enforces a 31-character total limit, so long names are folded from the full path rather
// than only the filename. This keeps equal filenames in different directories distinct.
static std::string MakeShmName(const std::filesystem::path& path) {
  std::string name = path.string();
  for (char& c : name) {
    if (c == '/') {
      c = '_';
    }
  }
  if (name.empty() || name[0] != '/') {
    name.insert(name.begin(), '/');
  }
#if REX_PLATFORM_MAC
  if (name.size() > 30) {
    const std::size_t h = std::hash<std::string>{}(name);
    char hash_buf[24];
    std::snprintf(hash_buf, sizeof(hash_buf), "/%016zx", h);
    name = hash_buf;
  }
#endif
  return name;
}

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (android_get_device_api_level() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ = reinterpret_cast<decltype(android_ASharedMemory_create_)>(
          dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

size_t page_size() {
  return getpagesize();
}
size_t allocation_granularity() {
  return page_size();
}

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

bool IsWritableExecutableMemorySupported() {
#if REX_PLATFORM_MAC
  // macOS enforces W^X on Apple Silicon. Shared file mappings cannot be both
  // writable and executable. The code cache must use separate RW and RX views.
  return false;
#else
  return true;
#endif
}

// TODO(tomc): this needs to go somewhere else. we should utilize the platform namespace more.
#if REX_PLATFORM_LINUX
namespace {

struct LinuxMapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  char perms[5] = {};
};

// Parse a line from /proc/self/maps into a LinuxMapEntry
static bool ParseProcMapsLine(const std::string& line, LinuxMapEntry& out) {
  out = LinuxMapEntry{};
  unsigned long long start = 0, end = 0;
  char perms[5] = {};
  const int matched = std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, perms);
  if (matched < 3)
    return false;
  out.start = static_cast<uintptr_t>(start);
  out.end = static_cast<uintptr_t>(end);
  std::memcpy(out.perms, perms, sizeof(out.perms));
  return out.start < out.end;
}

// Find the mapping entry in /proc/self/maps that contains the given address
static bool FindEntryForAddress(void* address, LinuxMapEntry& out_entry) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open())
    return false;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e))
      continue;
    if (addr >= e.start && addr < e.end) {
      out_entry = e;
      return true;
    }
  }
  return false;
}

// Check if [base, base+length) is fully covered by existing mappings (no gaps)
static bool IsRangeFullyMapped(void* base_address, size_t length) {
  if (!base_address || length == 0)
    return false;

  const uintptr_t begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t end = begin + length;
  if (end < begin) {  // overflow check
    return false;
  }

  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open())
    return false;

  uintptr_t cursor = begin;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e))
      continue;
    if (e.end <= cursor)
      continue;
    if (e.start > cursor)
      return false;  // gap found
    cursor = e.end;
    if (cursor >= end)
      return true;
  }
  return cursor >= end;
}

// Convert /proc/self/maps permission chars to PageAccess
static PageAccess PermsToPageAccess(const char perms[5]) {
  const bool r = perms[0] == 'r';
  const bool w = perms[1] == 'w';
  const bool x = perms[2] == 'x';

  if (!r && !w && !x)
    return PageAccess::kNoAccess;
  if (x)
    return w ? PageAccess::kExecuteReadWrite : PageAccess::kExecuteReadOnly;
  return w ? PageAccess::kReadWrite : PageAccess::kReadOnly;
}

}  // namespace
#endif  // REX_PLATFORM_LINUX

#if REX_PLATFORM_LINUX
// Page protections set through this file, by host page. Linux has no call that returns the protection of a
// page: QueryProtect had to read and parse /proc/self/maps (thousands of lines) every time. The write-watch
// fault handler (MMIOHandler) asks on every fault on a watched page, and Protect asks for the old access when
// a watch is set or cleared: with the Xenos backend that was most of the game thread's time on Android.
//
// Every protection the SDK sets on guest memory goes through AllocFixed, DeallocFixed, Protect or MapFileView,
// so they record it here; a page nobody recorded (or whose mapping was released) is "unknown" and QueryProtect
// asks the kernel as before. Lock-free: QueryProtect runs inside the SIGSEGV handler. Chunks of 2^16 pages are
// created on first use and never freed (a few dozen cover the guest views); a full table only means more
// unknown pages.
namespace {
class PageProtectionTable {
 public:
  // value: PageAccess + 1, or 0 to forget the pages.
  void Set(const void* base, size_t length, uint8_t value) {
    const size_t page = page_size();
    if (!length || !page) {
      return;
    }
    uintptr_t first = reinterpret_cast<uintptr_t>(base) / page;
    const uintptr_t last = (reinterpret_cast<uintptr_t>(base) + length - 1) / page;
    while (first <= last) {
      const uintptr_t chunk = first >> kChunkBits;
      std::atomic<uint8_t>* data = value ? GetOrCreate(chunk) : Find(chunk);
      const uintptr_t chunk_end = std::min<uintptr_t>(last, ((chunk + 1) << kChunkBits) - 1);
      if (data) {
        for (uintptr_t i = first; i <= chunk_end; ++i) {
          data[i & kChunkMask].store(value, std::memory_order_release);
        }
      }
      first = chunk_end + 1;
    }
  }

  bool Get(const void* address, PageAccess& out) const {
    const size_t page = page_size();
    const uintptr_t index = reinterpret_cast<uintptr_t>(address) / page;
    const std::atomic<uint8_t>* data = Find(index >> kChunkBits);
    if (!data) {
      return false;
    }
    const uint8_t value = data[index & kChunkMask].load(std::memory_order_acquire);
    if (!value) {
      return false;
    }
    out = static_cast<PageAccess>(value - 1);
    return true;
  }

 private:
  static constexpr unsigned kChunkBits = 16;
  static constexpr uintptr_t kChunkMask = (uintptr_t(1) << kChunkBits) - 1;
  static constexpr size_t kSlots = 1024;  // open addressing, power of two
  static constexpr uintptr_t kEmpty = ~uintptr_t(0);

  struct Slot {
    std::atomic<uintptr_t> key{kEmpty};
    std::atomic<std::atomic<uint8_t>*> data{nullptr};
  };

  static size_t Hash(uintptr_t chunk) { return size_t((chunk * 0x9E3779B97F4A7C15ull) >> 54) & (kSlots - 1); }

  std::atomic<uint8_t>* Find(uintptr_t chunk) const {
    for (size_t i = 0, slot = Hash(chunk); i < kSlots; ++i, slot = (slot + 1) & (kSlots - 1)) {
      const uintptr_t key = slots_[slot].key.load(std::memory_order_acquire);
      if (key == chunk) {
        return slots_[slot].data.load(std::memory_order_acquire);
      }
      if (key == kEmpty) {
        return nullptr;
      }
    }
    return nullptr;
  }

  std::atomic<uint8_t>* GetOrCreate(uintptr_t chunk) {
    for (size_t i = 0, slot = Hash(chunk); i < kSlots; ++i, slot = (slot + 1) & (kSlots - 1)) {
      uintptr_t key = slots_[slot].key.load(std::memory_order_acquire);
      if (key == kEmpty) {
        if (slots_[slot].key.compare_exchange_strong(key, chunk, std::memory_order_acq_rel)) {
          auto* data = new std::atomic<uint8_t>[size_t(1) << kChunkBits]();
          slots_[slot].data.store(data, std::memory_order_release);
          return data;
        }
      }
      if (key == chunk) {
        // Another thread may still be creating it: wait for the pointer.
        std::atomic<uint8_t>* data;
        while (!(data = slots_[slot].data.load(std::memory_order_acquire))) {
        }
        return data;
      }
    }
    return nullptr;
  }

  Slot slots_[kSlots];
};

PageProtectionTable& Protections() {
  static PageProtectionTable table;
  return table;
}

uint8_t ProtectionValue(PageAccess access) { return uint8_t(static_cast<uint8_t>(access) + 1); }
}  // namespace
#endif

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  // Emulates Windows VirtualAlloc behavior:
  // - Reserve: create PROT_NONE mapping to hold address space
  // - Commit on existing reservation: mprotect to enable access (EEXIST path)
  // - New allocation: mmap with MAP_FIXED_NOREPLACE (never silently replace)
  const uint32_t prot_requested = ToPosixProtectFlags(access);

  // Determine initial protection based on allocation type
  int prot_initial = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      prot_initial = PROT_NONE;
      break;
    case AllocationType::kCommit:
    case AllocationType::kReserveCommit:
    default:
      prot_initial = static_cast<int>(prot_requested);
      break;
  }

    // On macOS, MAP_FIXED_NOREPLACE is unavailable. kCommit on a pre-reserved
    // range uses mprotect to avoid clobbering the existing reservation. Do not
    // widen sub-host-page requests here - higher-level guest heaps must reconcile
    // all guest permissions sharing a host page first.
#if REX_PLATFORM_MAC
  if (base_address != nullptr && allocation_type == AllocationType::kCommit) {
    const size_t host_page = page_size();
    const uintptr_t address = reinterpret_cast<uintptr_t>(base_address);
    if ((address % host_page) != 0 || (length % host_page) != 0) {
      return nullptr;
    }
    if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
      return base_address;
    }
    return nullptr;
  }
#endif

  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if REX_PLATFORM_MAC
  if (access == PageAccess::kExecuteReadWrite || access == PageAccess::kExecuteReadOnly) {
    flags |= MAP_JIT;
  }
  if (base_address) {
    flags |= MAP_FIXED;
  }
#elif defined(MAP_FIXED_NOREPLACE)
  if (base_address) {
    flags |= MAP_FIXED_NOREPLACE;
  }
#else
  if (base_address) {
    flags |= MAP_FIXED;
  }
#endif

  void* result = mmap(base_address, length, prot_initial, flags, -1, 0);
  if (result != MAP_FAILED) {
    if (!base_address || result == base_address) {
#if REX_PLATFORM_LINUX
      Protections().Set(result, length,
                        ProtectionValue(allocation_type == AllocationType::kReserve ? PageAccess::kNoAccess
                                                                                    : access));
#endif
      return result;
    }
    // Kernels before 4.17 may ignore MAP_FIXED_NOREPLACE and map elsewhere
    // instead of reporting EEXIST. A relocated mapping is not a fixed commit:
    // the guest would keep using the old, potentially protected stack pages.
    munmap(result, length);
    errno = EEXIST;
  }
#if defined(MAP_FIXED_NOREPLACE) && REX_PLATFORM_LINUX
  // Handle EEXIST: address already has a mapping (e.g., from prior Reserve)
  // This is the "commit on existing reservation" path
  if (errno == EEXIST && base_address &&
      (allocation_type == AllocationType::kCommit ||
       allocation_type == AllocationType::kReserveCommit)) {
    // Verify the entire range is mapped before using mprotect
    if (IsRangeFullyMapped(base_address, length)) {
      if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
        Protections().Set(base_address, length, ProtectionValue(access));
        return base_address;
      }
    }
  }
#endif

  return nullptr;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  switch (deallocation_type) {
    case DeallocationType::kDecommit: {
      // Decommit: remove access first, then release physical pages
      if (mprotect(base_address, length, PROT_NONE) != 0) {
        return false;
      }
#if REX_PLATFORM_LINUX
      Protections().Set(base_address, length, ProtectionValue(PageAccess::kNoAccess));
#endif
#if defined(MADV_DONTNEED)
      (void)madvise(base_address, length, MADV_DONTNEED);
#endif
      return true;
    }
    case DeallocationType::kRelease: {
#if REX_PLATFORM_LINUX
      Protections().Set(base_address, length, 0);
#endif
      return munmap(base_address, length) == 0;
    }
    default:
      // how we get here? :(
      assert_always();
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }

#if REX_PLATFORM_MAC
  // mprotect doesn't report the previous protection. Query the Mach region
  // before changing it so this matches VirtualProtect's out parameter.
  if (out_old_access) {
    size_t old_region_length = 0;
    QueryProtect(base_address, old_region_length, *out_old_access);
  }
#elif REX_PLATFORM_LINUX
  // NOTE(tomc): we may want to look at doing this differently. it should work for now
  //             but there is a TOCTOU window between reading and changing.
  //             This really shouldn't be an issue since VirtualProtect on Windows isn't truly
  //             atomic in a mutli-threaded process either, but it's something to be aware of.
  // Query old access before changing, if the caller needs it
  if (out_old_access && !Protections().Get(base_address, *out_old_access)) {
    LinuxMapEntry e;
    if (FindEntryForAddress(base_address, e)) {
      *out_old_access = PermsToPageAccess(e.perms);
    }
  }
#endif

  uint32_t prot = ToPosixProtectFlags(access);
  int ret = mprotect(base_address, length, prot);
#if REX_PLATFORM_LINUX
  // A failed mprotect may have changed part of the range: forget it rather than guess.
  Protections().Set(base_address, length, ret == 0 ? ProtectionValue(access) : 0);
#endif
  if (ret != 0) {
    REXSYS_ERROR("mprotect({}, 0x{:X}, {}) failed: {} ({})", base_address, length, prot,
                 strerror(errno), errno);
  }
  return ret == 0;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if REX_PLATFORM_MAC
  mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(base_address);
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name;

  kern_return_t kr =
      mach_vm_region(mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
                     reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
  if (kr != KERN_SUCCESS) {
    return false;
  }
  if (address > reinterpret_cast<mach_vm_address_t>(base_address)) {
    return false;
  }

  length = static_cast<size_t>((address + region_size) -
                               reinterpret_cast<mach_vm_address_t>(base_address));

  if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) ==
      (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) {
    access_out = PageAccess::kExecuteReadWrite;
  } else if ((info.protection & (VM_PROT_READ | VM_PROT_EXECUTE)) ==
             (VM_PROT_READ | VM_PROT_EXECUTE)) {
    access_out = PageAccess::kExecuteReadOnly;
  } else if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE)) {
    access_out = PageAccess::kReadWrite;
  } else if (info.protection & VM_PROT_READ) {
    access_out = PageAccess::kReadOnly;
  } else {
    access_out = PageAccess::kNoAccess;
  }
  return true;
#elif !REX_PLATFORM_LINUX
  access_out = PageAccess::kNoAccess;
  length = 0;
  return false;
#else
  access_out = PageAccess::kNoAccess;
  length = 0;

  if (Protections().Get(base_address, access_out)) {
    const size_t page = page_size();
    length = page - reinterpret_cast<uintptr_t>(base_address) % page;
    return true;
  }

  LinuxMapEntry e;
  if (!FindEntryForAddress(base_address, e)) {
    return false;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  length = static_cast<size_t>(e.end - addr);
  access_out = PermsToPageAccess(e.perms);

  return true;
#endif
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
#if REX_PLATFORM_ANDROID
  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? static_cast<FileMappingHandle>(sharedmem_fd)
                             : kFileMappingHandleInvalid;
  }

  // Use /dev/ashmem on API versions below 26, which added ASharedMemory.
  // /dev/ashmem was disabled on API 29 for apps targeting it.
  // https://chromium.googlesource.com/chromium/src/+/master/third_party/ashmem/ashmem-dev.c
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), rex::countof(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ashmem_fd);
#else
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;
  auto full_path = MakeShmName(path);
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    return kFileMappingHandleInvalid;
  }
  if (rex_ftruncate64(ret, static_cast<off_t>(length)) != 0) {
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ret);
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  close(static_cast<int>(handle));
#if !REX_PLATFORM_ANDROID
  auto full_path = MakeShmName(path);
  shm_unlink(full_path.c_str());
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  // file_offset must be page-aligned
  const size_t page = page_size();
  if (file_offset % page != 0) {
    return nullptr;
  }

  int flags = MAP_SHARED;

  // For file views, we need MAP_FIXED to replace existing reservations.
  // The emulator reserves address space first, then maps file views into it.
  // MAP_FIXED_NOREPLACE would fail with EEXIST in this case.
  if (base_address) {
    flags |= MAP_FIXED;
  }

  uint32_t prot = ToPosixProtectFlags(access);
  void* result = rex_mmap64(base_address, length, prot, flags, static_cast<int>(handle),
                            static_cast<off_t>(file_offset));
  if (result == MAP_FAILED) {
    return nullptr;
  }

  // Verify we got the address we asked for
  if (base_address && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

#if REX_PLATFORM_LINUX
  Protections().Set(result, length, ProtectionValue(access));
#endif
  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
#if REX_PLATFORM_LINUX
  Protections().Set(base_address, length, 0);
#endif
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace rex
