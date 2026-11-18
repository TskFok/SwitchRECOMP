/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */

/* Original SDK-free AArch64 fixture. x19 is the TLS message buffer. x24 is the
   failure checkpoint printed if an assertion fails. */
.text
  b main
  .space 0x7c

.macro header type, command, raw=16, words=16
  mov w9, #\type
  str w9, [x19]
  mov w9, #\words
  str w9, [x19, #4]
  movz w9, #0x4653
  movk w9, #0x4943, lsl #16
  str w9, [x19, #\raw]
  str wzr, [x19, #(\raw+4)]
  mov w9, #\command
  str w9, [x19, #(\raw+8)]
  str wzr, [x19, #(\raw+12)]
.endm

.macro send handle, result=24
  mov w0, \handle
  svc #0x21
  cbnz w0, fail
  ldr w9, [x19, #\result]
  cbnz w9, fail
.endm

.macro close handle
  mov w0, \handle
  svc #0x16
  cbnz w0, fail
  add x28, x28, #1
.endm

/* Record order is GUEST_FIELDS in run.py. */
.macro observe slot, reg
  str \reg, [sp, #(0x100 + 8*\slot)]
.endm

main:
  sub sp, sp, #0x400
  mrs x19, tpidrro_el0
  mov x28, #0
  movz w27, #0x4132
  movk w27, #0x4e48, lsl #16
  observe 0, x27

  mov w24, #1
  mrs x9, cntfrq_el0
  observe 1, x9
  movz x10, #0xf800
  movk x10, #0x124, lsl #16
  cmp x9, x10
  b.ne fail
  svc #0x1e
  mov x25, x0
  movz w0, #0x4240
  movk w0, #0xf, lsl #16
  mov x26, x0
  svc #0x0b
  observe 2, x0
  cmp x0, x26
  b.ne fail
  svc #0x1e
  cmp x0, x25
  b.ls fail
  sub x10, x0, x25
  observe 20, x10
  mrs x9, cntvct_el0
  cmp x9, x0
  b.lo fail
  sub x10, x9, x0
  observe 21, x10
  mov x0, #-2
  svc #0x0b
  observe 3, x0
  cmn x0, #2
  b.ne fail

  mov w24, #2
  adr x1, sm_name
  svc #0x1f
  cbnz w0, fail
  mov w20, w1
  header 4, 1
  adr x9, time_name
  ldr x9, [x9]
  str x9, [x19, #32]
  send w20
  ldr w21, [x19, #12]
  header 4, 300
  send w21
  ldr x25, [x19, #32]
  observe 22, x25
  cbz x25, fail

  /* Existing subclock, timezone and shared-memory paths under the same alias. */
  header 4, 0
  send w21
  ldr w22, [x19, #12]
  header 4, 0
  send w22
  ldr x9, [x19, #32]
  sub x9, x9, x25
  observe 23, x9
  add x9, x9, #5
  cmp x9, #10
  b.hi fail
  close w22
  header 4, 2
  send w21
  ldr w22, [x19, #12]
  header 4, 0
  send w22
  ldrb w9, [x19, #40]
  cmp w9, #1
  b.ne fail
  close w22
  header 4, 3
  send w21
  ldr w22, [x19, #12]
  header 4, 0
  send w22
  ldrb w9, [x19, #32]
  cmp w9, #'U'
  b.ne fail
  close w22
  header 4, 20
  send w21
  ldr w22, [x19, #12]
  cbz w22, fail
  close w22
  close w21

  mov w24, #3
  header 4, 1
  adr x9, fsp_name
  ldr x9, [x9]
  str x9, [x19, #32]
  send w20
  ldr w21, [x19, #12]
  close w20
  header 4, 18
  send w21
  ldr w22, [x19, #12]
  close w21
  /* Open a file through a domain, then clone the session and drop the original. */
  header 5, 0
  send w22
  ldr w9, [x19, #32]
  cmp w9, #1
  b.ne fail
  header 5, 2
  send w22
  ldr w21, [x19, #12]
  close w22
  header 4, 8, 32, 20
  /* One X descriptor, 8-byte path, address in the low 32-bit code area. */
  mov w9, #4
  movk w9, #1, lsl #16
  str w9, [x19]
  mov w9, #8
  lsl w9, w9, #16
  str w9, [x19, #8]
  adr x9, file_path
  str w9, [x19, #12]
  movz w9, #1
  movk w9, #20, lsl #16
  str w9, [x19, #16]
  mov w9, #1
  str w9, [x19, #20]
  str xzr, [x19, #24]
  str w9, [x19, #48]
  send w21, 40
  ldr w22, [x19, #48]
  cmp w22, #2
  b.ne fail
  header 4, 4, 32
  movz w9, #1
  movk w9, #16, lsl #16
  str w9, [x19, #16]
  str w22, [x19, #20]
  str xzr, [x19, #24]
  send w21, 40
  ldr x9, [x19, #48]
  observe 4, x9
  cmp x9, #6
  b.ne fail

  /* A short read into a stack B descriptor at a >32-bit guest address. */
  header 4, 0, 48, 24
  mov w9, #4
  movk w9, #0x100, lsl #16
  str w9, [x19]
  mov w9, #8
  str w9, [x19, #8]
  movz x10, #0xa5a5, lsl #48
  str x10, [sp, #32]
  add x9, sp, #32
  str w9, [x19, #12]
  ubfx x10, x9, #32, #4
  lsl w10, w10, #28
  ubfx x9, x9, #36, #3
  orr w10, w10, w9, lsl #2
  str w10, [x19, #16]
  movz w9, #1
  movk w9, #40, lsl #16
  str w9, [x19, #32]
  str w22, [x19, #36]
  str xzr, [x19, #40]
  str xzr, [x19, #64]
  str xzr, [x19, #72]
  mov w9, #8
  str x9, [x19, #80]
  send w21, 40
  ldr x9, [x19, #48]
  observe 5, x9
  cmp x9, #6
  b.ne fail
  ldrb w9, [sp, #32]
  cmp w9, #'h'
  b.ne fail
  ldrb w9, [sp, #37]
  cmp w9, #10
  b.ne fail
  ldr x9, [sp, #32]
  observe 6, x9
  /* Domain close must release the Native resource only once. */
  header 4, 0, 32
  mov w9, #2
  str w9, [x19, #16]
  str w22, [x19, #20]
  str xzr, [x19, #24]
  send w21, 40
  header 4, 4, 32
  movz w9, #1
  movk w9, #16, lsl #16
  str w9, [x19, #16]
  str w22, [x19, #20]
  str xzr, [x19, #24]
  mov w0, w21
  svc #0x21
  cbnz w0, fail
  ldr w9, [x19, #40]
  observe 7, x9
  close w21

  mov w24, #4
  svc #0x45
  cbnz w0, fail
  mov w21, w1
  mov w22, w2
  str w22, [sp]
  mov w0, w21
  svc #0x11
  cbnz w0, fail
  mov x1, sp
  mov w2, #1
  mov x3, #0
  svc #0x18
  cbnz w0, fail
  observe 8, x1
  mov w0, w22
  svc #0x17
  cbnz w0, fail
  mov x1, sp
  mov w2, #1
  mov x3, #0
  svc #0x18
  observe 9, x0
  mov w9, #0xea01
  cmp w0, w9
  b.ne fail
  mov w0, w21
  svc #0x12
  cbnz w0, fail
  close w21
  close w22

  /* Cancel before WaitSynchronization must be delivered on the next wait. */
  movz w0, #0x8000
  movk w0, #0xffff, lsl #16
  svc #0x19
  cbnz w0, fail
  mov x1, sp
  mov w2, #0
  mov x3, #0
  svc #0x18
  observe 10, x0
  mov w9, #0xec01
  cmp w0, w9
  b.ne fail

  mov w24, #5
  adr x1, worker
  add x2, sp, #0x40
  sub x3, sp, #0x100
  stp x3, x19, [x2]
  stp x27, xzr, [x2, #16]
  mov w4, #32
  mov w5, #1
  svc #0x08
  cbnz w0, fail
  mov w21, w1
  svc #0x0c
  cbnz w0, fail
  observe 11, x1
  mov w2, w21
  svc #0x0e
  cbnz w0, fail
  observe 12, x1
  observe 13, x2
  mov w0, w21
  svc #0x09
  cbnz w0, fail
  str w21, [sp]
  mov x1, sp
  mov w2, #1
  mov x3, #-1
  svc #0x18
  cbnz w0, fail
  observe 14, x1
  ldr x9, [sp, #0x58]
  observe 15, x9
  mov w0, w21
  svc #0x09
  observe 19, x0
  close w21
  mov w0, w21
  svc #0x16
  observe 17, x0

  mov w24, #6
  str wzr, [sp, #8]
  mov w0, #0
  add x1, sp, #8
  mov w2, #0
  svc #0x1a
  cbnz w0, fail
  add x0, sp, #8
  svc #0x1b
  cbnz w0, fail
  ldr w9, [sp, #8]
  cbnz w9, fail
  observe 16, x9
  observe 18, x28
  /* Emit the observed values in one SDK-free log call. */
  add x4, sp, #0x100
  add x5, sp, #0x200
  movz w9, #0x484e
  movk w9, #0x3241, lsl #16
  str w9, [x5], #4
  mov w7, #24
format_word:
  ldr x6, [x4], #8
  mov w9, #' '
  strb w9, [x5], #1
  mov w8, #16
format_digit:
  lsr x9, x6, #60
  cmp w9, #9
  add w10, w9, #'0'
  add w11, w9, #('a'-10)
  csel w9, w10, w11, ls
  strb w9, [x5], #1
  lsl x6, x6, #4
  subs w8, w8, #1
  b.ne format_digit
  subs w7, w7, #1
  b.ne format_word
  add x0, sp, #0x200
  sub x1, x5, x0
  svc #0x27

  adr x0, pass_message
  mov w1, #(pass_end-pass_message)
  svc #0x27
  svc #0x07
1: b 1b

worker:
  ldp x9, x10, [x0]
  mov x11, sp
  cmp x11, x9
  b.ne worker_exit
  mrs x11, tpidrro_el0
  cbz x11, worker_exit
  cmp x11, x10
  b.eq worker_exit
  ldr x9, [x0, #16]
  str x9, [sp, #-16]!
  ldr x10, [sp], #16
  str x10, [x0, #24]
worker_exit:
  svc #0x0a
  ret

fail:
  adr x0, fail_message
  mov w1, #(fail_end-fail_message)
  svc #0x27
  add w9, w24, #'0'
  strb w9, [sp]
  mov x0, sp
  mov w1, #1
  svc #0x27
  svc #0x07
1: b 1b

.balign 8
sm_name: .ascii "sm:\0\0\0\0\0\0\0\0\0"
time_name: .ascii "time:a\0\0"
fsp_name: .ascii "fsp-srv\0"
file_path: .asciz "one.txt"
pass_message: .ascii "NHA contract passed"
pass_end:
fail_message: .ascii "NHA contract failed at checkpoint"
fail_end:
.balign 4
