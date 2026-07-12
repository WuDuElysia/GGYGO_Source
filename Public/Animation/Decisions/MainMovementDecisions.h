/**
 * @file MainMovementDecisions.h
 * @brief Layer 1 MainMovement 决策模块
 *
 * 封装顶层运动模式路由的 5 个过渡判定函数（Fall/Jump/Grounded/Gliding/Swimming）。
 * 纯 C++ 类，仅依赖 FAnimSnapshot 只读指针，不依赖 UObject 运行时。
 */
#pragma once

#include "CoreMinimal.h"

// 前向声明（编译隔离：头文件不需要完整定义）
struct FAnimSnapshot;

/**
 * Layer 1 MainMovement 决策类
 *
 * 负责顶层状态机的 5 条过渡判定：
 *   Grounded → Fall / Jump / Gliding / Swimming
 *   Fall/Jump/Gliding/Swimming → Grounded
 *
 * 所有决策函数为 const，只读 Snap，线程安全。
 */
class FMainMovementDecisions
{
public:
	/** 每帧由 AnimInstance 调用，设置快照指针 */
	void SetSnap(const FAnimSnapshot* InSnap);

	/** 切换到下落：不在地面且垂直速度小于 0 且无滑翔意图 */
	bool Main_To_Fall() const;

	/** 切换到跳跃：有跳跃意图 */
	bool Main_To_Jump() const;

	/** 切换到地面：在地面 */
	bool Main_To_Grounded() const;

	/** 切换到滑翔：不在地面且有滑翔意图 */
	bool Main_To_Gliding() const;

	/** 切换到游泳：处于水中 */
	bool Main_To_Swimming() const;

	// ---- Grounded 子机过渡（#345–#349）----
	/** #345 离开地面意图：bWantsToLeaveGround */
	bool MainMove_Grounded_To_MovementState() const;
	/** #346 起跳（带 TakeOff）：bWantJump && bUseJumpTakeOff */
	bool MainMove_Grounded_To_JumpTakeOff() const;
	/** #347 直接跳跃：bWantJump && !bUseJumpTakeOff */
	bool MainMove_Grounded_To_Jump() const;
	/** #349 即时跳跃：bSecondJump || bForceJump */
	bool MainMove_Grounded_To_Jump_Instant() const;

	// ---- Fall 子机过渡（#350–#352）----
	/** #350 进入空中动作：bWantsAirAction */
	bool MainMove_Fall_To_InAir() const;
	/** #351 落地：bIsLanding */
	bool MainMove_Fall_To_Land() const;
	/** #352 ⚠️ 待验证：土狼时间跳跃（provisional bCoyoteTimeJump），待蓝图验证 */
	bool MainMove_Fall_To_Jump() const;

	// ---- Jump 子机过渡（#353–#354）----
	/** #353 到达跳跃顶点：bJumpApexReached */
	bool MainMove_Jump_To_InAir() const;
	/** #354 落地：bIsLanding */
	bool MainMove_Jump_To_Land() const;

	// ---- MovementState (<-MS->) 分派（#355–#360）----
	/** #355 ⚠️ 待验证：从空中进入（provisional bEnteredFromAir），待蓝图验证 */
	bool MainMove_MS_To_InAir() const;
	/** #356 进入藤蔓：bOnVines */
	bool MainMove_MS_To_Vines() const;
	/** #357 翻越进入空中：bVaultTriggered */
	bool MainMove_MS_To_VaultToAir() const;
	/** #358 进入游泳：bInWater */
	bool MainMove_MS_To_Swimming() const;
	/** #359 进入驾驶：bStartedDriving */
	bool MainMove_MS_To_Driving() const;
	/** #360 返回地面：bReturnToGround */
	bool MainMove_MS_To_Grounded() const;

	// ---- InAir 导管（#361–#363）----
	/** #361 二段跳：bCanDoubleJump */
	bool MainMove_InAir_To_Jump() const;
	/** #362 特殊空中模式：bSpecialModeInAir */
	bool MainMove_InAir_To_MS() const;
	/** #363 fallback（唯一出边）：true */
	bool MainMove_InAir_To_Fall() const;

	// ---- Land 导管（#364）----
	/** #364 唯一出边：true */
	bool MainMove_Land_To_Grounded() const;

	// ---- TakeOff 子机（#365–#366）----
	/** #365 ⚠️ 待验证：起跳完成（provisional bTakeOffComplete），待蓝图验证 */
	bool MainMove_TakeOff_To_Jump() const;
	/** #366 ⚠️ 待验证：取消跳跃（provisional bCancelJump），待蓝图验证 */
	bool MainMove_TakeOff_To_MS() const;

	// ---- Vines 子机（#367–#368）----
	/** #367 离开藤蔓：bLeaveVines */
	bool MainMove_Vines_To_MS() const;
	/** #368 AutoRule 组合：bVinesAnimComplete */
	bool MainMove_Vines_To_MS_Auto() const;

	// ---- Vault 子机（#369–#370）----
	/** #369 翻越退出条件：bVaultExitCondition */
	bool MainMove_Vault_To_MS() const;
	/** #370 AutoRule 组合：true */
	bool MainMove_Vault_To_MS_Auto() const;

	// ---- Gliding / GlidingToFall（#371–#375）----
	/** #371 滑翔转下落：bStopGliding && !bLanding */
	bool MainMove_Gliding_To_GlidingToFall() const;
	/** #372 滑翔转退出：bLanding || bForceExitGliding */
	bool MainMove_Gliding_To_OutGliding() const;
	/** #373 ⚠️ 待验证：过渡完成（provisional bTransitionComplete），待蓝图验证 */
	bool MainMove_GlidingToFall_To_Fall() const;
	/** #374 恢复滑翔：bResumeGliding */
	bool MainMove_GlidingToFall_To_Gliding() const;
	/** #375 落地：bLanding */
	bool MainMove_GlidingToFall_To_OutGliding() const;

	// ---- Swimming / Driving（#376–#377）----
	/** #376 离开水域：bLeaveWater */
	bool MainMove_Swimming_To_MS() const;
	/** #377 停止驾驶：bStopDriving */
	bool MainMove_Driving_To_MS() const;

	// ---- OutGliding 导管（#378）----
	/** #378 唯一出边：true */
	bool MainMove_OutGliding_To_MS() const;

private:
	/** 当前帧快照指针（游戏线程写入，worker 线程只读） */
	const FAnimSnapshot* Snap = nullptr;
};
