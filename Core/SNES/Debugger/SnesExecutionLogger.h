#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Shared/MemoryOperationType.h"
#include "SNES/Debugger/IExecutionLogger.h"

//Records what the SNES CPU did as a set of distinct relationships, each kept
//once with a count: the instructions it ran and the M/X/E states they ran in,
//the addresses each instruction read and wrote, the control transfers between
//instructions, and the DMA transfers. Unlike a trace log, its size depends on
//how much of the game was seen, not on how long it ran.
//
//The binary format written by Serialize() is described in
//SnesExecutionLogger.cpp. All values are little-endian.
class SnesExecutionLogger final : public IExecutionLogger
{
public:
	enum class FlowKind : uint8_t
	{
		Branch = 0,
		BranchNotTaken = 1,
		Jump = 2,
		IndirectJump = 3,
		Call = 4,
		IndirectCall = 5,
		Return = 6,
		ReturnFromInterrupt = 7,
		Nmi = 8,
		Irq = 9,
		SoftwareInterrupt = 10,
	};

	//A small, stable subset of MemoryType for the file format.
	enum class MemKind : uint8_t
	{
		Unmapped = 0,
		PrgRom = 1,
		WorkRam = 2,
		SaveRam = 3,
		Register = 4,
		Other = 5,
	};

private:
	//`Taken` and `TakenStates` are what the last TakeDelta() reported, so the
	//next one can report only what changed since.
	struct InstructionEntry
	{
		int32_t AbsAddress;
		MemKind Kind;
		uint8_t StateMask;
		uint32_t Count;
		uint32_t Taken = 0;
		uint8_t TakenStates = 0;
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
	unordered_map<uint64_t, AccessEntry> _dma;

	//Per channel (0-7 DMA, 8-15 HDMA): the address of a DMA read awaiting
	//its write. HDMA can run in the middle of a GP-DMA, so they are kept apart.
	int64_t _pendingDmaRead[16] = {};
	AddressInfo _pendingDmaReadAbs[16] = {};
	//The instruction that last wrote $420B, which started the running GP-DMA.
	uint32_t _gpDmaPc = 0;

	void ClearPendingDma();

	static MemKind GetMemKind(AddressInfo& info);
	static bool GetFlowKind(uint8_t opCode, uint32_t fromPc, uint32_t toPc, FlowKind& kind);
	static void AddCount(uint32_t& count);
	void LogDma(uint32_t busA, AddressInfo& busAAbs, bool toBusA, uint8_t slot, uint8_t dest, uint8_t mode);
	vector<uint8_t> Write(bool delta);

public:
	SnesExecutionLogger(uint32_t prgRomCrc32, uint32_t prgRomSize);

	bool IsEnabled() override { return _enabled; }
	void SetEnabled(bool enabled) override;
	void Clear() override;

	//Called by SnesDebugger. `prevOpCode` and `prevPc` describe the instruction
	//that ran before `pc` (0xFF when there is none, e.g. after an interrupt).
	void LogInstruction(uint32_t pc, AddressInfo& info, uint8_t ps, bool emulationMode, uint8_t prevOpCode, uint32_t prevPc);
	//A control transfer from the instruction at `fromPc` (opcode `opCode`) to `toPc`.
	void LogTransfer(uint8_t opCode, uint32_t fromPc, uint32_t toPc);
	void LogInterrupt(uint32_t originalPc, uint32_t handlerPc, bool forNmi);
	void LogRead(uint32_t pc, uint32_t addr, AddressInfo& info, MemoryOperationType type);
	void LogWrite(uint32_t pc, uint32_t addr, AddressInfo& info, MemoryOperationType type);

	//One byte of a DMA or HDMA transfer. `channel` is the DMA controller's
	//active channel (bit 6 set for HDMA); `dest` and `mode` are that channel's
	//B-bus address ($21xx) and transfer mode.
	void LogDmaRead(uint32_t addr, AddressInfo& info, uint8_t channel);
	void LogDmaWrite(uint32_t addr, AddressInfo& info, uint8_t channel, uint8_t dest, uint8_t mode);

	//The whole log.
	vector<uint8_t> Serialize() override;
	//What was recorded since the previous call, in the same format: entries
	//that are new or whose count or width states changed, each count being
	//the increase. Merging every delta in order gives the whole log.
	vector<uint8_t> TakeDelta() override;
};
