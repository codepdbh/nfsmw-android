// Valid ARM64 ELF with no Vulkan implementation: the loader must reject it without a game crash.
extern "C" __attribute__((visibility("default"))) int nfsmw_driver_test_fixture() { return 1; }
