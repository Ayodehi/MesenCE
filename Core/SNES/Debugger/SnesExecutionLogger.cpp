#include "pch.h"
#include "SNES/Debugger/SnesExecutionLogger.h"
#include "SNES/SnesCpuTypes.h"
#include "Shared/MemoryType.h"

//File format, version 1 (all values little-endian):
//
//  Header (20 bytes)
//    0   char[4]  "MXLG"
//    4   u16      version (1)
//    6   u8       CPU (0 = SNES main CPU)
//    7   u8       reserved (0)
//    8   u32      CRC32 of the PRG ROM (the value the CDL file uses)
//    12  u32      PRG ROM size in bytes
//    16  u32      number of sections
//
//  Each section: char[4] tag, u32 record size, u32 record count, then the
//  records. Readers skip sections they don't know, using the record size.
//  Records are sorted by their first fields. Counts saturate at 0xFFFFFFFF.
//  CPU addresses are 24-bit; "abs" is the offset in the memory named by
//  "kind" (see MemKind), or -1 when unmapped.
//
//  "INST" (16 bytes): an instruction that ran
//    u32 pc, i32 abs, u8 kind, u8 states, u16 reserved, u32 count
//    `states` has bit (M + 2*X + 4*E) set for each flag state it ran in,
//    where M/X are 1 for 8-bit and E is 1 in emulation mode.
//
//  "ACCS" (24 bytes): a run of addresses one instruction read or wrote
//    u32 pc, u32 address, i32 abs, u32 length, u8 access (0 = read,
//    1 = write), u8 kind, u16 reserved, u32 count (summed over the run)
//
//  "FLOW" (16 bytes): a control transfer
//    u32 from, u32 to, u8 flow (see FlowKind), u8[3] reserved, u32 count
//    `from` is the transferring instruction, or for NMI/IRQ the instruction
//    that was about to run.
//
//  "DMA " (24 bytes): a run of A-bus addresses a DMA channel moved
//    u32 pc, u32 address, i32 abs, u32 length, u8 B-bus address (the
//    channel's $43x1, so $18 for VRAM whether a byte went to $2118 or $2119),
//    u8 flags (bit 0: B-bus to A-bus, bit 1: HDMA, bits 2-4: transfer mode),
//    u8 kind, u8 channel (0-7), u32 count
//    `pc` is the instruction that wrote $420B for a GP-DMA, 0 for HDMA.
//    HDMA table reads that transfer nothing (line counts, indirect
//    addresses) are not recorded.

namespace
{
	void Put8(vector<uint8_t>& out, uint8_t value)
	{
		out.push_back(value);
	}

	void Put16(vector<uint8_t>& out, uint16_t value)
	{
		out.push_back(value & 0xFF);
		out.push_back(value >> 8);
	}

	void Put32(vector<uint8_t>& out, uint32_t value)
	{
		for(int i = 0; i < 4; i++) {
			out.push_back((value >> (i * 8)) & 0xFF);
		}
	}

	void PutTag(vector<uint8_t>& out, const char* tag)
	{
		for(int i = 0; i < 4; i++) {
			out.push_back((uint8_t)tag[i]);
		}
	}

	//The keys of the entries `include` accepts, sorted.
	template<typename K, typename V, typename F>
	vector<K> SortedKeys(const unordered_map<K, V>& map, F include)
	{
		vector<K> keys;
		keys.reserve(map.size());
		for(auto& entry : map) {
			if(include(entry.second)) {
				keys.push_back(entry.first);
			}
		}
		std::sort(keys.begin(), keys.end());
		return keys;
	}
}

SnesExecutionLogger::SnesExecutionLogger(uint32_t prgRomCrc32, uint32_t prgRomSize)
{
	_prgRomCrc32 = prgRomCrc32;
	_prgRomSize = prgRomSize;
	ClearPendingDma();
}

void SnesExecutionLogger::ClearPendingDma()
{
	for(int i = 0; i < 16; i++) {
		_pendingDmaRead[i] = -1;
	}
}

void SnesExecutionLogger::SetEnabled(bool enabled)
{
	_enabled = enabled;
	ClearPendingDma();
}

void SnesExecutionLogger::Clear()
{
	_instructions.clear();
	_accesses.clear();
	_flows.clear();
	_dma.clear();
	ClearPendingDma();
}

