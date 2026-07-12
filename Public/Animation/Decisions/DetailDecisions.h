/**
 * @file DetailDecisions.h
 * @brief Layer 4.1 + 4.2 Detail 步态与转身决策模块
 *
 * 封装 Moving 子机内部的步态切换（Walk/Run/Sprint）与转身（TurnBack）判定函数。
 * 纯 C++ 类，依赖 FAnimSnapshot 只读指针和 FLocomotionTuning 配置，不依赖 UObject 运行时。
 */
#pragma once

#include "CoreMinimal.h"

// 前向声明（编译隔离：头文件不需要完整定义）
struct FAnimSnapshot;
struct FLocomotionTuning;

/**
 * Layer 4.1 + 4.2 Detail 决策类
 *
 * 负责 Moving 子机内部的过渡判定：
 *   Layer 4.1 — 步态切换（Walk ↔ Run）与转身触发/恢复
 *   Layer 4.2 — RunTurnBackState 脚分派 + Sprint 进出
 *
 * 所有决策函数为 const，只读 Snap，线程安全。
 */
class FDetailDecisions
{
public:
	/**
	 * 初始化 Tuning 配置指针
	 * @param InTuning 配置指针；若为 nullptr 则降级到 DefaultTuning
	 */
	void Init(const FLocomotionTuning* InTuning);

	/** 每帧由 AnimInstance 调用，设置快照指针 */
	void SetSnap(const FAnimSnapshot* InSnap);

	// ---- Layer 4.1 Detail 步态 ----

	/** EnterRunState → Run：进入后立即过渡（无条件 true） */
	bool Detail_EnterRun_To_Run() const;

	/** Walk → Run：解析步态达到 Run 或更高 */
	bool Detail_Walk_To_Run() const;

	/** Walk → WalkToRun：步态升级需要过渡动画 */
	bool Detail_Walk_To_WalkToRun() const;

	/** Run → TurnBack：移动角绝对值大于 Tuning.TurnBackAngle */
	bool Detail_Run_To_TurnBack() const;

	/** WalkToRun → Run（打断）：过渡中步态降回 Walk */
	bool Detail_WalkToRun_To_Run() const;

	/** TurnBack → Run：转身结束，角度回归正常 */
	bool Detail_TurnBack_To_Run() const;

	/** Run → Walk：解析步态等于 Walk */
	bool Detail_Run_To_Walk() const;

	// ---- Layer 4.2 RunTurnBackState ----

	/** RunTurnBackState Conduit → Left：当前支撑脚为左脚 */
	bool Detail_TurnBack_IsLeft() const;

	/** RunTurnBackState Conduit → Right：当前支撑脚为右脚 */
	bool Detail_TurnBack_IsRight() const;

	// ---- Layer 4.2 Sprint ----

	/** 进入冲刺：解析步态等于 Sprint */
	bool Gait_To_Sprint() const;

	/** 退出冲刺：解析步态低于 Sprint */
	bool Gait_Exit_Sprint() const;

private:
	/** 当前帧快照指针（游戏线程写入，worker 线程只读） */
	const FAnimSnapshot* Snap = nullptr;

	/** 移动参数配置（Init 时绑定，生命周期由 AnimInstance 保证） */
	const FLocomotionTuning* Tuning = nullptr;

	/** 降级默认配置（Tuning 传入 nullptr 时使用） */
	static const FLocomotionTuning DefaultTuning;
};
