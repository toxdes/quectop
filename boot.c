/* Process start, before any other code: applies a static PIE's relocations,
 * makes RELRO read-only and seeds the stack protector's guard. */
#pragma GCC diagnostic ignored "-Wunused-function" /* linux.h helpers */
#include "linux.h"

#define AT_PHDR 3
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_RANDOM 25 /* 16 random bytes from the kernel */
#define PT_GNU_RELRO 0x6474e552

void entry(long *stack) __attribute__((noreturn));

unsigned long __stack_chk_guard;
void __stack_chk_fail(void) __attribute__((noreturn));
void __stack_chk_fail(void) {
  static const char message[] = "stack smashing detected\n";
  call4(N_write, 2, (long)message, sizeof(message) - 1, 0);
  __builtin_trap();
}

#if defined(__PIE__)
#if defined(__arm__)
struct reloc { u32 offset, info; }; /* Elf32_Rel: the addend is in place */
struct segment { u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; };
#define DT_RELOCS 17 /* DT_REL */
#define DT_RELOCS_BYTES 18
#define RELATIVE(info) (((info) & 0xff) == 23)
#else
struct reloc { u64 offset, info; i64 addend; }; /* Elf64_Rela */
struct segment { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; };
#define DT_RELOCS 7 /* DT_RELA */
#define DT_RELOCS_BYTES 8
#if defined(__x86_64__)
#define RELATIVE(info) ((u32)(info) == 8)
#else
#define RELATIVE(info) ((u32)(info) == 1027)
#endif
#endif
#define DT_RELR 36

/* Linked at address 0, so these hidden symbols' addresses give the base. */
extern const unsigned long _DYNAMIC[] __attribute__((visibility("hidden")));
extern const char __ehdr_start[] __attribute__((visibility("hidden")));

static void relocate(unsigned long base, const struct segment *s, unsigned long n,
                     unsigned long page_size) {
  const struct reloc *r = 0;
  unsigned long bytes = 0;
  for (const unsigned long *d = _DYNAMIC; d[0]; d += 2) {
    if (d[0] == DT_RELOCS)
      r = (const struct reloc *)(base + d[1]);
    else if (d[0] == DT_RELOCS_BYTES)
      bytes = d[1];
    else if (d[0] == DT_RELR)
      __builtin_trap();
  }
  for (unsigned long i = 0; i < bytes / sizeof(*r); ++i) {
    if (!RELATIVE(r[i].info))
      __builtin_trap();
    unsigned long *at = (unsigned long *)(base + r[i].offset);
#if defined(__arm__)
    *at += base;
#else
    *at = base + (unsigned long)r[i].addend;
#endif
  }
  unsigned long page_mask = ~(page_size - 1);
  for (; n--; ++s)
    if (s->type == PT_GNU_RELRO) {
      unsigned long start = (base + s->vaddr) & page_mask,
                    end = (base + s->vaddr + s->memsz) & page_mask;
      if (end > start &&
          call4(N_mprotect, (long)start, (long)(end - start), 1, 0) < 0)
        __builtin_trap();
    }
}
#endif

void start(long *stack) __attribute__((noreturn, no_stack_protector));
void start(long *stack) {
  long *aux = stack + 1 + stack[0] + 1; /* past argv */
  while (*aux++) /* past the environment */
    ;
  unsigned long phdr = 0, phnum = 0, page_size = 0;
  const u8 *random = 0;
  for (; aux[0]; aux += 2)
    if (aux[0] == AT_PHDR)
      phdr = (unsigned long)aux[1];
    else if (aux[0] == AT_PHNUM)
      phnum = (unsigned long)aux[1];
    else if (aux[0] == AT_PAGESZ)
      page_size = (unsigned long)aux[1];
    else if (aux[0] == AT_RANDOM)
      random = (const u8 *)aux[1];
#if defined(__PIE__)
  if (!page_size || (page_size & (page_size - 1)))
    __builtin_trap();
  relocate((unsigned long)__ehdr_start, (const struct segment *)phdr, phnum,
           page_size);
#else
  (void)phdr;
  (void)phnum;
  (void)page_size;
#endif
  if (!random)
    __builtin_trap();
  unsigned long guard = 0;
  for (unsigned i = 1; i < sizeof(guard); ++i) /* byte 0 stays 0, as in libc */
    guard |= (unsigned long)random[i] << 8 * i;
  __stack_chk_guard = guard;
  entry(stack);
}
