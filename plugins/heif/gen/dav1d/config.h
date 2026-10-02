/* config.h de dav1d (normalement produit par meson) : Windows x64, MSVC, sans assembleur NASM. */
#pragma once
#define ARCH_AARCH64 0
#define ARCH_ARM 0
#define ARCH_LOONGARCH 0
#define ARCH_LOONGARCH32 0
#define ARCH_LOONGARCH64 0
#define ARCH_PPC64LE 0
#define ARCH_RISCV 0
#define ARCH_RV32 0
#define ARCH_RV64 0
#define ARCH_X86 1
#define ARCH_X86_32 0
#define ARCH_X86_64 1
#define CONFIG_16BPC 1
#define CONFIG_8BPC 1
#define CONFIG_LOG 0
#define ENDIANNESS_BIG 0
/* nasm absent : code C pur (les fonctions DSP assembleur ne sont pas compilées) */
#define HAVE_ASM 0
#define HAVE_AS_FUNC 0
#define HAVE_AS_ARCH_DIRECTIVE 0
#define HAVE_C11_GENERIC 1
#define HAVE_CLOCK_GETTIME 0
#define HAVE_DLSYM 0
#define HAVE_ELF_AUX_INFO 0
#define HAVE_GETAUXVAL 0
#define HAVE_IO_H 1
#define HAVE_MEMALIGN 0
#define HAVE_POSIX_MEMALIGN 0
#define HAVE_ALIGNED_ALLOC 0
#define HAVE_PTHREAD_GETAFFINITY_NP 0
#define HAVE_PTHREAD_NP_H 0
#define HAVE_PTHREAD_SETAFFINITY_NP 0
#define HAVE_PTHREAD_SETNAME_NP 0
#define HAVE_PTHREAD_SET_NAME_NP 0
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 0
#define TRIM_DSP_FUNCTIONS 1
#define HAVE_DOTPROD 0
#define HAVE_I8MM 0
#define HAVE_SVE 0
#define HAVE_SVE2 0
#ifndef _CRT_DECLARE_NONSTDC_NAMES
#define _CRT_DECLARE_NONSTDC_NAMES 1
#endif
#define fseeko _fseeki64
#define ftello _ftelli64
