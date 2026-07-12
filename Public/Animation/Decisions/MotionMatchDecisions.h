/**
 * @file MotionMatchDecisions.h
 * @brief 顶层辅助 MotionMatch 决策模块
 *
 * 封装 SMWithBaseAndMM 顶层辅助状态机的 2 个过渡判定函数（MM → Base）。
 * 纯 C++ 类，仅依赖 FAnimSnapshot 只读指针，不依赖 UObject 运行时。
 */
#pragma once

#include "CoreMinimal.h"

// 前向声明（编译隔离：头文件不需要完整定义）
struct FAnimSnapshot;

/**
 * 顶层辅助 MotionMatch 决策类
 *
 * 负责 MotionMatching 顶层辅助状态机的 2 条过渡判定：
 *   MM → Base(0s)   即时退出
 *   MM → Base(0.1s) 平滑退出
 *
 * 所有决策函数为 const，只读 Snap，线程安全。
 */
class FMotionMatchDecisions
{
public:
	/** 每帧由 AnimInstance 调用，设置快照指针 */
	void SetSnap(const FAnimSnapshot* InSnap);

	/** #44 MM → Base(0s)：强制退出 MotionMatching（bForceExitMotionMatching） */
	bool MM_To_Base_Instant() const;

	/** #45 MM → Base(0.1s)：匹配完成或未激活（bMotionMatchComplete || !bMotionMatchActive） */
	bool MM_To_Base_Smooth() const;

private:
	/** 当前帧快照指针（游戏线程写入，worker 线程只读） */
	const FAnimSnapshot* Snap = nullptr;
};
