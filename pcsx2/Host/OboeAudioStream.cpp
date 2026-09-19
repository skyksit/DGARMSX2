// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Host/AudioStream.h"
#include "VMManager.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"

#include "oboe/Oboe.h"
#include "oboe/OboeExtensions.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#if defined(__ANDROID__)
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
	// Device-buffer floor in bursts (AAudio's safe fast-path minimum). See ApplyBurstAdaptiveBuffering().
	constexpr int32_t kMinDeviceBufferBursts = 2;
	// How often onAudioReady() polls the device xrun counter for the grow-only tuner.
	constexpr u32 kTuneEveryNCallbacks = 8;
	// Slice size for ReadFrames() so its alloca() stays bounded now that the callback size is
	// whatever the route negotiates.
	constexpr int32_t kMaxFramesPerRead = 1024;

	class OboeAudioStream final : public AudioStream,
	                               oboe::AudioStreamDataCallback,
	                               oboe::AudioStreamErrorCallback
	{
	public:
		OboeAudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
		~OboeAudioStream() override;

		void SetPaused(bool paused) override;

		bool Initialize(bool stretch_enabled);
		bool Open();
		bool Start();
		void Stop();
		void Close();

		oboe::DataCallbackResult onAudioReady(oboe::AudioStream* p_audioStream,
			void* p_audioData, int32_t p_numFrames) override;
		bool onError(oboe::AudioStream* oboeStream, oboe::Result error) override;

	protected:
		void FillBackendStats(Stats* stats) const override;

	private:
		// Post-open tuning: read the route's burst, grow (never shrink) the device buffer to the
		// floor, decide the affinity pin, and log one diagnostic line.
		void ApplyBurstAdaptiveBuffering();
		bool ComputeShouldPinAudioThread(int32_t burst) const;

		// ★ Serialises the stream lifecycle. onError() runs on OBOE'S OWN callback thread and
		// tears the stream down and back up (Stop/Close/Open/Start), while the CPU thread can be
		// inside SetPaused()/Close() on the very same object. SetPaused's `if (m_stream)` followed
		// by `m_stream->requestPause()` is not atomic against onError's `m_stream.reset()`, so the
		// stream could be destroyed between the null check and the dereference — a use-after-free.
		// That window opens exactly where users report crashing: Android reclaims the audio device
		// a few seconds into the pause menu (#333), onError fires to reopen it, and touching any
		// setting at that moment re-enters SPU2 from the CPU thread (#422).
		// Recursive because Close() calls Stop(), and onError() calls all four in sequence.
		std::recursive_mutex m_lock;

		bool m_playing = false;
		// Written by Start()/Stop() on the CPU thread, read by onError() on the callback thread.
		std::atomic<bool> m_stop_requested{false};

		std::shared_ptr<oboe::AudioStream> m_stream;

		// Performance mode the stream is (re)opened with. Starts at LowLatency;
		// Initialize() downgrades it to None if the device refuses the fast path
		// (some Adreno/AAudio devices fail requestStart() with ErrorDisconnected
		// at boot). onError()'s reopen then reuses whatever mode actually worked.
		oboe::PerformanceMode m_perf_mode = oboe::PerformanceMode::LowLatency;

		// Affinity pin latch. Oboe spawns its own audio data thread; we don't
		// see its TID until the callback fires the first time. After the
		// first callback we apply the perf-cluster affinity once. Audio
		// callbacks compete with EE for cache lines + share the same big
		// cluster — without pinning, the audio thread can land on a little
		// core (jitter) or migrate onto EE's core (L2 pollution).
		std::atomic<bool> m_audio_thread_pinned{false};

		// Route-dependent decision made in Open() (under m_lock, before the new stream's first
		// callback), read by onAudioReady(). Open() happens-before the callback, so a plain bool.
		bool m_pin_audio_thread = false;

		// Backend telemetry for AudioStream::Stats. Written in Open()/onAudioReady(), read by the
		// CPU thread's 1 Hz poll through FillBackendStats() — atomics only, never m_stream itself.
		std::atomic<int32_t> m_burst_frames{0};
		std::atomic<int32_t> m_device_buffer_frames{0};
		std::atomic<int32_t> m_capacity_frames{0};
		std::atomic<int32_t> m_xrun_total{0};

		// Grow-only xrun tuner state (callback thread only).
		int32_t m_xrun_baseline = 0;
		u32 m_callbacks_since_tune = 0;
	};
} // namespace

