/* census_stubs.s -- argument-preserving, timing GOT hooks for sprite_census.c
 *
 * Each stub saves every argument register (x0-x8, d0-d7), calls
 * census_enter() for a start timestamp, calls the original function with the
 * original arguments, then reports (start, id, caller return address) to
 * census_leave() and returns the original result (x0) to the caller.
 * Every hooked function returns a pointer (or nothing), so x0 is the only
 * result register that needs preserving.
 *
 * Frame layout (0xb0 bytes):
 *   0x00 x29/x30   0x10..0x48 x0-x7   0x50..0x88 d0-d7   0x90 x8   0xa0 start
 */
  .text
  .macro CENSUS_STUB id
  .global census_stub_\id
  .type census_stub_\id, %function
  .p2align 2
census_stub_\id:
  stp x29, x30, [sp, #-0xb0]!
  mov x29, sp
  stp x0, x1, [sp, #0x10]
  stp x2, x3, [sp, #0x20]
  stp x4, x5, [sp, #0x30]
  stp x6, x7, [sp, #0x40]
  stp d0, d1, [sp, #0x50]
  stp d2, d3, [sp, #0x60]
  stp d4, d5, [sp, #0x70]
  stp d6, d7, [sp, #0x80]
  str x8, [sp, #0x90]
  bl census_enter
  str x0, [sp, #0xa0]
  ldp x0, x1, [sp, #0x10]
  ldp x2, x3, [sp, #0x20]
  ldp x4, x5, [sp, #0x30]
  ldp x6, x7, [sp, #0x40]
  ldp d0, d1, [sp, #0x50]
  ldp d2, d3, [sp, #0x60]
  ldp d4, d5, [sp, #0x70]
  ldp d6, d7, [sp, #0x80]
  ldr x8, [sp, #0x90]
  adrp x16, census_orig
  add x16, x16, :lo12:census_orig
  ldr x16, [x16, #(\id * 8)]
  blr x16
  str x0, [sp, #0x10]
  ldr x0, [sp, #0xa0]
  mov w1, #\id
  ldr x2, [sp, #0x08]
  bl census_leave
  ldr x0, [sp, #0x10]
  ldp x29, x30, [sp], #0xb0
  ret
  .size census_stub_\id, . - census_stub_\id
  .endm

  CENSUS_STUB 0
  CENSUS_STUB 1
  CENSUS_STUB 2
  CENSUS_STUB 3
  CENSUS_STUB 4
  CENSUS_STUB 5
  CENSUS_STUB 6
  CENSUS_STUB 7
  CENSUS_STUB 8
  CENSUS_STUB 9
  CENSUS_STUB 10
  CENSUS_STUB 11
  CENSUS_STUB 12
  CENSUS_STUB 13
  CENSUS_STUB 14
  CENSUS_STUB 15
  CENSUS_STUB 16
  CENSUS_STUB 17
  CENSUS_STUB 18
  CENSUS_STUB 19
  CENSUS_STUB 20
  CENSUS_STUB 21
  CENSUS_STUB 22
  CENSUS_STUB 23
