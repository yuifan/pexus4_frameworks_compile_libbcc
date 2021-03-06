/* ===-- divsi3.c - Implement __divsi3 -------------------------------------===
 *
 *                     The LLVM Compiler Infrastructure
 *
 * This file is distributed under the University of Illinois Open Source
 * License. See LICENSE.TXT for details.
 *
 * ===----------------------------------------------------------------------===
 *
 * This file implements __divsi3 for the compiler_rt library.
 *
 * ===----------------------------------------------------------------------===
 */

#if !defined(__GNUC__) || __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 7) || defined(ARCH_X86) || defined(ARCH_MIPS)
// ARM gcc >= 4.7 implements this in libgcc
#include "int_lib.h"

su_int __udivsi3(su_int n, su_int d);

/* Returns: a / b */

si_int
__divsi3(si_int a, si_int b)
{
    const int bits_in_word_m1 = (int)(sizeof(si_int) * CHAR_BIT) - 1;
    si_int s_a = a >> bits_in_word_m1;           /* s_a = a < 0 ? -1 : 0 */
    si_int s_b = b >> bits_in_word_m1;           /* s_b = b < 0 ? -1 : 0 */
    a = (a ^ s_a) - s_a;                         /* negate if s_a == -1 */
    b = (b ^ s_b) - s_b;                         /* negate if s_b == -1 */
    s_a ^= s_b;                                  /* sign of quotient */
    return (__udivsi3(a, b) ^ s_a) - s_a;        /* negate if s_a == -1 */
}
#endif
