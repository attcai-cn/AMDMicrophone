/*
 * AMDMicrophoneKmodInfo.c
 *
 * Defines the kmod_info structure whose Mach-O symbol (_kmod_info) is
 * required before a kext can be placed in a kernel collection. Without it
 * kmutil fails with:
 *   "cannot be placed in kernel collection because: kexts must have a
 *    _kmod_info symbol"
 *
 * Xcode's Kernel Extension target generates this stub automatically. The
 * Command Line Tools-only build in build.sh has to provide it explicitly.
 *
 * libkmod.a supplies _start/_stop (c_start.o / c_stop.o) and references
 * _kmod_info, _realmain and _antimain; everything below resolves them.
 */
#include <mach/mach_types.h>
#include <mach/kmod.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

KMOD_EXPLICIT_DECL(com.qhuyduong.AMDMicrophone, "1.0.0d1", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
