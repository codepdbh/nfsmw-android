// Runs the production probe with the actual Android loader, independently of the game process.
// Arguments: hook-library directory, temporary directory, [driver directory, library name].
#include "../../android/app/src/main/cpp/android_diagnostico.cpp"
#include <cstdio>
int main(int argc, char** argv) {
  if (argc != 3 && argc != 5) return 2;
  std::string result = Probe(argv[1], argv[2], argc == 5 ? argv[3] : "", argc == 5 ? argv[4] : "");
  std::puts(result.c_str());
  return 0;
}