oboe::DataCallbackResult OboeAudioStream::onAudioReady(oboe::AudioStream* p_audioStream,
	void* p_audioData, int32_t p_numFrames)
{
#if defined(__ANDROID__)
	// Affinity pin. Oboe owns the audio data thread; we only see its TID
	// inside this callback. Pin onto the same perf-cluster as EE/VU/GS so
	// the audio thread doesn't (a) get scheduled to a little core and
	// inject jitter into the callback's deadline, or (b) land on EE's
	// core and pollute L2.
	//
	// Only on routes where that reasoning holds — see ComputeShouldPinAudioThread(). On
	// coarse-burst routes (A2DP, offload, OpenSL ES) this callback is nearly idle with a long
	// deadline, and caging it in the cluster EE/VU/GS already saturate only adds wake-up
	// preemption latency.
	//
	// VMManager's SetEmuThreadAffinities runs when the VM transitions to
	// Running, which is typically AFTER Oboe has opened its stream and
	// fired the first callback. So the first few callbacks see
	// perf_mask=0 (pinning not yet active) and skip. Latch only after a
	// SUCCESSFUL pin so we keep polling cheaply (one atomic-acquire +
	// `s_thread_affinities_set` bool check inside
	// GetPerformanceClusterAffinityMask) until pinning actually turns on.
	if (m_pin_audio_thread && !m_audio_thread_pinned.load(std::memory_order_acquire))
	{
		const u64 perf_mask = VMManager::Internal::GetPerformanceClusterAffinityMask();
		if (perf_mask != 0)
		{
			const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
			cpu_set_t set;
			CPU_ZERO(&set);
			for (u32 i = 0; i < 64; i++)
			{
				if (perf_mask & (static_cast<u64>(1) << i))
					CPU_SET(i, &set);
			}
			if (sched_setaffinity(tid, sizeof(set), &set) == 0)
			{
				INFO_LOG("(Oboe) audio thread tid={} pinned to perf-cluster mask 0x{:x}", tid, perf_mask);
				m_audio_thread_pinned.store(true, std::memory_order_release);
			}
			else
			{
				WARNING_LOG("(Oboe) sched_setaffinity tid={} failed (errno {}) — will retry next callback", tid, errno);
			}
		}
		// else: pinning not active yet (VM hasn't reached Running). Skip
		// the syscall + don't latch — next callback retries.
	}
#endif

	if (p_audioData != nullptr)
	{
		// ReadFrames() uses alloca() in its underrun path, sized by the request. With the callback
		// size left to the route (no setFramesPerDataCallback, see Open()), that request is
		// unbounded in principle, so slice it. Slicing is otherwise transparent: an underrun in the
		// first slice flips m_filling and the rest come out as silence, exactly like one big read.
		SampleType* out = reinterpret_cast<SampleType*>(p_audioData);
		int32_t remaining = p_numFrames;
		while (remaining > 0)
		{
			const int32_t n = std::min(remaining, kMaxFramesPerRead);
			ReadFrames(out, static_cast<u32>(n));
			out += static_cast<size_t>(n) * m_output_channels;
			remaining -= n;
		}
	}

	// Grow-only xrun response. Deliberately not oboe::LatencyTuner: its constructor calls reset(),
	// which first DROPS the buffer to 2 bursts and grows back on glitches — on A2DP that is a
	// guaranteed dropout storm on every stream open. We start from whatever the route negotiated
	// (never below it, see ApplyBurstAdaptiveBuffering) and only ever add bursts.
	// getXRunCount() reads shared memory (MMAP) or the cblk (legacy) — no binder call, safe here.
	if (m_parameters.android_adaptive_buffer && ++m_callbacks_since_tune >= kTuneEveryNCallbacks)
	{
		m_callbacks_since_tune = 0;
		const oboe::ResultWithValue<int32_t> xr = p_audioStream->getXRunCount();
		if (xr)
		{
			const int32_t total = xr.value();
			if (total > m_xrun_baseline)
			{
				m_xrun_baseline = total;
				m_xrun_total.store(total, std::memory_order_relaxed);
				const int32_t size = p_audioStream->getBufferSizeInFrames();
				const int32_t burst = p_audioStream->getFramesPerBurst();
				if (burst > 0)
				{
					// AAudio clips this to the capacity, so growth simply stops at the ceiling.
					const oboe::ResultWithValue<int32_t> r = p_audioStream->setBufferSizeInFrames(size + burst);
					if (r)
						m_device_buffer_frames.store(r.value(), std::memory_order_relaxed);
				}
			}
		}
	}

	return oboe::DataCallbackResult::Continue;
}

