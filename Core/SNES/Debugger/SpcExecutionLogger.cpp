#include "pch.h"
#include "SNES/Debugger/SpcExecutionLogger.h"
#include "SNES/Debugger/SpcDisUtils.h"
#include "Shared/MemoryType.h"

//File format: SnesExecutionLogger's version 1 (see SnesExecutionLogger.cpp),
//with these differences:
//
//  Header: CPU = 1 (the SPC700); the CRC32 and size are still the PRG ROM's,
//  to say which game the log was recorded from. 3 sections.
//
//  Addresses are the SPC700's 16-bit addresses. "abs" is the offset in audio
//  RAM (kind 6) or in the boot ROM (kind 7, while it is mapped at $FFC0).
//
//  "INST" (16 bytes): u32 pc, i32 abs, u8 kind, u8 states (always 0),
//    u16 reserved, u32 count
//
//  "ACCS" (24 bytes): as for the main CPU, with two more access kinds:
//    2 = the DSP read the addresses by itself (the sample directory, BRR
//    sample data and the echo buffer), 3 = the DSP wrote them (the echo
//    buffer). For those, pc is 0.
//
//  "FLOW" (16 bytes): as for the main CPU. Flow kinds used: 0 branch
//    (BRA too), 1 branch not taken, 2 jump (JMP !a), 3 indirect jump
//    (JMP [!a+X]), 4 call (CALL, PCALL), 5 indirect call (TCALL, through its
//    vector), 6 return (RET), 7 return from interrupt (RETI), 10 software
//    interrupt (BRK).
//
//  There is no "DMA " section.

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

SpcExecutionLogger::SpcExecutionLogger(uint32_t prgRomCrc32, uint32_t prgRomSize)
{
	_prgRomCrc32 = prgRomCrc32;
	_prgRomSize = prgRomSize;
}

void SpcExecutionLogger::Clear()
{
	_instructions.clear();
	_accesses.clear();
	_flows.clear();
}

SpcExecutionLogger::MemKind SpcExecutionLogger::GetMemKind(AddressInfo& info)
{
	if(info.Address < 0) {
		return MemKind::Unmapped;
	}
	return info.Type == MemoryType::SpcRom ? MemKind::SpcRom : MemKind::SpcRam;
}

void SpcExecutionLogger::AddCount(uint32_t& count)
{
	if(count != 0xFFFFFFFF) {
		count++;
	}
}

bool SpcExecutionLogger::GetFlowKind(uint8_t opCode, uint16_t fromPc, uint16_t toPc, uint8_t& kind)
{
	bool conditional = false;
	switch(opCode) {
		case 0x10:
		case 0x30:
		case 0x50:
		case 0x70:
		case 0x90:
		case 0xB0:
		case 0xD0:
		case 0xF0:
		case 0x2E: //CBNE dp
		case 0xDE: //CBNE dp+X
		case 0x6E: //DBNZ dp
		case 0xFE: //DBNZ Y
			conditional = true;
			break;

		case 0x2F: kind = 0; return true; //BRA
		case 0x5F: kind = 2; return true; //JMP !a
		case 0x1F: kind = 3; return true; //JMP [!a+X]
		case 0x3F:
		case 0x4F: kind = 4; return true; //CALL, PCALL
		case 0x6F: kind = 6; return true; //RET
		case 0x7F: kind = 7; return true; //RETI
		case 0x0F: kind = 10; return true; //BRK

		default:
			if((opCode & 0x0F) == 0x03) {
				conditional = true; //BBS, BBC
			} else if((opCode & 0x0F) == 0x01) {
				kind = 5; //TCALL
				return true;
			} else {
				return false;
			}
			break;
	}
	if(conditional) {
		uint16_t next = fromPc + SpcDisUtils::GetOpSize(opCode);
		kind = toPc == next ? 1 : 0;
	}
	return conditional;
}

void SpcExecutionLogger::LogInstruction(uint16_t pc, AddressInfo& info, uint8_t prevOpCode, uint16_t prevPc)
{
	MemKind kind = GetMemKind(info);
	uint32_t key = pc | ((uint32_t)kind << 16);
	auto result = _instructions.try_emplace(key, InstructionEntry { info.Address, kind, 0 });
	AddCount(result.first->second.Count);

	uint8_t flow;
	if(prevOpCode != 0xFF && GetFlowKind(prevOpCode, prevPc, pc, flow)) {
		uint64_t flowKey = ((uint64_t)prevPc << 32) | ((uint64_t)pc << 8) | flow;
		AddCount(_flows[flowKey].Count);
	}
}

void SpcExecutionLogger::LogAccess(uint16_t pc, uint16_t addr, AddressInfo& info, Access access)
{
	uint64_t key = ((uint64_t)pc << 32) | ((uint64_t)addr << 8) | (uint8_t)access;
	auto result = _accesses.try_emplace(key, AccessEntry { info.Address, GetMemKind(info), 0 });
	AddCount(result.first->second.Count);
}

