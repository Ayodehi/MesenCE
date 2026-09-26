#pragma once
#include "pch.h"

//What the Lua API needs from a CPU's execution log (see
//SnesExecutionLogger.cpp for the format they share).
class IExecutionLogger
{
public:
	virtual ~IExecutionLogger() = default;

	virtual bool IsEnabled() = 0;
	virtual void SetEnabled(bool enabled) = 0;
	virtual void Clear() = 0;
	virtual vector<uint8_t> Serialize() = 0;
	virtual vector<uint8_t> TakeDelta() = 0;
};
