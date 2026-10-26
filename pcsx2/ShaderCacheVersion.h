// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

/// Version number for GS and other shaders. Increment whenever any of the contents of the
/// shaders change, to invalidate the cache.
// 109: driver-workaround shader wrappers (gpu_bitwise_and / gpu_bitwise_not / gpu_boolean_not /
// gpu_matrix_element). Every TFX and convert shader's source text changed, so a cached blob from
// 108 no longer matches the source that produced it — leaving this alone hands users stale
// binaries and garbage rendering after the update.
// 110: Vulkan emits the gpu_bitwise_and / gpu_matrix_element wrappers as bare #defines when no
// driver workaround is active, so unaffected drivers get the same SPIR-V they had at 108. 109 wrapped
// them in real functions on EVERY driver, and Qualcomm's SPIR-V compiler segfaults compiling a TFX
// pipeline containing those calls.
// 111: the 2026-08 upstream sync drops the unused SW_DEPTH term from the Vulkan TFX ZWRITE
// condition, changing that shader's source text. ⚠️ Upstream numbered the same change 109, which
// is BELOW our 110 — taking their value would hand every user a stale blob for a source they no
// longer have. Our counter has been ahead of theirs since 109 and cannot be resynced by adopting
// their numbers; always bump past our own last value.
// 112: the Vulkan expanded-line vertex shader pairs a line's two ends by index on every device and
// no longer gets a VS_PROVOKING_VERTEX_LAST define (upstream 7ee071a7a4, their 129).
// 113: the Vulkan TFX shader gains PS_AF_IN_SRC1 (fixed blend factor through the second output on a
// driver that ignores the blend constant; upstream 5791d09a2b, their 114).
static constexpr u32 SHADER_CACHE_VERSION = 113; // 108 was upstream PR 14688; their 109 = our 111