SnesExecutionLogger::MemKind SnesExecutionLogger::GetMemKind(AddressInfo& info)
{
	if(info.Address < 0) {
		return MemKind::Unmapped;
	}
	switch(info.Type) {
		case MemoryType::SnesPrgRom: return MemKind::PrgRom;
		case MemoryType::SnesWorkRam: return MemKind::WorkRam;
		case MemoryType::SnesSaveRam: return MemKind::SaveRam;
		case MemoryType::SnesRegister: return MemKind::Register;
		default: return MemKind::Other;
	}
}

void SnesExecutionLogger::AddCount(uint32_t& count)
{
	if(count != 0xFFFFFFFF) {
		count++;
	}
}

bool SnesExecutionLogger::GetFlowKind(uint8_t opCode, uint32_t fromPc, uint32_t toPc, FlowKind& kind)
{
	switch(opCode) {
		case 0x10:
		case 0x30:
		case 0x50:
		case 0x70:
		case 0x90:
		case 0xB0:
		case 0xD0:
		case 0xF0: {
			//Conditional branches are 2 bytes; the fall-through stays in the bank
			uint32_t next = (fromPc & 0xFF0000) | ((fromPc + 2) & 0xFFFF);
			kind = toPc == next ? FlowKind::BranchNotTaken : FlowKind::Branch;
			return true;
		}

		case 0x80:
		case 0x82: kind = FlowKind::Branch; return true;
		case 0x4C:
		case 0x5C: kind = FlowKind::Jump; return true;
		case 0x6C:
		case 0x7C:
		case 0xDC: kind = FlowKind::IndirectJump; return true;
		case 0x20:
		case 0x22: kind = FlowKind::Call; return true;
		case 0xFC: kind = FlowKind::IndirectCall; return true;
		case 0x60:
		case 0x6B: kind = FlowKind::Return; return true;
		case 0x40: kind = FlowKind::ReturnFromInterrupt; return true;
		case 0x00:
		case 0x02: kind = FlowKind::SoftwareInterrupt; return true;
		default: return false;
	}
}

void SnesExecutionLogger::LogInstruction(uint32_t pc, AddressInfo& info, uint8_t ps, bool emulationMode, uint8_t prevOpCode, uint32_t prevPc)
{
	uint8_t state = ((ps & ProcFlags::MemoryMode8) ? 1 : 0) | ((ps & ProcFlags::IndexMode8) ? 2 : 0) | (emulationMode ? 4 : 0);
	auto result = _instructions.try_emplace(pc, InstructionEntry { info.Address, GetMemKind(info), 0, 0 });
	InstructionEntry& entry = result.first->second;
	entry.StateMask |= (1 << state);
	AddCount(entry.Count);

	if(prevOpCode != 0xFF) {
		LogTransfer(prevOpCode, prevPc, pc);
	}
}

void SnesExecutionLogger::LogTransfer(uint8_t opCode, uint32_t fromPc, uint32_t toPc)
{
	FlowKind kind;
	if(GetFlowKind(opCode, fromPc, toPc, kind)) {
		uint64_t key = ((uint64_t)(fromPc & 0xFFFFFF) << 32) | ((uint64_t)(toPc & 0xFFFFFF) << 8) | (uint8_t)kind;
		AddCount(_flows[key].Count);
	}
}

void SnesExecutionLogger::LogInterrupt(uint32_t originalPc, uint32_t handlerPc, bool forNmi)
{
	FlowKind kind = forNmi ? FlowKind::Nmi : FlowKind::Irq;
	uint64_t key = ((uint64_t)(originalPc & 0xFFFFFF) << 32) | ((uint64_t)(handlerPc & 0xFFFFFF) << 8) | (uint8_t)kind;
	AddCount(_flows[key].Count);
}

void SnesExecutionLogger::LogDmaRead(uint32_t addr, AddressInfo& info, uint8_t channel)
{
	uint8_t slot = ((channel & 0x40) ? 8 : 0) | (channel & 0x07);
	_pendingDmaRead[slot] = addr & 0xFFFFFF;
	_pendingDmaReadAbs[slot] = info;
}

void SnesExecutionLogger::LogDmaWrite(uint32_t addr, AddressInfo& info, uint8_t channel, uint8_t dest, uint8_t mode)
{
	uint8_t slot = ((channel & 0x40) ? 8 : 0) | (channel & 0x07);
	if(_pendingDmaRead[slot] < 0) {
		return;
	}
	uint32_t readAddr = (uint32_t)_pendingDmaRead[slot];
	if((addr & 0xFF00) == 0x2100 && (addr & 0x400000) == 0) {
		//A-bus to B-bus: the read was the A-bus source
		LogDma(readAddr, _pendingDmaReadAbs[slot], false, slot, dest, mode);
	} else {
		//B-bus to A-bus: the write is the A-bus destination
		LogDma(addr & 0xFFFFFF, info, true, slot, dest, mode);
	}
	_pendingDmaRead[slot] = -1;
}

