/**
 * @file StopGaitDecisions.h
 * @brief Layer 4.3 StopGait 停步步态决策模块
 *
 * 封装停步状态机内部的步态路由判定函数（Sprint/Run/Walk/ForceRun）。
 * 纯 C++ 类，仅依赖 FAnimSnapshot 只读指针，不依赖 UObject 运行时。
 */
#pragma once

#include "CoreMinimal.h"

// 前向声明（编译隔离：头文件不需要完整定义）
struct FAnimSnapshot;

/**
 * Layer 4.3 StopGait 决策类
 *
 * 负责停步子机内部的步态路由判定：
 *   StopGait → Sprint / Run / Walk
 *   ForceRun → State（步态不为 Run 时强制跳转）
 *
 * 所有决策函数为 const，只读 Snap，线程安全。
 */
class FStopGaitDecisions
{
public:
	/** 每帧由 AnimInstance 调用，设置快照指针 */
	void SetSnap(const FAnimSnapshot* InSnap);

	/** 停步步态为 Sprint */
	bool StopGait_Is_Sprint() const;

	/** 停步步态为 Run（含 None 降级） */
	bool StopGait_Is_Run() const;

	/** 停步步态为 Walk */
	bool StopGait_Is_Walk() const;

	/** 非 Run 步态时强制跳转到目标状态 */
	bool StopGait_ForceRun_To_State() const;

private:
	/** 当前帧快照指针（游戏线程写入，worker 线程只读） */
	const FAnimSnapshot* Snap = nullptr;
};
