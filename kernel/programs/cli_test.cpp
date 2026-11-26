#include <kernel/memory/heap.h>
#include <kernel/scheduler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace Kernel::Apps {
namespace {
struct MemoryLifetimeProbe {
  volatile uint32_t ready;
  volatile uint32_t release;
  uint64_t tid;
};
void memory_lifetime_worker(void *opaque) {
  auto *probe = static_cast<MemoryLifetimeProbe *>(opaque);
  void *memory = thread_malloc(128);
  if (memory)
    memset(memory, 0xA5, 128);
  __atomic_store_n(&probe->ready, memory ? 1u : 2u, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&probe->release, __ATOMIC_ACQUIRE))
    Kernel::Scheduler::thread_yield();
}
} // namespace

int cli_args_test(int argc, char **argv, void *) {
  printf("process: pid=%llu ppid=%llu\n",
         static_cast<unsigned long long>(Kernel::Scheduler::current_process_id()),
         static_cast<unsigned long long>(Kernel::Scheduler::current_parent_process_id()));
  printf("argc=%d\n", argc);
  for (int i = 0; i < argc; ++i)
    printf("argv[%d]=<%s>\n", i, argv[i]);
  return 0;
}

int cli_exitcode_test(int argc, char **argv, void *) {
  int status = 0;
  if (argc > 1)
    status = atoi(argv[1]);
  printf("exitcode: returning %d\n", status);
  return status;
}

int cli_stdio_test(int, char **, void *) {
  printf("stdio: printf/vprintf formatting check: %s %d %#x %.2f\n",
         "ready", 42, 0xBEEF, 3.14159);
  char name[32]{};
  int value = 0;
  printf("Enter one word and an integer: ");
  fflush(stdout);
  const int scanned = scanf("%31s %d", name, &value);
  if (scanned != 2) {
    fprintf(stderr, "scanf failed: assigned %d field(s)\n", scanned);
    return 2;
  }
  printf("scanf: name=<%s> value=%d\n", name, value);

  FILE *file = fopen("/stdio-test.tmp", "w+");
  if (!file) {
    perror("fopen");
    return 3;
  }
  const char suffix[] = "source=fwrite\n";
  if (fprintf(file, "name=%s value=%d\n", name, value) < 0 ||
      fwrite(suffix, 1, sizeof(suffix) - 1, file) != sizeof(suffix) - 1 ||
      fflush(file) != 0) {
    perror("fprintf/fflush");
    fclose(file);
    remove("/stdio-test.tmp");
    return 4;
  }
  const long end_position = ftell(file);
  if (end_position < 0 || fseek(file, 0, SEEK_SET) != 0) {
    perror("fseek");
    fclose(file);
    remove("/stdio-test.tmp");
    return 5;
  }
  char contents[96]{};
  char parsed_name[32]{};
  int parsed_value = 0;
  const int parsed = fscanf(file, "name=%31s value=%d", parsed_name, &parsed_value);
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    remove("/stdio-test.tmp");
    return 6;
  }
  const size_t read_count = fread(contents, 1, sizeof(contents) - 1, file);
  contents[read_count] = '\0';
  const bool eof_seen = feof(file) != 0;
  const bool read_error = ferror(file) != 0;
  const int close_result = fclose(file);
  printf("file: wrote=%ld bytes parsed=%d<%s,%d> read=%zu eof=%d error=%d close=%d\n",
         end_position, parsed, parsed_name, parsed_value, read_count,
         eof_seen, read_error, close_result);
  printf("file contents: %s", contents);
  const int remove_result = remove("/stdio-test.tmp");
  printf("remove: %s\n", remove_result == 0 ? "OK" : "FAILED");

  double parsed_float = 0.0;
  float parsed_float32 = 0.0f;
  const int parsed_double_count =
      sscanf("3.125e1", "%lf", &parsed_float);
  const int parsed_float_count =
      sscanf("-0.625", "%f", &parsed_float32);
  printf("scanf float: double=%d<%.3f> float=%d<%.3f>\n",
         parsed_double_count, parsed_float, parsed_float_count,
         static_cast<double>(parsed_float32));

  char formatted[64];
  const int formatted_length =
      snprintf(formatted, sizeof(formatted), "pid=%llu value=%d",
               static_cast<unsigned long long>(Kernel::Scheduler::current_process_id()),
               value);
  printf("snprintf: length=%d text=<%s>\n", formatted_length, formatted);
  return parsed != 2 || strcmp(parsed_name, name) != 0 || parsed_value != value ||
                 parsed_double_count != 1 || parsed_float_count != 1 ||
                 parsed_float < 31.249 || parsed_float > 31.251 ||
                 parsed_float32 < -0.626f || parsed_float32 > -0.624f ||
                 read_error || close_result != 0 || remove_result != 0
             ? 7
             : 0;
}

int cli_memory_lifetime_test(int, char **, void *) {
  const uint64_t pid = Kernel::Scheduler::current_process_id();
  const uint64_t tid = Kernel::Scheduler::current_thread_id();
  void *process_memory = malloc(96);
  void *thread_memory = thread_malloc(64);
  if (!process_memory || !thread_memory) {
    free(process_memory);
    free(thread_memory);
    printf("memlife: initial allocation failed\n");
    return 1;
  }
  memset(process_memory, 0x11, 96);
  memset(thread_memory, 0x22, 64);

  MemoryLifetimeProbe probe{};
  const int worker_id = Kernel::Scheduler::thread_create(
      memory_lifetime_worker, &probe);
  if (worker_id <= 0) {
    free(process_memory);
    free(thread_memory);
    printf("memlife: worker creation failed\n");
    return 2;
  }
  probe.tid = static_cast<uint64_t>(worker_id);
  for (unsigned spin = 0; spin < 1000000 &&
                          !__atomic_load_n(&probe.ready, __ATOMIC_ACQUIRE);
       ++spin)
    Kernel::Scheduler::thread_yield();
  const uint32_t ready = __atomic_load_n(&probe.ready, __ATOMIC_ACQUIRE);
  const size_t worker_live =
      Kernel::Memory::g_kheap.get_thread_allocated_size(probe.tid);
  const size_t process_live =
      Kernel::Memory::g_kheap.get_process_allocated_size(pid);
  const size_t thread_live =
      Kernel::Memory::g_kheap.get_thread_allocated_size(tid);
  __atomic_store_n(&probe.release, 1u, __ATOMIC_RELEASE);
  for (unsigned spin = 0; spin < 1000000 &&
                          Kernel::Memory::g_kheap.get_thread_allocated_size(
                              probe.tid) != 0;
       ++spin)
    Kernel::Scheduler::thread_yield();
  const size_t worker_after_exit =
      Kernel::Memory::g_kheap.get_thread_allocated_size(probe.tid);
  free(process_memory);
  free(thread_memory);

  printf("memlife: pid=%llu tid=%llu worker=%llu ready=%u\n",
         static_cast<unsigned long long>(pid),
         static_cast<unsigned long long>(tid),
         static_cast<unsigned long long>(probe.tid), ready);
  printf("memlife: process-owned=%zu current-thread-owned=%zu worker-owned=%zu after-exit=%zu\n",
         process_live, thread_live, worker_live, worker_after_exit);
  const bool ok = ready == 1 && process_live > 0 && thread_live > 0 &&
                  worker_live > 0 && worker_after_exit == 0;
  printf("memlife: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 3;
}

} // namespace Kernel::Apps