bool OboeAudioStream::onError(oboe::AudioStream* oboeStream, oboe::Result error)
{
	Console.Error("(Oboe) ErrorCB %d", error);
	if (error == oboe::Result::ErrorDisconnected && !m_stop_requested.load(std::memory_order_acquire))
	{
		// Held across the whole teardown/rebuild so the CPU thread can't observe (or destroy) a
		// half-open stream partway through. See the m_lock comment.
		const std::lock_guard<std::recursive_mutex> guard(m_lock);
		Console.Error("(Oboe) Stream disconnected, reopening...");
		Stop();
		Close();
		if (!Open() || !Start())
			Console.Error("(Oboe) Failed to reopen stream after disconnection.");
		return true;
	}
	return false;
}

bool OboeAudioStream::Initialize(bool stretch_enabled)
{
	static constexpr const std::array<SampleReader, static_cast<size_t>(AudioExpansionMode::Count)> sample_readers = {{
		&StereoSampleReaderImpl,
		&SampleReaderImpl<AudioExpansionMode::StereoLFE,
			READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT, READ_CHANNEL_LFE>,
		&SampleReaderImpl<AudioExpansionMode::Quadraphonic,
			READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
			READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
		&SampleReaderImpl<AudioExpansionMode::QuadraphonicLFE,
			READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT, READ_CHANNEL_LFE,
			READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
		&SampleReaderImpl<AudioExpansionMode::Surround51,
			READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT, READ_CHANNEL_FRONT_CENTER,
			READ_CHANNEL_LFE, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
		&SampleReaderImpl<AudioExpansionMode::Surround71,
			READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT, READ_CHANNEL_FRONT_CENTER,
			READ_CHANNEL_LFE, READ_CHANNEL_SIDE_LEFT, READ_CHANNEL_SIDE_RIGHT,
			READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
	}};
	BaseInitialize(sample_readers[static_cast<size_t>(m_parameters.expansion_mode)], stretch_enabled);

	// Resilient open: some devices (seen on Adreno/AAudio) refuse a low-latency /
	// fast-path output stream at boot and fail requestStart() with
	// ErrorDisconnected — the audio device was reclaimed the instant we tried to
	// start it. Rather than fall straight to permanent silent null output, retry,
	// and if the fast path keeps failing drop to the most compatible
	// PerformanceMode::None (shared slow-path) stream before giving up.
	static constexpr oboe::PerformanceMode kModes[] = {
		oboe::PerformanceMode::LowLatency,
		oboe::PerformanceMode::None,
	};
	for (const oboe::PerformanceMode mode : kModes)
	{
		m_perf_mode = mode;
		for (int attempt = 0; attempt < 2; attempt++)
		{
			if (Open() && Start())
			{
				if (mode != oboe::PerformanceMode::LowLatency || attempt != 0)
					Console.WriteLn("(Oboe) Audio stream opened with performance mode %d (attempt %d).",
						static_cast<int>(mode), attempt);
				return true;
			}
			// Open() failed, or Open() succeeded but Start() failed: tear the
			// half-open stream down before the next attempt / mode, then pause
			// briefly to let a transient device-reclaim settle.
			Close();
			std::this_thread::sleep_for(std::chrono::milliseconds(60));
		}
		Console.Warning("(Oboe) performance mode %d failed; trying a more compatible mode...",
			static_cast<int>(mode));
	}
	Console.Error("(Oboe) All open/start attempts failed; audio will be silent.");
	return false;
}

