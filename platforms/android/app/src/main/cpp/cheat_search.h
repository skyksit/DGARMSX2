// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// RetroArch-style RAM value scanner over EE main memory, driving the in-game
// cheat search UI. Ported from dsam3/LibretroDroid's CheatSearchManager
// (itself a port of RetroArch cheat_manager.c), simplified for PS2:
//
// - Single buffer: eeMem->Main, window = Ps2MemSize::ExposedRam captured at
//   searchStart. Item offsets ARE pnach EE addresses (0x00000000-0x01FFFFFF),
//   so the UI can emit `patch=1,EE,<addr>,...` lines verbatim.
// - Byte-sized items only (bitSize 3/4/5 = 1/2/4 bytes) — no sub-byte modes.
// - No freeze list: freezing is pnach's job (PPT_CONTINUOUSLY re-applies every
//   vsync on the CPU thread), so this class only ever READS guest memory.
// - Reads are raw host-pointer loads, same tier as Achievements::ClientReadMemory,
//   with no VM pause required. The overlay opens from the pause menu anyway,
//   so in practice the VM is parked while scans run.
// - Search state lives here (not in the UI) so a session survives the overlay
//   being closed: search -> resume game -> change value -> reopen -> refine.
// - All public methods serialize on an internal mutex and are safe to call
//   from any thread. runVMThread brackets VM lifetime via onVMStart /
//   onVMShutdown; onVMShutdown must run BEFORE VMManager::Shutdown so an
//   in-flight scan drains before eeMem is torn down.
class EmuCheatSearch
{
public:
	static EmuCheatSearch& getInstance();
	EmuCheatSearch(EmuCheatSearch const&) = delete;
	void operator=(EmuCheatSearch const&) = delete;

	// RetroArch enum cheat_search_type order — keep in sync with the
	// COMPARE_* constants in dsam3's Armsx2 cheat search UI.
	enum CompareType : unsigned
	{
		COMPARE_EXACT = 0, // curr == operand
		COMPARE_LT, // curr <  prev
		COMPARE_LTE, // curr <= prev
		COMPARE_GT, // curr >  prev
		COMPARE_GTE, // curr >= prev
		COMPARE_EQ, // curr == prev (unchanged)
		COMPARE_NEQ, // curr != prev (changed)
		COMPARE_EQPLUS, // curr == prev + operand
		COMPARE_EQMINUS, // curr == prev - operand
	};

	// bitSize: 3=8bit, 4=16bit, 5=32bit (RetroArch search_bit_size convention).
	// Snapshots ExposedRam and marks every aligned item a candidate.
	// False when the VM is down or the snapshot allocation fails.
	bool searchStart(unsigned bitSize);

	// One comparison pass; narrows candidates against the previous snapshot,
	// then refreshes the snapshot. Returns remaining match count, -1 = no session.
	int searchUpdate(unsigned compareType, uint32_t operand);

	// Remaining match count, -1 = no session. Cheap; safe as a liveness probe.
	int getMatchCount();

	// Bit size of the live session (3/4/5), -1 = no session. Lets the UI
	// restore the chip state when the overlay is reopened.
	int getSearchBitSize();

	// From the offset-th match, up to maxCount matches as flat
	// [eeAddress, currentValue] pairs. Values are re-read live.
	std::vector<int64_t> getMatches(int offset, int maxCount);

	void searchStop();

	// Live read of one value (for refreshing a displayed match). -1 on any
	// failure (VM down, out of range, bad bitSize).
	int64_t readValue(uint32_t address, unsigned bitSize);

	// VM lifetime brackets, called from runVMThread. onVMShutdown drops the
	// session and blocks guest-memory access; a scan already holding the
	// mutex drains first, which is what makes the teardown safe.
	void onVMStart();
	void onVMShutdown();

private:
	EmuCheatSearch() = default;

	// mutex must be held.
	bool memAccessibleLocked() const;
	void invalidateSearchLocked();
	static unsigned bytesPerItem(unsigned bitSize) { return 1u << (bitSize - 3); }
	static uint32_t loadValue(const uint8_t* p, unsigned itemBytes);

	std::mutex mutex;

	// True between onVMStart and onVMShutdown — the only window in which
	// dereferencing eeMem is allowed.
	bool vmAlive = false;

	bool searchActive = false;
	unsigned searchBitSize = 5;
	uint32_t ramSize = 0; // ExposedRam captured at searchStart
	// Raw nothrow buffers, not std::vector — the core builds with -fno-exceptions,
	// so allocation failure must be observable without bad_alloc.
	std::unique_ptr<uint8_t[]> prevMemoryBuf; // full RAM snapshot (last search pass)
	std::unique_ptr<uint8_t[]> matchMask; // 1 byte per item, 1 = still a candidate
	uint32_t itemCount = 0; // matchMask length (ramSize / bytesPerItem)
	uint32_t numMatches = 0;
};
