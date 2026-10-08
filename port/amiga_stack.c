/*
 * amiga_stack.c - stack size of the AmigaOS port's programs.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * A Shell runs a command on a stack of the size set with its Stack command;
 * CreateNewProc() defaults to 4000 bytes (dos/dostags.h, NP_StackSize).
 * That is far too small for Dropbear: the post-quantum key exchanges have
 * single stack frames of 11 to 23 KB (gcc -fstack-usage on sntrup761.c,
 * mlkem768.c and libcrux_mlkem768_sha3.h), and the server's ML-KEM 768
 * step alone nests three of them:
 * crypto_kem_mlkem768_enc (7 KB) calls libcrux_ml_kem_ind_cpa_encrypt_60
 * (19 KB), which calls libcrux_ml_kem_matrix_sample_matrix_A_38 (15 KB),
 * about 41 KB before the frames of Dropbear's session loop above them.
 *
 * libnix's swapstack module (sources/misc/swapstack.c) runs before main()
 * and, if the task's stack is smaller than __stack bytes, allocates a stack
 * of that size and moves the program onto it; at exit it moves back and
 * frees it. libnix defines no __stack of its own, and the module is linked
 * only when something refers to __stkinit (swapstack.c:9-15), which the
 * pointer below does. Linking this file into a program is therefore all it
 * takes; nobody has to type a Stack command.
 *
 * AMIGA_STACK_SIZE is set by build.sh: 131072 for sshd and dropbearkey,
 * less for the small helper programs.
 */

#ifndef AMIGA_STACK_SIZE
#define AMIGA_STACK_SIZE 131072
#endif

/* swapstack.c's entry routine; referring to it links the module. */
extern void __stkinit(void);
void *__stkinit_ref = (void *)__stkinit;

/* The stack size the program needs, in bytes. */
unsigned long __stack = AMIGA_STACK_SIZE;
