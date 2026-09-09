/**
 * @file ZZZAnimContext.h
 * @brief ZZZ 动画层的上下文聚合
 *
 * 读写权限通过不同的上下文类型区分；结构体仅保存指针，
 * 不拥有所指对象的生命周期。
 */
#pragma once

struct FZZZAnimSnapshot;
struct FZZZAnimTuning;
struct FZZZAnimStateMemory;

/** 只读上下文：供只读判定使用 */
struct FZZZAnimReadContext
{
	const FZZZAnimSnapshot* Snap = nullptr;
	const FZZZAnimTuning* Tuning = nullptr;
	const FZZZAnimStateMemory* Memory = nullptr;

	bool IsValid() const
	{
		return Snap != nullptr && Tuning != nullptr && Memory != nullptr;
	}
};

/** 可写上下文：供事件层使用 */
struct FZZZAnimWriteContext
{
	const FZZZAnimSnapshot* Snap = nullptr;
	const FZZZAnimTuning* Tuning = nullptr;
	FZZZAnimStateMemory* Memory = nullptr;

	bool IsValid() const
	{
		return Snap != nullptr && Tuning != nullptr && Memory != nullptr;
	}

	/** 降级为只读视图，供只读判定使用 */
	FZZZAnimReadContext ToRead() const
	{
		return FZZZAnimReadContext{ Snap, Tuning, Memory };
	}
};
