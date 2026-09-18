/**
 * @file ZZZAnimContext.h
 * @brief ZZZ 动画层的上下文聚合
 *
 * 结构体仅保存兼容表现更新所需的指针，不拥有所指对象的生命周期。
 */
#pragma once

struct FZZZAnimSnapshot;
struct FZZZAnimTuning;
struct FZZZAnimStateMemory;

/** 可写上下文：供兼容表现记忆更新使用。 */
struct FZZZAnimWriteContext
{
	const FZZZAnimSnapshot* Snap = nullptr;
	const FZZZAnimTuning* Tuning = nullptr;
	FZZZAnimStateMemory* Memory = nullptr;

	bool IsValid() const
	{
		return Snap != nullptr && Tuning != nullptr && Memory != nullptr;
	}
};
