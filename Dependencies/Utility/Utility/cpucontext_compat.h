/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// AoT @feature 64-bit: architecture neutral access to the CPU context that the
// crash handlers and stack walkers use. On 32-bit builds every macro expands to
// exactly what the original code wrote, so 32-bit output is unchanged.

#pragma once

#include <windows.h>

#if defined(_WIN64)

#define CPUCONTEXT_IP(ctx) ((ctx)->Rip)
#define CPUCONTEXT_SP(ctx) ((ctx)->Rsp)
#define CPUCONTEXT_FP(ctx) ((ctx)->Rbp)
#define CPUCONTEXT_AX(ctx) ((ctx)->Rax)
#define CPUCONTEXT_BX(ctx) ((ctx)->Rbx)
#define CPUCONTEXT_CX(ctx) ((ctx)->Rcx)
#define CPUCONTEXT_DX(ctx) ((ctx)->Rdx)
#define CPUCONTEXT_SI(ctx) ((ctx)->Rsi)
#define CPUCONTEXT_DI(ctx) ((ctx)->Rdi)

// The machine type for StackWalk/StackWalk64.
#define CPUCONTEXT_MACHINE IMAGE_FILE_MACHINE_AMD64

// dbghelp/imagehlp export names. On 64-bit only the "64" variants exist.
#define CPUCONTEXT_DBGHELP_NAME(name) name "64"

// The x87 register dump only exists in 32-bit contexts.
#define CPUCONTEXT_HAS_X87_SAVE_AREA 0

#else

#define CPUCONTEXT_IP(ctx) ((ctx)->Eip)
#define CPUCONTEXT_SP(ctx) ((ctx)->Esp)
#define CPUCONTEXT_FP(ctx) ((ctx)->Ebp)
#define CPUCONTEXT_AX(ctx) ((ctx)->Eax)
#define CPUCONTEXT_BX(ctx) ((ctx)->Ebx)
#define CPUCONTEXT_CX(ctx) ((ctx)->Ecx)
#define CPUCONTEXT_DX(ctx) ((ctx)->Edx)
#define CPUCONTEXT_SI(ctx) ((ctx)->Esi)
#define CPUCONTEXT_DI(ctx) ((ctx)->Edi)

#define CPUCONTEXT_MACHINE IMAGE_FILE_MACHINE_I386

#define CPUCONTEXT_DBGHELP_NAME(name) name

#define CPUCONTEXT_HAS_X87_SAVE_AREA 1

#endif
