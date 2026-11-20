#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <lz4.h>
#include <lz4hc.h>
#include <vector>
int main(int argc, char **argv) {
  if (argc != 3)
    return 1;
  std::ifstream f(argv[1], std::ios::binary);
  if (!f)
    return 2;
  std::vector<char> src((std::istreambuf_iterator<char>(f)), {});
  if (src.size() > LZ4_MAX_INPUT_SIZE)
    return 3;
  int n = src.size(), bound = LZ4_compressBound(n);
  std::vector<char> dst(bound);
  int size = LZ4_compress_HC(src.data(), dst.data(), n, bound, 12);
  if (!size)
    return 4;
  std::ofstream o(argv[2], std::ios::binary);
  o.write(dst.data(), size);
  return o ? 0 : 5;
}