void SnesExecutionLogger::LogRead(uint32_t pc, uint32_t addr, AddressInfo& info, MemoryOperationType type)
{
	if(type != MemoryOperationType::Read) {
		return;
	}
	uint64_t key = ((uint64_t)(pc & 0xFFFFFF) << 32) | ((uint64_t)(addr & 0xFFFFFF) << 8);
	auto result = _accesses.try_emplace(key, AccessEntry { info.Address, GetMemKind(info), 0 });
	AddCount(result.first->second.Count);
}

void SnesExecutionLogger::LogWrite(uint32_t pc, uint32_t addr, AddressInfo& info, MemoryOperationType type)
{
	if(type != MemoryOperationType::Write) {
		return;
	}
	if((addr & 0xFFFF) == 0x420B && (addr & 0x400000) == 0) {
		_gpDmaPc = pc & 0xFFFFFF;
	}
	uint64_t key = ((uint64_t)(pc & 0xFFFFFF) << 32) | ((uint64_t)(addr & 0xFFFFFF) << 8) | 1;
	auto result = _accesses.try_emplace(key, AccessEntry { info.Address, GetMemKind(info), 0 });
	AddCount(result.first->second.Count);
}

void SnesExecutionLogger::LogDma(uint32_t busA, AddressInfo& busAAbs, bool toBusA, uint8_t slot, uint8_t dest, uint8_t mode)
{
	bool hdma = slot >= 8;
	uint32_t pc = hdma ? 0 : _gpDmaPc;
	uint8_t flags = (toBusA ? 1 : 0) | (hdma ? 2 : 0) | ((mode & 0x07) << 2);
	//pc (24) | A-bus address (24) | B-bus address (8) | channel (3) | flags (5)
	uint64_t key = ((uint64_t)pc << 40) | ((uint64_t)busA << 16) | ((uint64_t)dest << 8) | ((uint64_t)(slot & 0x07) << 5) | flags;
	auto result = _dma.try_emplace(key, AccessEntry { busAAbs.Address, GetMemKind(busAAbs), 0 });
	AddCount(result.first->second.Count);
}

vector<uint8_t> SnesExecutionLogger::Serialize()
{
	return Write(false);
}

vector<uint8_t> SnesExecutionLogger::TakeDelta()
{
	vector<uint8_t> out = Write(true);
	for(auto& e : _instructions) {
		e.second.Taken = e.second.Count;
		e.second.TakenStates = e.second.StateMask;
	}
	for(auto& e : _accesses) {
		e.second.Taken = e.second.Count;
	}
	for(auto& e : _flows) {
		e.second.Taken = e.second.Count;
	}
	for(auto& e : _dma) {
		e.second.Taken = e.second.Count;
	}
	return out;
}

