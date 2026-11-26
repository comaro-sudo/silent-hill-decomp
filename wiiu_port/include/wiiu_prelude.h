/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * wiiu_prelude.h - force-included (-include) into every GAME translation unit
 * of the Wii U build, never into the Wii U HAL files in wiiu_port/src/.
 *
 * The decomp's `bool` is `typedef enum { false, true } bool` (4 bytes,
 * include/decomp/types.h), and only when `bool` is not already a macro.
 * devkitPro's newlib pulls <stdbool.h> in from <sys/iosupport.h>, which the
 * stat/dirent headers reach -- AFTER types.h in most TUs, so half a file saw the
 * enum and the other half _Bool: "conflicting types for Fs_QueueCanRead".
 *
 * Including <stdbool.h> here first and dropping its macros keeps the enum
 * everywhere: stdbool.h is include-guarded, so every later include is a no-op.
 */
#ifndef SH_WIIU_PRELUDE_H
#define SH_WIIU_PRELUDE_H
#ifndef __cplusplus
#include <stdbool.h>
#undef bool
#undef true
#undef false
#endif
#endif