bool OboeAudioStream::Open()
{
	const std::lock_guard<std::recursive_mutex> guard(m_lock);
	// Each Open() spawns a fresh Oboe audio thread with a new TID, so the
	// per-stream pin latch needs to clear here. Without this, an error-
	// recovery re-Open() (onError → Stop/Close/Open) keeps the latch set
	// from the previous instance and the new audio thread runs un-pinned.
	m_audio_thread_pinned.store(false, std::memory_order_release);
	m_pin_audio_thread = false;

	oboe::AudioStreamBuilder builder;
	builder.setDirection(oboe::Direction::Output);
	builder.setPerformanceMode(m_perf_mode);
	// Opt-in legacy OpenSL ES output. AAudio's low-latency fast path is the one
	// Android silently reclaims when the stream sits idle (e.g. the in-game pause
	// menu), which then forces a full Close/Open stream rebuild on resume — the
	// ~1s hitch users see toggling fast-forward through the menu, and the cause of
	// audio dying a few seconds into a pause (#333). OpenSL ES is a higher-latency
	// buffer-queue path Android does NOT aggressively reclaim, so pause→resume
	// stays a cheap requestPause/requestStart with no rebuild. Off by default; the
	// trade is a little more output latency.
	if (m_parameters.android_use_opensles)
		builder.setAudioApi(oboe::AudioApi::OpenSLES);
	builder.setSharingMode(oboe::SharingMode::Shared);
	builder.setFormat(oboe::AudioFormat::Float);
	// Fixed on purpose: SPU2 fills the ring at exactly this rate (48 kHz; 44.1 kHz in PSX mode)
	// and AudioStream has no resampler of its own beyond the underrun stretch, so the device must
	// consume at the same rate. Never leave this Unspecified.
	builder.setSampleRate(m_sample_rate);
	// The real channel count. The old `== 2 ? Stereo : Mono` mapped every expansion mode
	// (4/6/8 ch) onto a MONO stream while ReadFrames() kept writing N floats per frame — an
	// N-fold overflow of the device buffer that nobody hit only because expansion defaults off.
	builder.setChannelCount(m_output_channels);
	builder.setDeviceId(oboe::kUnspecified);

	// Attribute hints. Usage::Game routes and volumes like Media (STREAM_MUSIC) but tells vendor
	// stacks this is interactive audio; opting out of spatialization removes platform
	// post-processing that is pure added latency for us. Kill switch: AndroidAudioUsageGame.
	if (m_parameters.android_audio_usage_game)
	{
		builder.setUsage(oboe::Usage::Game);
		builder.setContentType(oboe::ContentType::Music);
	}
	builder.setIsContentSpatialized(false);
	builder.setSpatializationBehavior(oboe::SpatializationBehavior::Never);

	// Capacity is the CEILING setBufferSizeInFrames() may grow to, not the latency. The legacy
	// hardcoded 4096 frames (85 ms @ 48 kHz) was too shallow a ceiling for A2DP, whose end-to-end
	// budget is 120-250 ms. Kept explicit rather than Unspecified because OpenSL ES derives its
	// buffer-queue length from it — dropping it would fall back to a queue of 2 and get worse.
	builder.setBufferCapacityInFrames(static_cast<int32_t>(
		GetBufferSizeForMS(m_sample_rate, m_parameters.android_buffer_capacity_ms)));

	// Deliberately NO setFramesPerDataCallback(). The old fixed 2048-frame callback drained
	// 42.7 ms from the SPU2 ring in one go; with the default BufferMS=50 that left ~8 ms of
	// headroom in steady state — fine against the speaker's regular 2-5 ms bursts, hopeless
	// against A2DP jitter (the reported Bluetooth stutter). The 2048-frame sawtooth also kept the
	// time-stretcher permanently active. ReadFrames() handles any size, so take the route's
	// natural burst and let the ring drain in small steps.
	builder.setDataCallback(this);
	builder.setErrorCallback(this);

	Console.WriteLn("(Oboe) Opening stream...");
	const oboe::Result result = builder.openStream(m_stream);
	if (result != oboe::Result::OK)
	{
		Console.Error("(Oboe) openStream() failed: %d", result);
		return false;
	}

	ApplyBurstAdaptiveBuffering();
	return true;
}

