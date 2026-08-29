// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "PrecompiledHeader.h"

#include "cheat_search.h"

#include "pcsx2/Memory.h"
#include "pcsx2/MemoryTypes.h"
#include "pcsx2/VMManager.h"

#include "common/Console.h"

#include <cstring>
#include <new>

EmuCheatSearch& EmuCheatSearch::getInstance()
{
	static EmuCheatSearch instance;
	return instance;
}

bool EmuCheatSearch::memAccessibleLocked() const
{
	return vmAlive && eeMem != nullptr && VMManager::HasValidVM();
}

void EmuCheatSearch::invalidateSearchLocked()
{
	searchActive = false;
	numMatches = 0;
	ramSize = 0;
	itemCount = 0;
	prevMemoryBuf.reset();
	matchMask.reset();
}

uint32_t EmuCheatSearch::loadValue(const uint8_t* p, unsigned itemBytes)
{
	// EE RAM is little-endian and so is every Android host we run on;
	// memcpy keeps the unaligned 16/32-bit loads UB-free.
	switch (itemBytes)
	{
		case 2:
		{
			uint16_t v;
			std::memcpy(&v, p, sizeof(v));
			return v;
		}
		case 4:
		{
			uint32_t v;
			std::memcpy(&v, p, sizeof(v));
			return v;
		}
		default:
			return *p;
	}
}

bool EmuCheatSearch::searchStart(unsigned bitSize)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!memAccessibleLocked())
		return false;
	if (bitSize < 3 || bitSize > 5)
		bitSize = 5;

	const unsigned itemBytes = bytesPerItem(bitSize);
	const uint32_t size = Ps2MemSize::ExposedRam;
	const uint32_t items = size / itemBytes;

	// The core builds with -fno-exceptions, so allocate nothrow and check.
	prevMemoryBuf.reset(new (std::nothrow) uint8_t[size]);
	matchMask.reset(new (std::nothrow) uint8_t[items]);
	if (!prevMemoryBuf || !matchMask)
	{
		Console.Error("@@ANDROID_CHEATSEARCH@@ searchStart OOM (ram=%u)", size);
		invalidateSearchLocked();
		return false;
	}

	std::memcpy(prevMemoryBuf.get(), eeMem->Main, size);
	std::memset(matchMask.get(), 1, items);
	ramSize = size;
	itemCount = items;
	numMatches = items;
	searchBitSize = bitSize;
	searchActive = true;
	Console.WriteLn("@@ANDROID_CHEATSEARCH@@ start bitSize=%u items=%u", bitSize, items);
	return true;
}

int EmuCheatSearch::searchUpdate(unsigned compareType, uint32_t operand)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!searchActive || !memAccessibleLocked())
		return -1;

	const unsigned itemBytes = bytesPerItem(searchBitSize);
	// Mask the operand to the search width so e.g. an 8-bit "= 300" can't
	// silently match nothing for a reason the UI can't show.
	const uint32_t valueMask = (itemBytes >= 4) ? 0xFFFFFFFFu : ((1u << (itemBytes * 8)) - 1);
	operand &= valueMask;

	const uint8_t* ram = eeMem->Main;
	for (uint32_t item = 0; item < itemCount; item++)
	{
		if (!matchMask[item])
			continue;

		const uint32_t idx = item * itemBytes;
		const uint32_t curr = loadValue(ram + idx, itemBytes);
		const uint32_t prev = loadValue(prevMemoryBuf.get() + idx, itemBytes);

		bool match = false;
		switch (compareType)
		{
			case COMPARE_EXACT:   match = (curr == operand); break;
			case COMPARE_LT:      match = (curr < prev); break;
			case COMPARE_LTE:     match = (curr <= prev); break;
			case COMPARE_GT:      match = (curr > prev); break;
			case COMPARE_GTE:     match = (curr >= prev); break;
			case COMPARE_EQ:      match = (curr == prev); break;
			case COMPARE_NEQ:     match = (curr != prev); break;
			case COMPARE_EQPLUS:  match = (curr == ((prev + operand) & valueMask)); break;
			case COMPARE_EQMINUS: match = (curr == ((prev - operand) & valueMask)); break;
			default: break;
		}

		if (!match)
		{
			matchMask[item] = 0;
			numMatches--;
		}
	}

	std::memcpy(prevMemoryBuf.get(), ram, ramSize);
	Console.WriteLn("@@ANDROID_CHEATSEARCH@@ update compare=%u matches=%u", compareType, numMatches);
	return static_cast<int>(numMatches);
}

int EmuCheatSearch::getMatchCount()
{
	std::lock_guard<std::mutex> lock(mutex);
	return searchActive ? static_cast<int>(numMatches) : -1;
}

int EmuCheatSearch::getSearchBitSize()
{
	std::lock_guard<std::mutex> lock(mutex);
	return searchActive ? static_cast<int>(searchBitSize) : -1;
}

std::vector<int64_t> EmuCheatSearch::getMatches(int offset, int maxCount)
{
	std::vector<int64_t> result;
	std::lock_guard<std::mutex> lock(mutex);
	if (!searchActive || !memAccessibleLocked() || maxCount <= 0 || offset < 0)
		return result;

	const unsigned itemBytes = bytesPerItem(searchBitSize);
	const uint8_t* ram = eeMem->Main;
	int matchIdx = 0;
	for (uint32_t item = 0; item < itemCount; item++)
	{
		if (!matchMask[item])
			continue;
		if (matchIdx >= offset)
		{
			const uint32_t idx = item * itemBytes;
			result.push_back(static_cast<int64_t>(idx));
			result.push_back(static_cast<int64_t>(loadValue(ram + idx, itemBytes)));
			if (static_cast<int>(result.size() / 2) >= maxCount)
				break;
		}
		matchIdx++;
	}
	return result;
}

void EmuCheatSearch::searchStop()
{
	std::lock_guard<std::mutex> lock(mutex);
	invalidateSearchLocked();
}

int64_t EmuCheatSearch::readValue(uint32_t address, unsigned bitSize)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!memAccessibleLocked() || bitSize < 3 || bitSize > 5)
		return -1;
	const unsigned itemBytes = bytesPerItem(bitSize);
	if (address + itemBytes > Ps2MemSize::ExposedRam || address + itemBytes < address)
		return -1;
	return static_cast<int64_t>(loadValue(eeMem->Main + address, itemBytes));
}

void EmuCheatSearch::onVMStart()
{
	std::lock_guard<std::mutex> lock(mutex);
	// A fresh boot must never inherit a previous game's session.
	invalidateSearchLocked();
	vmAlive = true;
}

void EmuCheatSearch::onVMShutdown()
{
	std::lock_guard<std::mutex> lock(mutex);
	vmAlive = false;
	invalidateSearchLocked();
}