void SpcExecutionLogger::LogRead(uint16_t pc, uint16_t addr, AddressInfo& info, MemoryOperationType type)
{
	if(type == MemoryOperationType::Read) {
		LogAccess(pc, addr, info, Access::Read);
	}
}

void SpcExecutionLogger::LogWrite(uint16_t pc, uint16_t addr, AddressInfo& info, MemoryOperationType type)
{
	if(type == MemoryOperationType::Write) {
		LogAccess(pc, addr, info, Access::Write);
	}
}

void SpcExecutionLogger::LogDspRead(uint16_t addr)
{
	//The DSP always reads RAM, never the boot ROM
	AddressInfo info { (int32_t)addr, MemoryType::SpcRam };
	LogAccess(0, addr, info, Access::DspRead);
}

void SpcExecutionLogger::LogDspWrite(uint16_t addr)
{
	AddressInfo info { (int32_t)addr, MemoryType::SpcRam };
	LogAccess(0, addr, info, Access::DspWrite);
}

vector<uint8_t> SpcExecutionLogger::Serialize()
{
	return Write(false);
}

vector<uint8_t> SpcExecutionLogger::TakeDelta()
{
	vector<uint8_t> out = Write(true);
	for(auto& e : _instructions) {
		e.second.Taken = e.second.Count;
	}
	for(auto& e : _accesses) {
		e.second.Taken = e.second.Count;
	}
	for(auto& e : _flows) {
		e.second.Taken = e.second.Count;
	}
	return out;
}

vector<uint8_t> SpcExecutionLogger::Write(bool delta)
{
	auto newInstruction = [&](const InstructionEntry& e) { return !delta || e.Count != e.Taken; };
	auto newAccess = [&](const AccessEntry& e) { return !delta || e.Count != e.Taken; };
	auto newFlow = [&](const FlowEntry& e) { return !delta || e.Count != e.Taken; };
	auto accessCount = [&](const AccessEntry& e) { return delta ? e.Count - e.Taken : e.Count; };

	vector<uint8_t> out;
	PutTag(out, "MXLG");
	Put16(out, 1);
	Put8(out, 1);
	Put8(out, 0);
	Put32(out, _prgRomCrc32);
	Put32(out, _prgRomSize);
	Put32(out, 3);

	//Instructions
	vector<uint32_t> keys = SortedKeys(_instructions, newInstruction);
	PutTag(out, "INST");
	Put32(out, 16);
	Put32(out, (uint32_t)keys.size());
	for(uint32_t key : keys) {
		InstructionEntry& e = _instructions[key];
		Put32(out, key & 0xFFFF);
		Put32(out, (uint32_t)e.AbsAddress);
		Put8(out, (uint8_t)e.Kind);
		Put8(out, 0);
		Put16(out, 0);
		Put32(out, delta ? e.Count - e.Taken : e.Count);
	}

	//Accesses, merged into runs of consecutive addresses
	PutTag(out, "ACCS");
	Put32(out, 24);
	size_t countPos = out.size();
	Put32(out, 0);
	uint32_t runs = 0;
	vector<uint64_t> accessKeys = SortedKeys(_accesses, newAccess);
	for(size_t i = 0; i < accessKeys.size();) {
		uint64_t key = accessKeys[i];
		uint32_t pc = (uint32_t)(key >> 32);
		uint32_t addr = (uint32_t)(key >> 8) & 0xFFFF;
		uint8_t access = key & 0xFF;
		AccessEntry& first = _accesses[key];
		uint32_t len = 1;
		uint64_t count = accessCount(first);
		size_t j = i + 1;
		while(j < accessKeys.size()) {
			uint64_t next = accessKeys[j];
			AccessEntry& e = _accesses[next];
			bool adjacent = (uint32_t)(next >> 32) == pc && (next & 0xFF) == access && ((uint32_t)(next >> 8) & 0xFFFF) == addr + len && e.Kind == first.Kind && (first.AbsAddress < 0 ? e.AbsAddress < 0 : e.AbsAddress == first.AbsAddress + (int32_t)len);
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
	for(int i = 0; i < 4; i++) {
		out[countPos + i] = (runs >> (i * 8)) & 0xFF;
	}

	//Control transfers
	vector<uint64_t> flowKeys = SortedKeys(_flows, newFlow);
	PutTag(out, "FLOW");
	Put32(out, 16);
	Put32(out, (uint32_t)flowKeys.size());
	for(uint64_t key : flowKeys) {
		FlowEntry& e = _flows[key];
		Put32(out, (uint32_t)(key >> 32));
		Put32(out, (uint32_t)(key >> 8) & 0xFFFF);
		Put8(out, key & 0xFF);
		Put8(out, 0);
		Put16(out, 0);
		Put32(out, delta ? e.Count - e.Taken : e.Count);
	}

	return out;
}
