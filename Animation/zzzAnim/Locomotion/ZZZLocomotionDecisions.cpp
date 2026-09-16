/**
 * @file ZZZLocomotionDecisions.cpp
 * @brief ZZZ 动画 Locomotion 决策模块实现
 */

#include "Animation/zzzAnim/Locomotion/ZZZLocomotionDecisions.h"
#include "Animation/zzzAnim/Data/ZZZAnimSnapshot.h"
#include "Character/Data/GGYGOMovementTypes.h"
#include "Animation/zzzAnim/Data/ZZZAnimStateMemory.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionRules.h"

void FZZZLocomotionDecisions::SetContext(const FZZZAnimReadContext& InContext)
{
	Context = InContext;
}

bool FZZZLocomotionDecisions::NotMoving_To_Conduit() const
{
	if (!Context.Snap)
	{
		return false;
	}

	// 原本还要求 `CurrentState == Moving`，那个逻辑状态与 bShouldMove 永远同值
	// （见 FZZZAnimSnapshot::bShouldMove 的注释），是冗余条件，随快照重构一并去掉。
	return Context.Snap->bShouldMove;
}

bool FZZZLocomotionDecisions::Stop_To_Conduit() const
{
	// Stop → Conduit 是重新启动移动入口，直接反映移动输入/意图。
	// 不依赖 Gait 解析结果，也不读取速度、GaitBlendY、GaitValue 或 StateMemory。
	return Context.Snap && Context.Snap->bShouldMove;
}

bool FZZZLocomotionDecisions::Conduit_To_EnterMove() const
{
	// 设 P = (Context.Snap != nullptr && Context.Snap->Gait == EGGYGOGait::Run)。
	// 本函数返回 !P，与 Conduit_To_Moving_Direct() 的 P 恰为互补，故同一帧恰有一条为真。
	// Context.Snap 缺失时这里仍返回 true，确定性地落到起步路径。
	// 相对旧的 Context.Memory 前缀判定，这修复了 Memory 缺失时两条判定同时为假的无出口帧。
	return !Context.Snap || Context.Snap->Gait != EGGYGOGait::Run;
}

bool FZZZLocomotionDecisions::Conduit_To_Moving_Direct() const
{
	// 设 P = (Context.Snap != nullptr && Context.Snap->Gait == EGGYGOGait::Run)。
	// 本函数返回 P；Conduit_To_EnterMove() 返回 !P，因此两条判定保持互补。
	return Context.Snap && Context.Snap->Gait == EGGYGOGait::Run;
}

bool FZZZLocomotionDecisions::ShouldStopMoving() const
{
	// EnterMove → Stop 的基础停止判定只由当前帧是否有移动输入决定。
	return Context.Snap && !Context.Snap->bShouldMove;
}

bool FZZZLocomotionDecisions::ShouldExitMoving() const
{
	// Moving → Stop 由无输入触发；转身进行中仍保持 Moving，避免中途松开输入就提前退出。
	// 转身自己的 RunOut 段本来就以松手为出口，那之后相位会变回 None，这里自然放行。
	if (!Context.Snap || Context.Snap->bShouldMove)
	{
		return false;
	}

	if (Context.Snap->TurnBackPhase != EGGYGOTurnBackPhase::None)
	{
		return false;
	}

	return true;
}

bool FZZZLocomotionDecisions::WalkRun_To_TurnBack() const
{
	if (!Context.Snap)
	{
		return false;
	}

	// 只有曲线接管段（Turning / Braking）才进 TurnBack 状态，**不包括 RunOut**。
	//
	// 不能用 `Phase != None`：AnimBP 的 TurnBack → WalkRun 靠动画播完判定
	// （`GetRelevantAnimTimeRemainingFraction <= 0.03`），而玩家在 RunOut 段持续按着输入时
	// 相位会一直停在 RunOut。那样动画播完切到 WalkRun 后，下一帧本判定又成立，
	// 立刻退回 TurnBack 从头播 —— 转身动画会无限循环直到相位超时。
	//
	// 排除 RunOut 在表现上也是对的：那一段的移动方向已经交回输入，与普通走跑无异，
	// 曲线给出的速度也回升到了 Run_Loop 的量级，播通用跑步循环比播转身动画的后半段
	// 更贴合「玩家可能已经把方向打到别处」这个事实。
	const EGGYGOTurnBackPhase Phase = Context.Snap->TurnBackPhase;
	return Phase == EGGYGOTurnBackPhase::Turning
		|| Phase == EGGYGOTurnBackPhase::Braking;
}
