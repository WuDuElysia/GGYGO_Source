/**
 * @file AnimSignalFrame.h
 * @brief 动画到逻辑层的本帧信号模型
 */
#pragma once

#include "CoreMinimal.h"

/** 统一保存动画曲线信号，并提供缺失信号归零的读取契约。 */
struct FAnimSignalFrame
{
	void Reset()
	{
		Values.Reset();
	}

	void Add(FName SignalName, float Value)
	{
		Values.Add(SignalName, Value);
	}

	float Get(FName SignalName) const
	{
		const float* Value = Values.Find(SignalName);
		return Value ? *Value : 0.f;
	}

private:
	TMap<FName, float> Values;
};