void OboeAudioStream::ApplyBurstAdaptiveBuffering()
{
	const int32_t burst = m_stream->getFramesPerBurst();
	const int32_t capacity = m_stream->getBufferCapacityInFrames();
	const int32_t current = m_stream->getBufferSizeInFrames();

	int32_t applied = current;
	if (burst > 0)
	{
		// Floor = max(2 bursts, OutputLatencyMS) rounded up to whole bursts. Two bursts is the
		// AAudio convention for a safe fast-path minimum; the ms floor is for routes whose bursts
		// are coarse and irregular (A2DP). OutputLatencyMS (default 20) is reused on purpose so
		// this adds no new knob.
		const int32_t floor_frames = static_cast<int32_t>(
			GetBufferSizeForMS(m_sample_rate, m_parameters.output_latency_ms));
		const int32_t bursts = std::max(kMinDeviceBufferBursts, (floor_frames + burst - 1) / burst);
		int32_t wanted = bursts * burst;
		if (capacity > 0)
			wanted = std::min(wanted, capacity);

		// GROW-ONLY: whatever AAudio already chose for this route is never reduced. This is the
		// entire speaker-regression guard — we only ever add headroom.
		if (wanted > current)
		{
			const oboe::ResultWithValue<int32_t> r = m_stream->setBufferSizeInFrames(wanted);
			if (r)
				applied = r.value();
			else
				Console.Warning("(Oboe) setBufferSizeInFrames(%d) failed: %d", wanted, static_cast<int>(r.error()));
		}
	}

	m_burst_frames.store(burst, std::memory_order_relaxed);
	m_device_buffer_frames.store(applied, std::memory_order_relaxed);
	m_capacity_frames.store(capacity, std::memory_order_relaxed);
	m_xrun_baseline = 0;
	m_xrun_total.store(0, std::memory_order_relaxed);
	m_callbacks_since_tune = 0;

	m_pin_audio_thread = ComputeShouldPinAudioThread(burst);

	// One line per open so a field log states which path we actually got. isMMapUsed() is a
	// best-effort dlsym of a hidden AAudio symbol — diagnostic only, never gate on it.
	Console.WriteLn("(Oboe) opened: api=%s perf=%d sharing=%d rate=%d ch=%d burst=%d bufsize=%d->%d cap=%d mmap=%d xrun_supported=%d pin=%d",
		m_stream->usesAAudio() ? "AAudio" : "OpenSLES",
		static_cast<int>(m_stream->getPerformanceMode()),
		static_cast<int>(m_stream->getSharingMode()),
		m_stream->getSampleRate(), m_stream->getChannelCount(),
		burst, current, applied, capacity,
		oboe::OboeExtensions::isMMapUsed(m_stream.get()) ? 1 : 0,
		m_stream->isXRunCountSupported() ? 1 : 0,
		m_pin_audio_thread ? 1 : 0);
}

bool OboeAudioStream::ComputeShouldPinAudioThread(int32_t burst) const
{
	if (!m_parameters.android_pin_audio_thread)
		return false;
	// Pin only when we actually got the fast path AND the burst is short enough that one missed
	// wake-up is an xrun. Coarse-burst routes (A2DP, offload, OpenSL ES) have an almost idle
	// callback with a long deadline; confining it to the 3-4 cores EE/VU/GS already saturate
	// (affinity mode 7) only adds preemption latency when it wakes. Route changes go through
	// disconnect → Open(), so this is re-evaluated whenever Bluetooth connects or drops.
	if (m_stream->getPerformanceMode() != oboe::PerformanceMode::LowLatency)
		return false;
	if (burst <= 0)
		return false;
	return burst <= static_cast<int32_t>(m_sample_rate / 100); // <= 10 ms
}

