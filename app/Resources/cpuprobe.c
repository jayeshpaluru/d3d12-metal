// SPDX-License-Identifier: LGPL-2.1-or-later
// Prints the x86 CPU features Rosetta advertises to x86-64 programs: "avx=1 avx2=1 fma=1 bmi2=1 f16c=1".
// Built for x86_64 only (clang -arch x86_64) and run under Rosetta by `d3d12metal doctor`.
#include <stdio.h>

int main(void)
{
    __builtin_cpu_init();
    printf("avx=%d avx2=%d fma=%d bmi2=%d f16c=%d\n", !!__builtin_cpu_supports("avx"), !!__builtin_cpu_supports("avx2"),
           !!__builtin_cpu_supports("fma"), !!__builtin_cpu_supports("bmi2"), !!__builtin_cpu_supports("f16c"));
    return 0;
}
