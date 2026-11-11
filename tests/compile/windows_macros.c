/* Compile check: every public header in include/ysp, with its
 * implementation, after the macros that <windows.h> defines on ordinary
 * names.
 *
 * A user file that includes <windows.h> before a ysp header gets these
 * macros in the header's text. A struct field, parameter or local with one
 * of these names then fails to compile on Windows only: aep.h 0.16.0's
 * desc.hyper became desc.__int64 under rpcndr.h's "#define hyper __int64",
 * and the player's impl.c, where gfx.h's implementation includes
 * <windows.h> before aep.h, failed on MSVC.
 *
 * Here, off Windows, the same definitions come before every header. Each
 * one is the expansion in the MinGW-w64 headers (mingw-w64-headers/include
 * at commit a215715f0b87, read 2026-11-11), with the file and line, except
 * where MSVC's Windows SDK differs: there the SDK's, because the check is
 * for the MSVC build. The calling-convention macros take the headers' ARM
 * branch (empty): an empty expansion still removes the name, so its use
 * as an identifier fails, and GCC refuses the x86 __stdcall attribute on
 * x86-64.
 * A header's own Windows code is compiled out here; it includes
 * <windows.h> itself, so the Windows builds check it.
 *
 * Linux only: elsewhere main() says it skipped. On Windows the real
 * headers define these names and would clash with this copy; on macOS the
 * system headers included after them have not been tried.
 *
 * CMake builds it as compile_windows_macros_c and _cxx where SDL3 and
 * miniaudio are found. windows_macros.cpp is the same unit as C++17. */
#ifdef __linux__

/* _mingw.h: MinGW's spelling of MSVC's 64-bit integer keyword */
#ifndef __int64
#define __int64 long long
#endif

/* rpcndr.h:99 */
#define hyper __int64
/* rpcndr.h:91 has it under RC_INVOKED only; the Windows SDK's rpcndr.h
 * defines it always */
#define small char
/* rpcndr.h:93 and :95, typedefs, not macros. A local, parameter or member
 * named byte only shadows the type, which C and C++ allow, but gcc's
 * -Wshadow warns about it (outline.h's CFF reader and box.h's
 * ybox__fixed_byte(), 2026-11-11), and -Werror would fail this unit on that
 * warning alone. MSVC has no such warning for a type. Every header's own
 * check keeps -Wshadow, so only these shadows go unwarned here. */
typedef unsigned char byte;
typedef unsigned char boolean;
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wshadow"
#endif
/* minwindef.h:57, :61, :65 */
#define IN
#define OUT
#define OPTIONAL
/* minwindef.h:73, :74, :76 (ARM), :81 */
#define far
#define near
#define pascal
#define cdecl
/* minwindef.h:88 and :106 (ARM) */
#define CALLBACK
#define PASCAL
/* minwindef.h:118, :119, :122 */
#define FAR
#define NEAR
#define CONST const
/* minwindef.h:173 and :177; NOMINMAX in a ysp header does not remove them
 * once the user's <windows.h> has defined them */
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))
/* combaseapi.h:87 (C), :52 (C++, through __STRUCT__) */
#define interface struct
/* winnt.h:309 */
#define VOID void
/* winnt.h:3146, __MSABI_LONG(x) being x##l */
#define DELETE (0x00010000l)
/* wingdi.h:75 */
#define ERROR 0

#define YVID_NO_PL_MPEG   /* not about MPEG-1 */

/* The player's order (player/impl.c): gfx.h implements screen.h and
 * color.h, video.h implements timeline.h, response.h needs trials.h. */
#define YSP_OUTLINE_IMPLEMENTATION
#include "ysp/outline.h"
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"
#define YSP_VIDEO_IMPLEMENTATION
#include "ysp/video.h"
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"
#define YSP_STAIR_IMPLEMENTATION
#include "ysp/stair.h"
#define YSP_QUEST_IMPLEMENTATION
#include "ysp/quest.h"
#define YSP_AEP_IMPLEMENTATION
#include "ysp/aep.h"
#define YSP_RESPONSE_IMPLEMENTATION
#include "ysp/response.h"
#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"
#define YSP_DATASET_IMPLEMENTATION
#include "ysp/dataset.h"
#define YSP_PACK_IMPLEMENTATION
#include "ysp/pack.h"
#define YSP_JSON_IMPLEMENTATION
#include "ysp/json.h"
#define YSP_EXPERIMENT_IMPLEMENTATION
#include "ysp/experiment.h"
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"
#define YSP_PARALLEL_IMPLEMENTATION
#include "ysp/parallel.h"
#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"
#define YSP_TABLE_IMPLEMENTATION
#define YTB_STDIO
#include "ysp/table.h"
#include "ysp/input.h"
#define YSP_SIG_IMPLEMENTATION
#include "ysp/sig.h"
#define YSP_BOX_IMPLEMENTATION
#include "ysp/box.h"
#define YSP_DEVICE_IMPLEMENTATION
#include "ysp/device.h"
#define YSP_NET_IMPLEMENTATION
#include "ysp/net.h"

#include <stdio.h>

int main(void) {
    printf("windows_macros: every header after <windows.h>'s macros, %s\n", yaep_version());
    return 0;
}

#else

#include <stdio.h>

int main(void) {
    printf("windows_macros: skipped, Linux only\n");
    return 0;
}

#endif
