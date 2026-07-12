/**
 * @file LocomotionDecisions.h
 * @brief Layer 3 Locomotion 决策模块
 *
 * 封装 Layer 3 LocomotionStates（7 个 Loco_* 函数）和
 * LocomotionStatesMachine（43 个 Loco3_* 函数）的全部过渡判定。
 *
 * 纯 C++ 类，依赖 FAnimSnapshot 只读指针、FLocomotionTuning 配置、
 * 以及 FCurveValueDelegate / FStateWeightDelegate 代理。不依赖 UObject 运行时。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/Decisions/DecisionTypes.h"

// 前向声明（编译隔离：头文件不需要完整定义）
struct FAnimSnapshot;
struct FLocomotionTuning;

/**
 * Layer 3 Locomotion 决策类
 *
 * 负责 LocomotionStates（待机/进入/移动/停止）和
 * LocomotionStatesMachine（NTE Layer 3 全拓扑，43 个过渡条件）的判定。
 *
 * 所有决策函数为 const，只读 Snap，线程安全。
 * 依赖动画曲线和状态权重时通过注入的代理间接查询。
 */
class FLocomotionDecisions
{
public:
	/**
	 * 初始化配置和代理
	 * @param InTuning 配置指针；若为 nullptr 则降级到 DefaultTuning
	 * @param InCurveDelegate 曲线值查询代理
	 * @param InWeightDelegate 状态权重查询代理
	 */
	void Init(const FLocomotionTuning* InTuning, FCurveValueDelegate InCurveDelegate, FStateWeightDelegate InWeightDelegate);

	/** 每帧由 AnimInstance 调用，设置快照指针 */
	void SetSnap(const FAnimSnapshot* InSnap);

	// ================================================================
	// Layer 3 LocomotionStates（7 个决策函数）
	// ================================================================

	/** 待机 → 进入：有移动意图且在地面 */
	bool Loco_NotMoving_To_Enter() const;

	/** 进入 → 移动：Enter 动画播完 */
	bool Loco_Enter_To_Moving() const;

	/** 进入 → 待机：无移动意图 */
	bool Loco_Enter_To_NotMoving() const;

	/** 移动 → 左脚停：无移动意图且速度低于阈值且左脚支撑 */
	bool Loco_Moving_To_LeftStop() const;

	/** 移动 → 右脚停：无移动意图且速度低于阈值且右脚支撑 */
	bool Loco_Moving_To_RightStop() const;

	/** 停止 → 移动：重新有移动意图 */
	bool Loco_Stop_To_Moving() const;

	/** 停止 → 待机：Stop 动画播完 */
	bool Loco_Stop_To_NotMoving() const;

	// ================================================================
	// Layer 3 LocomotionStatesMachine（43 个决策函数）
	// ================================================================

	// ---- Conduit entry (#662-#664) ----
	bool Loco3_Conduit_To_Moving_Sprint() const;
	bool Loco3_Conduit_To_Moving() const;
	bool Loco3_Conduit_To_NotMoving() const;

	// ---- NotMoving transitions (#665-#668) ----
	bool Loco3_NotMoving_To_Moving_Auto() const;
	bool Loco3_NotMoving_To_Conduit5() const;
	bool Loco3_NotMoving_To_Moving_Alt() const;
	bool Loco3_NotMoving_To_Stop_MM() const;

	// ---- Moving transitions (#669-#671) ----
	bool Loco3_Moving_To_NotMoving_Auto() const;
	bool Loco3_Moving_To_NotMoving() const;
	bool Loco3_Moving_To_Conduit1() const;

	// ---- Stop transitions (#672-#674) ----
	bool Loco3_Stop_To_NotMoving_Auto1() const;
	bool Loco3_Stop_To_NotMoving_Auto2() const;
	bool Loco3_Stop_To_Conduit4() const;

	// ---- Patrol cycle (#675-#681) ----
	bool Loco3_NotMoving1_To_Moving1_Patrol() const;
	bool Loco3_NotMoving1_To_LeftStop1() const;
	bool Loco3_NotMoving1_To_RightStop1() const;
	bool Loco3_NotMoving1_To_Moving1_Should() const;
	bool Loco3_Moving1_To_NotMoving1() const;
	bool Loco3_Moving1_To_CanStop1() const;
	bool Loco3_AutoRule_Fallback() const;

	// ---- Conduit_2 (#682-#684) ----
	bool Loco3_Conduit2_To_Moving1_Sprint() const;
	bool Loco3_Conduit2_To_Moving1() const;
	bool Loco3_Conduit2_To_NotMoving1() const;

	// ---- Stop_1 (#685-#687) ----
	bool Loco3_LeftStop1_To_NotMoving1_Auto() const;
	bool Loco3_Stop1_To_Moving1_Resume() const;
	bool Loco3_RightStop1_To_NotMoving1_Auto() const;

	// ---- CanStop_1 (#688-#689) ----
	bool Loco3_CanStop1_To_Conduit1_1_1() const;
	bool Loco3_CanStop1_To_Conduit1_2() const;

	// ---- StopConduit (#690-#691) ----
	bool Loco3_StopConduit_NoSprint() const;
	bool Loco3_StopConduit_Sprint() const;

	// ---- Stop route (#692-#693) ----
	bool Loco3_Conduit1_To_Stop_A() const;
	bool Loco3_Conduit1_To_Stop_B() const;

	// ---- EnterMoveState (#694-#695) ----
	bool Loco3_EnterMove_To_Moving() const;
	bool Loco3_EnterMove_To_Conduit1() const;

	// ---- Conduit_3 (#696-#697) ----
	bool Loco3_Conduit3_To_Moving() const;
	bool Loco3_Conduit3_To_EnterState() const;

	// ---- EnterState (#698) ----
	bool Loco3_EnterState_To_EnterMove() const;

	// ---- Conduit_5 (#699-#701) ----
	bool Loco3_Conduit5_To_Conduit3() const;
	bool Loco3_Conduit5_To_Moving_Walk() const;
	bool Loco3_Conduit5_To_EnterWalk() const;

	// ---- EnterWalk (#702) ----
	bool Loco3_EnterWalk_To_Moving() const;

	// ---- Conduit_4 (#703-#704) ----
	bool Loco3_Conduit4_To_EnterState() const;
	bool Loco3_Conduit4_To_Moving() const;

private:
	/**
	 * 安全获取动画曲线值
	 * @param CurveName 曲线名
	 * @param OutValue 输出值（delegate 为空时置 0.f）
	 * @return 是否成功获取
	 */
	bool GetCurveValueSafe(FName CurveName, float& OutValue) const;

	/**
	 * 安全获取状态机权重
	 * @param MachineIndex 状态机索引
	 * @param StateIndex 状态索引
	 * @return 权重值（delegate 为空时返回 0.f）
	 */
	float GetStateWeightSafe(int32 MachineIndex, int32 StateIndex) const;

	/** 当前帧快照指针（游戏线程写入，worker 线程只读） */
	const FAnimSnapshot* Snap = nullptr;

	/** 移动参数配置（Init 时绑定，生命周期由 AnimInstance 保证） */
	const FLocomotionTuning* Tuning = nullptr;

	/** 曲线值查询代理 */
	FCurveValueDelegate CurveDelegate;

	/** 状态权重查询代理 */
	FStateWeightDelegate WeightDelegate;

	/** 降级默认配置（Tuning 传入 nullptr 时使用） */
	static const FLocomotionTuning DefaultTuning;
};
