#include <mach/mach.h>
#include <cstdio>
#include <vector>
static double fp() { task_vm_info_data_t i; mach_msg_type_number_t c = TASK_VM_INFO_COUNT; task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&i, &c); return i.phys_footprint / 1048576.0; }
int main() {
  std::printf("start %.1f\n", fp());
  for (size_t sz : {size_t(64) << 10, size_t(1) << 20, size_t(4) << 20}) {
    std::vector<std::vector<unsigned char>> v;
    for (size_t t = 0; t < (size_t(27) << 20) / sz; ++t) v.emplace_back(sz, 1);
    double a = fp();
    for (auto& x : v) std::vector<unsigned char>().swap(x);
    std::printf("blocks of %zu KiB: allocated 27 MiB -> footprint %.1f, after free %.1f\n", sz >> 10, a, fp());
  }
}
