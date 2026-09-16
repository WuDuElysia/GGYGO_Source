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
	// 反向输入检测的唯一真相在移动层；此处只读相位：整个 TurnBack 生命周期都允许进入。
	// 用 Phase != None 而不是只等某个具体相位，避免蓝图求值晚于相位推进时错过进入窗口 ——
	// 相位由曲线驱动，一帧内就可能从 Turning 走到 Braking。
	return Context.Snap
		&& Context.Snap->TurnBackPhase != EGGYGOTurnBackPhase::None;
}
