/**
 * @file CyclesDecisions.h
 * @brief Layer 5 LocomotionCycles 决策模块
 *
 * 封装循环层级的过渡判定函数（LocomotionCycles + MoveR + MoveL）。
 * 纯 C++ 类，依赖 FAnimSnapshot 只读指针和 FLocomotionTuning 配置，不依赖 UObject 运行时。
 *
 * 包含 18 个决策函数：
 *   8 个 Cycles_*  — 左右脚循环入口 + StopRotation 触发/结束 + Conduit 脚分发
 *   5 个 MoveR_*   — 右脚循环内 Sprint/RunWalk 切换
 *   5 个 MoveL_*   — 左脚循环内 Sprint/RunWalk 切换（与 MoveR 对称）
 */
#pragma once

#include "CoreMinimal.h"

// 前向声明（编译隔离：头文件不需要完整定义）
struct FAnimSnapshot;
struct FLocomotionTuning;

/**
 * Layer 5 LocomotionCycles 决策类
 *
 * 负责循环层状态机的过渡判定：左右脚循环入口、停止旋转触发/结束、
 * MoveR/MoveL 内部 Sprint 与 RunWalk 之间的步态切换。
 *
 * 所有决策函数为 const，只读 Snap，线程安全。
 */
class FCyclesDecisions
{
public:
	/**
	 * 初始化 Tuning 配置指针
	 * @param InTuning 配置指针；若为 nullptr 则降级到 DefaultTuning
	 */
	void Init(const FLocomotionTuning* InTuning);

	/** 每帧由 AnimInstance 调用，设置快照指针 */
	void SetSnap(const FAnimSnapshot* InSnap);

	// ---- LocomotionCycles（8 个） ----

	/** 入口→左脚循环：当前支撑脚为左脚 */
	bool Cycles_To_Left() const;

	/** 入口→右脚循环：当前支撑脚为右脚 */
	bool Cycles_To_Right() const;

	/** 循环→停止旋转触发（Run 步态）：无移动意图 + 角度超阈值 + Run */
	bool Cycles_To_RunStopRotation() const;

	/** 循环→停止旋转触发（通用）：无移动意图 + 角度超阈值 */
	bool Cycles_To_StopRotation() const;

	/** 循环→停止旋转触发（Walk 步态）：无移动意图 + 角度超阈值 + Walk */
	bool Cycles_To_WalkStopRotation() const;

	/** StopRotation 结束→回到循环：有移动意图或角度回归正常 */
	bool Cycles_StopRotation_Done() const;

	/** Conduit 按脚分发→左脚 */
	bool Cycles_Conduit_IsLeft() const;

	/** Conduit 按脚分发→右脚 */
	bool Cycles_Conduit_IsRight() const;

	// ---- MoveR（5 个，右脚循环内 Sprint/RunWalk 切换） ----

	/** MoveR Conduit → RunWalk：非冲刺步态进入走跑循环 */
	bool MoveR_Conduit_To_RunWalk() const;

	/** MoveR Conduit → SprintToRunWalk：从冲刺减速进入走跑 */
	bool MoveR_Conduit_To_SprintToRunWalk() const;

	/** MoveR RunWalk → Sprint：步态升级到冲刺 */
	bool MoveR_RunWalk_To_Sprint() const;

	/** MoveR Sprint → RunWalk：步态从冲刺降回走跑 */
	bool MoveR_Sprint_To_RunWalk() const;

	/** MoveR SprintToRunWalk → RunWalk：减速过渡完成（AutoRule） */
	bool MoveR_SprintToRunWalk_To_RunWalk() const;

	// ---- MoveL（5 个，左脚循环内 Sprint/RunWalk 切换，与 MoveR 对称） ----

	/** MoveL Conduit → RunWalk：非冲刺步态进入走跑循环 */
	bool MoveL_Conduit_To_RunWalk() const;

	/** MoveL Conduit → SprintToRunWalk：从冲刺减速进入走跑 */
	bool MoveL_Conduit_To_SprintToRunWalk() const;

	/** MoveL RunWalk → Sprint：步态升级到冲刺 */
	bool MoveL_RunWalk_To_Sprint() const;

	/** MoveL Sprint → RunWalk：步态从冲刺降回走跑 */
	bool MoveL_Sprint_To_RunWalk() const;

	/** MoveL SprintToRunWalk → RunWalk：减速过渡完成（AutoRule） */
	bool MoveL_SprintToRunWalk_To_RunWalk() const;

private:
	/** 当前帧快照指针（游戏线程写入，worker 线程只读） */
	const FAnimSnapshot* Snap = nullptr;

	/** 移动参数配置（Init 时绑定，生命周期由 AnimInstance 保证） */
	const FLocomotionTuning* Tuning = nullptr;

	/** 降级默认配置（Tuning 传入 nullptr 时使用） */
	static const FLocomotionTuning DefaultTuning;
};
