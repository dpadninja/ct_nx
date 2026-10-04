/* prof_stubs.s -- timing GOT hooks for func_prof.c (diagnostic)
 *
 * PROF_MAX identical stubs, each 256-byte aligned: stub N is at
 * prof_stub_base + N*256. Each stub saves the argument registers (x0-x8,
 * d0-d7), copies 64 bytes of the caller's outgoing stack arguments (for
 * functions with more than 8 register arguments), calls prof_enter(id),
 * calls the original function prof_orig[id], keeps the return registers
 * (x0, x1, d0-d3), calls prof_leave(start, id) and returns.
 *
 * Frame (0x100 bytes): 0x00 stack-arg copy | 0x40 x29/x30 | 0x50 x0-x7 |
 *                      0x90 d0-d7 | 0xd0 x8 | 0xd8 start
 */
  .text
  .altmacro
  .macro PROF_STUB id
  .balign 256
  sub sp, sp, #0x100
  stp x29, x30, [sp, #0x40]
  add x29, sp, #0x40
  stp x0, x1, [sp, #0x50]
  stp x2, x3, [sp, #0x60]
  stp x4, x5, [sp, #0x70]
  stp x6, x7, [sp, #0x80]
  stp d0, d1, [sp, #0x90]
  stp d2, d3, [sp, #0xa0]
  stp d4, d5, [sp, #0xb0]
  stp d6, d7, [sp, #0xc0]
  str x8, [sp, #0xd0]
  ldp x9, x10, [sp, #0x100]
  stp x9, x10, [sp, #0x00]
  ldp x9, x10, [sp, #0x110]
  stp x9, x10, [sp, #0x10]
  ldp x9, x10, [sp, #0x120]
  stp x9, x10, [sp, #0x20]
  ldp x9, x10, [sp, #0x130]
  stp x9, x10, [sp, #0x30]
  mov w0, #\id
  bl prof_enter
  str x0, [sp, #0xd8]
  ldp x0, x1, [sp, #0x50]
  ldp x2, x3, [sp, #0x60]
  ldp x4, x5, [sp, #0x70]
  ldp x6, x7, [sp, #0x80]
  ldp d0, d1, [sp, #0x90]
  ldp d2, d3, [sp, #0xa0]
  ldp d4, d5, [sp, #0xb0]
  ldp d6, d7, [sp, #0xc0]
  ldr x8, [sp, #0xd0]
  adrp x16, prof_orig
  add x16, x16, :lo12:prof_orig
  ldr x16, [x16, #(\id * 8)]
  blr x16
  stp x0, x1, [sp, #0x50]
  stp d0, d1, [sp, #0x90]
  stp d2, d3, [sp, #0xa0]
  ldr x0, [sp, #0xd8]
  mov w1, #\id
  bl prof_leave
  ldp x0, x1, [sp, #0x50]
  ldp d0, d1, [sp, #0x90]
  ldp d2, d3, [sp, #0xa0]
  ldp x29, x30, [sp, #0x40]
  add sp, sp, #0x100
  ret
  .endm

  .global prof_stub_base
  .type prof_stub_base, %function
  .balign 256
prof_stub_base:
  .set prof_i, 0
  .rept 3072
  PROF_STUB %prof_i
  .set prof_i, prof_i + 1
  .endr
