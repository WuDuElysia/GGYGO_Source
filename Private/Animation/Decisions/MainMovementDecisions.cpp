/**
 * @file MainMovementDecisions.cpp
 * @brief Layer 1 MainMovement 决策模块实现
 */
#include "Animation/Decisions/MainMovementDecisions.h"
#include "Animation/GGYGOAnimInstance.h"  // FAnimSnapshot 完整定义

// ----------------------------------------------------------------------------

void FMainMovementDecisions::SetSnap(const FAnimSnapshot* InSnap)
{
	Snap = InSnap;
}

// ----------------------------------------------------------------------------
// Layer 1 Decision Functions
// ----------------------------------------------------------------------------

bool FMainMovementDecisions::Main_To_Fall() const
{
	if (!Snap) return false;
	return !Snap->bGrounded && Snap->VerticalVelocity < 0.f && !Snap->bWantGlide;
}

bool FMainMovementDecisions::Main_To_Jump() const
{
	if (!Snap) return false;
	return Snap->bWantJump;
}

bool FMainMovementDecisions::Main_To_Grounded() const
{
	if (!Snap) return false;
	return Snap->bGrounded;
}

bool FMainMovementDecisions::Main_To_Gliding() const
{
	if (!Snap) return false;
	return !Snap->bGrounded && Snap->bWantGlide;
}

bool FMainMovementDecisions::Main_To_Swimming() const
{
	if (!Snap) return false;
	return Snap->bInWater;
}
// ----------------------------------------------------------------------------
// 顶层决策补全（#345–#378，共 33 个）
// 判定语义严格对齐 NTE_05 附录 B.2.7 明细表。
// ----------------------------------------------------------------------------

// ---- Grounded 子机过渡（#345–#349）----

bool FMainMovementDecisions::MainMove_Grounded_To_MovementState() const
{
	if (!Snap) return false;
	return Snap->bWantsToLeaveGround;  // #345
}

bool FMainMovementDecisions::MainMove_Grounded_To_JumpTakeOff() const
{
	if (!Snap) return false;
	return Snap->bWantJump && Snap->bUseJumpTakeOff;  // #346
}

bool FMainMovementDecisions::MainMove_Grounded_To_Jump() const
{
	if (!Snap) return false;
	return Snap->bWantJump && !Snap->bUseJumpTakeOff;  // #347
}

bool FMainMovementDecisions::MainMove_Grounded_To_Jump_Instant() const
{
	if (!Snap) return false;
	return Snap->bSecondJump || Snap->bForceJump;  // #349
}

// ---- Fall 子机过渡（#350–#352）----

bool FMainMovementDecisions::MainMove_Fall_To_InAir() const
{
	if (!Snap) return false;
	return Snap->bWantsAirAction;  // #350
}

bool FMainMovementDecisions::MainMove_Fall_To_Land() const
{
	if (!Snap) return false;
	return Snap->bIsLanding;  // #351
}

bool FMainMovementDecisions::MainMove_Fall_To_Jump() const
{
	if (!Snap) return false;
	// ⚠️ 待验证（NTE_05 B.2.7 #352）：provisional bCoyoteTimeJump，
	// 语义由 JSON 推导，需蓝图侧验证后确认。字段默认 false 安全降级。
	return Snap->bCoyoteTimeJump;  // #352
}

// ---- Jump 子机过渡（#353–#354）----

bool FMainMovementDecisions::MainMove_Jump_To_InAir() const
{
	if (!Snap) return false;
	return Snap->bJumpApexReached;  // #353
}

bool FMainMovementDecisions::MainMove_Jump_To_Land() const
{
	if (!Snap) return false;
	return Snap->bIsLanding;  // #354
}

// ---- MovementState (<-MS->) 分派（#355–#360）----

bool FMainMovementDecisions::MainMove_MS_To_InAir() const
{
	if (!Snap) return false;
	// ⚠️ 待验证（NTE_05 B.2.7 #355）：provisional bEnteredFromAir，
	// 语义由 JSON 推导，需蓝图侧验证后确认。字段默认 false 安全降级。
	return Snap->bEnteredFromAir;  // #355
}

bool FMainMovementDecisions::MainMove_MS_To_Vines() const
{
	if (!Snap) return false;
	return Snap->bOnVines;  // #356
}

bool FMainMovementDecisions::MainMove_MS_To_VaultToAir() const
{
	if (!Snap) return false;
	return Snap->bVaultTriggered;  // #357
}

