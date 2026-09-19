// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SaveState.h"
#include "IopCounters.h"

#include <memory>

struct Pcsx2Config;

class AudioStream;

namespace SPU2
{
/// PS2/Native Sample Rate.
static constexpr u32 SAMPLE_RATE = 48000;

/// PSX Mode Sample Rate.
static constexpr u32 PSX_SAMPLE_RATE = 44100;

/// Open/close, call at VM startup/shutdown.
bool Open();
void Close();

/// Reset, rebooting VM or going into PSX mode.
void Reset(bool psxmode);

/// Identifies any configuration changes and applies them.
void CheckForConfigChanges(const Pcsx2Config& old_config);

/// Returns the current output volume, irrespective of the configuration.
u32 GetOutputVolume();

/// Directly updates the output volume without going through the configuration.
void SetOutputVolume(u32 volume);

/// Sets up muting and unmuting and reports success or failure.
bool SetOutputMuted(const bool muted);

/// Returns true if the output is muted (distinct from 0%).
bool IsOutputMuted();

/// Swaps the final stereo output channels (L<->R) for devices forced into
/// reverse-landscape (flipped physical speakers, e.g. Clamp pad).
void SetSwapChannels(bool swap);

/// Returns true if the final stereo output is L<->R swapped.
bool IsSwapChannels();

/// Updates the current volume based on running state.
void UpdateOutputVolume();

/// Saves the current volume based on running state.
void SaveOutputVolume();

/// Pauses/resumes the output stream.
void SetOutputPaused(bool paused);

/// When suppressed, SetOutputPaused() is a no-op so the audio device stays
/// running across a transient VM park (e.g. settings-apply), preventing the
/// OS from reclaiming the low-latency stream during a long park.
void SetOutputPauseSuppressed(bool suppressed);

/// Clears output buffers in no-sync mode, prevents long delays after fast forwarding.
void OnTargetSpeedChanged();

/// Returns true if we're currently running in PSX mode.
bool IsRunningPSXMode();

/// Returns the current sample rate the SPU2 is operating at.
u32 GetConsoleSampleRate();

	// libretro: direct access to the output stream so the frontend can pull
	// mixed frames from retro_run.
	AudioStream* GetOutputStream();

/// Audio telemetry published once per second by PollAudioStats(). Readable from any thread.
struct AudioStatsSnapshot
{
	u32 underruns = 0;         ///< ring underruns in the last 1 s window
	u32 fabricated_frames = 0; ///< frames of stretched/silent output fabricated in the window
	u32 overruns = 0;          ///< ring overruns (producer ahead) in the window
	u32 low_water_frames = 0;  ///< lowest ring occupancy right after a read, in the window
	u32 buffered_frames = 0;   ///< ring occupancy at the tick
	u32 target_frames = 0;     ///< ring target (BufferMS)
	u32 backend_xruns = 0;     ///< device-side xruns since the stream opened (0 if unsupported)
	u32 backend_buffer_frames = 0;
	u32 backend_burst_frames = 0;
	u32 sample_rate = 0;
	u32 windows = 0;           ///< 1 s windows published so far (0 = nothing published yet)
	// Session-cumulative (survive stream recreates) — what a host records at session end.
	u32 total_underruns = 0;
	u32 total_fabricated_frames = 0;
	u32 total_overruns = 0;
};

/// CPU thread only (owns s_output_stream). Cheap unless a second has elapsed; then it snapshots
/// the ring/backend counters, resets the window and logs ONE warning line if anything went wrong.
/// Called from VMManager::Internal::VSyncOnCPUThread().
void PollAudioStats();

/// Copy of the last published snapshot. Safe from any thread (mutex copy, never touches the stream).
AudioStatsSnapshot GetAudioStatsSnapshot();
} // namespace SPU2

void SPU2write(u32 mem, u16 value);
u16 SPU2read(u32 mem);

void SPU2async();
s32 SPU2freeze(FreezeAction mode, freezeData* data);

// Partial restore from a legacy-format (AetherSX2-era) SPU2 block, whose tail
// cannot be replayed. See the definition for what is and is not kept.
s32 SPU2freezeLegacy(const void* data, size_t size);

void SPU2readDMA4Mem(u16* pMem, u32 size);
void SPU2writeDMA4Mem(u16* pMem, u32 size);
void SPU2interruptDMA4();
void SPU2interruptDMA7();
void SPU2readDMA7Mem(u16* pMem, u32 size);
void SPU2writeDMA7Mem(u16* pMem, u32 size);

extern u64 lClocks;

extern void CounterUpdate(u32 DMAICounter);
extern void TimeUpdate(u64 cClocks);
extern void SPU2_FastWrite(u32 rmem, u16 value);

