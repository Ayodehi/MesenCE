#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Shared/MemoryOperationType.h"
#include "SNES/Debugger/IExecutionLogger.h"

//The SPC700's execution log: the same relationships and file format as the
//SNES main CPU's (SnesExecutionLogger), with CPU 1 in the header. The
//instructions the SPC700 ran, the addresses each read and wrote, and its
//control transfers; and, as two more access kinds, the addresses the DSP
//read and wrote by itself (sample data, the sample directory, the echo
//buffer). The format is described in SpcExecutionLogger.cpp.
class SpcExecutionLogger final : public IExecutionLogger
{
public:
	enum class Access : uint8_t
	{
		Read = 0,
		Write = 1,
		DspRead = 2,
		DspWrite = 3,
	};

	//Values continue SnesExecutionLogger::MemKind.
	enum class MemKind : uint8_t
	{
		Unmapped = 0,
		SpcRam = 6,
		SpcRom = 7,
	};

private:
	struct InstructionEntry
	{
		int32_t AbsAddress;
		MemKind Kind;
		uint32_t Count;
		uint32_t Taken = 0;
	};

	struct AccessEntry
	{
		int32_t AbsAddress;
		MemKind Kind;
		uint32_t Count;
		uint32_t Taken = 0;
	};

	struct FlowEntry
	{
		uint32_t Count = 0;
		uint32_t Taken = 0;
	};

	uint32_t _prgRomCrc32;
	uint32_t _prgRomSize;
	bool _enabled = false;

	unordered_map<uint32_t, InstructionEntry> _instructions;
	unordered_map<uint64_t, AccessEntry> _accesses;
	unordered_map<uint64_t, FlowEntry> _flows;

	static MemKind GetMemKind(AddressInfo& info);
	static bool GetFlowKind(uint8_t opCode, uint16_t fromPc, uint16_t toPc, uint8_t& kind);
	static void AddCount(uint32_t& count);
	void LogAccess(uint16_t pc, uint16_t addr, AddressInfo& info, Access access);
	vector<uint8_t> Write(bool delta);

public:
	SpcExecutionLogger(uint32_t prgRomCrc32, uint32_t prgRomSize);

	bool IsEnabled() override { return _enabled; }
	void SetEnabled(bool enabled) override { _enabled = enabled; }
	void Clear() override;

	//Called by SpcDebugger. `prevOpCode` and `prevPc` describe the instruction
	//that ran before `pc` (0xFF when there is none).
	void LogInstruction(uint16_t pc, AddressInfo& info, uint8_t prevOpCode, uint16_t prevPc);
	void LogRead(uint16_t pc, uint16_t addr, AddressInfo& info, MemoryOperationType type);
	void LogWrite(uint16_t pc, uint16_t addr, AddressInfo& info, MemoryOperationType type);
	//An access the DSP made by itself, not on behalf of an instruction.
	void LogDspRead(uint16_t addr);
	void LogDspWrite(uint16_t addr);

	vector<uint8_t> Serialize() override;
	vector<uint8_t> TakeDelta() override;
};