bool FMainMovementDecisions::MainMove_MS_To_Swimming() const
{
	if (!Snap) return false;
	return Snap->bInWater;  // #358
}

bool FMainMovementDecisions::MainMove_MS_To_Driving() const
{
	if (!Snap) return false;
	return Snap->bStartedDriving;  // #359
}

bool FMainMovementDecisions::MainMove_MS_To_Grounded() const
{
	if (!Snap) return false;
	return Snap->bReturnToGround;  // #360
}

// ---- InAir 导管（#361–#363）----

bool FMainMovementDecisions::MainMove_InAir_To_Jump() const
{
	if (!Snap) return false;
	return Snap->bCanDoubleJump;  // #361
}

bool FMainMovementDecisions::MainMove_InAir_To_MS() const
{
	if (!Snap) return false;
	return Snap->bSpecialModeInAir;  // #362
}

bool FMainMovementDecisions::MainMove_InAir_To_Fall() const
{
	if (!Snap) return false;
	return true;  // #363 fallback（唯一出边）
}

// ---- Land 导管（#364）----

bool FMainMovementDecisions::MainMove_Land_To_Grounded() const
{
	if (!Snap) return false;
	return true;  // #364 唯一出边
}

// ---- TakeOff 子机（#365–#366）----

bool FMainMovementDecisions::MainMove_TakeOff_To_Jump() const
{
	if (!Snap) return false;
	// ⚠️ 待验证（NTE_05 B.2.7 #365）：provisional bTakeOffComplete，
	// 语义由 JSON 推导，需蓝图侧验证后确认。字段默认 false 安全降级。
	return Snap->bTakeOffComplete;  // #365
}

bool FMainMovementDecisions::MainMove_TakeOff_To_MS() const
{
	if (!Snap) return false;
	// ⚠️ 待验证（NTE_05 B.2.7 #366）：provisional bCancelJump，
	// 语义由 JSON 推导，需蓝图侧验证后确认。字段默认 false 安全降级。
	return Snap->bCancelJump;  // #366
}

// ---- Vines 子机（#367–#368）----

bool FMainMovementDecisions::MainMove_Vines_To_MS() const
{
	if (!Snap) return false;
	return Snap->bLeaveVines;  // #367
}

bool FMainMovementDecisions::MainMove_Vines_To_MS_Auto() const
{
	if (!Snap) return false;
	return Snap->bVinesAnimComplete;  // #368 AutoRule + 附加条件
}

// ---- Vault 子机（#369–#370）----

bool FMainMovementDecisions::MainMove_Vault_To_MS() const
{
	if (!Snap) return false;
	return Snap->bVaultExitCondition;  // #369
}

bool FMainMovementDecisions::MainMove_Vault_To_MS_Auto() const
{
	if (!Snap) return false;
	return true;  // #370 AutoRule + true
}

// ---- Gliding / GlidingToFall（#371–#375）----

bool FMainMovementDecisions::MainMove_Gliding_To_GlidingToFall() const
{
	if (!Snap) return false;
	return Snap->bStopGliding && !Snap->bLanding;  // #371
}

bool FMainMovementDecisions::MainMove_Gliding_To_OutGliding() const
{
	if (!Snap) return false;
	return Snap->bLanding || Snap->bForceExitGliding;  // #372
}

bool FMainMovementDecisions::MainMove_GlidingToFall_To_Fall() const
{
	if (!Snap) return false;
	// ⚠️ 待验证（NTE_05 B.2.7 #373）：provisional bTransitionComplete，
	// 语义由 JSON 推导，需蓝图侧验证后确认。字段默认 false 安全降级。
	return Snap->bTransitionComplete;  // #373
}

bool FMainMovementDecisions::MainMove_GlidingToFall_To_Gliding() const
{
	if (!Snap) return false;
	return Snap->bResumeGliding;  // #374
}

bool FMainMovementDecisions::MainMove_GlidingToFall_To_OutGliding() const
{
	if (!Snap) return false;
	return Snap->bLanding;  // #375
}

// ---- Swimming / Driving（#376–#377）----

bool FMainMovementDecisions::MainMove_Swimming_To_MS() const
{
	if (!Snap) return false;
	return Snap->bLeaveWater;  // #376
}

bool FMainMovementDecisions::MainMove_Driving_To_MS() const
{
	if (!Snap) return false;
	return Snap->bStopDriving;  // #377
}

// ---- OutGliding 导管（#378）----

bool FMainMovementDecisions::MainMove_OutGliding_To_MS() const
{
	if (!Snap) return false;
	return true;  // #378 唯一出边
}
