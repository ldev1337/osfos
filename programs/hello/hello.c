/* First freestanding user program. The user ABI is intentionally tiny. */
__attribute__((noreturn)) void _start(void) {
  for (;;) {
    __asm__ volatile("pause");
  }
}