vector<uint8_t> SnesExecutionLogger::Write(bool delta)
{
	//In a delta, only what changed since the last take, counting the increase
	auto newInstruction = [&](const InstructionEntry& e) { return !delta || e.Count != e.Taken || e.StateMask != e.TakenStates; };
	auto newAccess = [&](const AccessEntry& e) { return !delta || e.Count != e.Taken; };
	auto newFlow = [&](const FlowEntry& e) { return !delta || e.Count != e.Taken; };
	auto accessCount = [&](const AccessEntry& e) { return delta ? e.Count - e.Taken : e.Count; };

	vector<uint8_t> out;
	PutTag(out, "MXLG");
	Put16(out, 1);
	Put8(out, 0);
	Put8(out, 0);
	Put32(out, _prgRomCrc32);
	Put32(out, _prgRomSize);
	Put32(out, 4);

	auto countAt = [&](size_t pos, uint32_t count) {
		for(int i = 0; i < 4; i++) {
			out[pos + i] = (count >> (i * 8)) & 0xFF;
		}
	};

	//Instructions
	vector<uint32_t> pcs = SortedKeys(_instructions, newInstruction);
	PutTag(out, "INST");
	Put32(out, 16);
	Put32(out, (uint32_t)pcs.size());
	for(uint32_t pc : pcs) {
		InstructionEntry& e = _instructions[pc];
		Put32(out, pc);
		Put32(out, (uint32_t)e.AbsAddress);
		Put8(out, (uint8_t)e.Kind);
		Put8(out, e.StateMask);
		Put16(out, 0);
		Put32(out, delta ? e.Count - e.Taken : e.Count);
	}

	//Accesses, merged into runs of consecutive addresses
	PutTag(out, "ACCS");
	Put32(out, 24);
	size_t accessCountPos = out.size();
	Put32(out, 0);
	uint32_t runs = 0;
	vector<uint64_t> keys = SortedKeys(_accesses, newAccess);
	for(size_t i = 0; i < keys.size();) {
		uint64_t key = keys[i];
		uint32_t pc = (uint32_t)(key >> 32);
		uint32_t addr = (uint32_t)(key >> 8) & 0xFFFFFF;
		uint8_t access = key & 0xFF;
		AccessEntry& first = _accesses[key];
		uint32_t len = 1;
		uint64_t count = accessCount(first);
		size_t j = i + 1;
		while(j < keys.size()) {
			uint64_t next = keys[j];
			AccessEntry& e = _accesses[next];
			bool adjacent = (uint32_t)(next >> 32) == pc && (next & 0xFF) == access && ((uint32_t)(next >> 8) & 0xFFFFFF) == addr + len && e.Kind == first.Kind && (first.AbsAddress < 0 ? e.AbsAddress < 0 : e.AbsAddress == first.AbsAddress + (int32_t)len);
			if(!adjacent) {
				break;
			}
			count += accessCount(e);
			len++;
			j++;
		}
		Put32(out, pc);
		Put32(out, addr);
		Put32(out, (uint32_t)first.AbsAddress);
		Put32(out, len);
		Put8(out, access);
		Put8(out, (uint8_t)first.Kind);
		Put16(out, 0);
		Put32(out, (uint32_t)std::min<uint64_t>(count, 0xFFFFFFFF));
		runs++;
		i = j;
	}
	countAt(accessCountPos, runs);

	//Control transfers
	vector<uint64_t> flowKeys = SortedKeys(_flows, newFlow);
	PutTag(out, "FLOW");
	Put32(out, 16);
	Put32(out, (uint32_t)flowKeys.size());
	for(uint64_t key : flowKeys) {
		FlowEntry& e = _flows[key];
		Put32(out, (uint32_t)(key >> 32));
		Put32(out, (uint32_t)(key >> 8) & 0xFFFFFF);
		Put8(out, key & 0xFF);
		Put8(out, 0);
		Put16(out, 0);
		Put32(out, delta ? e.Count - e.Taken : e.Count);
	}

	//DMA, merged into runs of consecutive A-bus addresses
	PutTag(out, "DMA ");
	Put32(out, 24);
	size_t dmaCountPos = out.size();
	Put32(out, 0);
	runs = 0;
	keys = SortedKeys(_dma, newAccess);
	for(size_t i = 0; i < keys.size();) {
		uint64_t key = keys[i];
		uint32_t pc = (uint32_t)(key >> 40);
		uint32_t addr = (uint32_t)(key >> 16) & 0xFFFFFF;
		uint8_t reg = (key >> 8) & 0xFF;
		uint8_t channel = (key >> 5) & 0x07;
		uint8_t flags = key & 0x1F;
		AccessEntry& first = _dma[key];
		uint32_t len = 1;
		uint64_t count = accessCount(first);
		size_t j = i + 1;
		while(j < keys.size()) {
			uint64_t next = keys[j];
			AccessEntry& e = _dma[next];
			bool adjacent = (uint32_t)(next >> 40) == pc && (next & 0xFFFF) == (key & 0xFFFF) && ((uint32_t)(next >> 16) & 0xFFFFFF) == addr + len && e.Kind == first.Kind && (first.AbsAddress < 0 ? e.AbsAddress < 0 : e.AbsAddress == first.AbsAddress + (int32_t)len);
			if(!adjacent) {
				break;
			}
			count += accessCount(e);
			len++;
			j++;
		}
		Put32(out, pc);
		Put32(out, addr);
		Put32(out, (uint32_t)first.AbsAddress);
		Put32(out, len);
		Put8(out, reg);
		Put8(out, flags);
		Put8(out, (uint8_t)first.Kind);
		Put8(out, channel);
		Put32(out, (uint32_t)std::min<uint64_t>(count, 0xFFFFFFFF));
		runs++;
		i = j;
	}
	countAt(dmaCountPos, runs);

	return out;
}