void OboeAudioStream::FillBackendStats(Stats* stats) const
{
	// Atomics only — the CPU-thread poller must never touch m_stream, which onError() may be
	// tearing down and rebuilding on its own thread at this very moment.
	stats->backend_xruns = static_cast<u32>(std::max(0, m_xrun_total.load(std::memory_order_relaxed)));
	stats->backend_buffer_frames = static_cast<u32>(std::max(0, m_device_buffer_frames.load(std::memory_order_relaxed)));
	stats->backend_burst_frames = static_cast<u32>(std::max(0, m_burst_frames.load(std::memory_order_relaxed)));
}

bool OboeAudioStream::Start()
{
	const std::lock_guard<std::recursive_mutex> guard(m_lock);
	if (m_playing)
		return true;

	Console.WriteLn("(Oboe) Starting stream...");
	m_stop_requested.store(false, std::memory_order_release);

	oboe::Result result = m_stream->requestStart();
	if (result != oboe::Result::OK)
	{
		Console.Error("(Oboe) requestStart() failed: %d", result);
		return false;
	}
	m_playing = true;
	return true;
}

void OboeAudioStream::Stop()
{
	const std::lock_guard<std::recursive_mutex> guard(m_lock);
	if (!m_playing)
		return;

	Console.WriteLn("(Oboe) Stopping stream...");
	m_stop_requested.store(true, std::memory_order_release);

	oboe::Result result = m_stream->requestStop();
	if (result != oboe::Result::OK)
		Console.Error("(Oboe) requestStop() failed: %d", result);

	m_playing = false;
}

void OboeAudioStream::Close()
{
	const std::lock_guard<std::recursive_mutex> guard(m_lock);
	Console.WriteLn("(Oboe) Closing stream...");
	if (m_playing)
		Stop();
	if (m_stream)
	{
		m_stream->close();
		m_stream.reset();
	}
}

void OboeAudioStream::SetPaused(bool paused)
{
	// This is the CPU-thread side of the race with onError(): without the lock, m_stream can be
	// reset by the reopen between the null check and the dereference below.
	const std::lock_guard<std::recursive_mutex> guard(m_lock);
	if (m_paused == paused)
		return;

	if (paused)
	{
		if (m_stream)
		{
			oboe::Result result = m_stream->requestPause();
			if (result != oboe::Result::OK)
				Console.Error("(Oboe) requestPause() failed: %d", result);
		}
		// Mark not-playing even if requestPause() failed, so the paused/
		// playing bookkeeping can't desync and strand a later resume.
		m_playing = false;
	}
	else
	{
		// Resume must be authoritative. If m_playing desynced to true (e.g.
		// an error-recovery reopen ran while we thought the stream was
		// paused), Start()'s `if (m_playing) return true;` guard would
		// swallow the restart and leave audio dead. Clear it first so the
		// resume always actually re-issues requestStart().
		m_playing = false;
		if (!Start())
		{
			// requestStart() failing here means the OS took the device away while we
			// were parked: Android reclaims an idle low-latency stream after a few
			// seconds, so just sitting in the in-game menu (issue #333) — or
			// backgrounding, or a call/BT switch — left audio dead for the rest of
			// the session. A paused stream never runs its data callback, so onError()
			// CANNOT fire for this; the resume is the only place that can notice.
			// Rebuild the stream exactly like the disconnect path does. Open() keeps
			// the negotiated performance-mode latch, so we don't re-lose the fast path.
			Console.Error("(Oboe) requestStart() on resume failed; reopening stream...");
			Close();
			if (!Open() || !Start())
				Console.Error("(Oboe) Failed to reopen the stream on resume.");
		}
	}
	m_paused = paused;
}

OboeAudioStream::OboeAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
	: AudioStream(sample_rate, parameters)
{
}

OboeAudioStream::~OboeAudioStream()
{
	Close();
}

std::unique_ptr<AudioStream> AudioStream::CreateOboeAudioStream(u32 sample_rate,
	const AudioStreamParameters& parameters, bool stretch_enabled, Error* error)
{
	std::unique_ptr<OboeAudioStream> stream = std::make_unique<OboeAudioStream>(sample_rate, parameters);
	if (!stream->Initialize(stretch_enabled))
		stream.reset();
	return stream;
}
