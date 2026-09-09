/**
 * @file InputPipeline.cpp
 * @brief 输入管线实现
 */

#include "Pipeline/InputPipeline.h"

FInputPipeline::FInputPipeline(FInputData& InInputData)
    : InputData(InInputData)
{
}

void FInputPipeline::Process(float DeltaTime)
{

    // 1. 推进双缓冲
    InputData.AdvanceFrame();

    // 2. 获取当前帧引用
    FProcessedInput& Current = InputData.CurrentFrame;

    // 3. 移动防抖处理
    if (!PendingMoveInput.IsNearlyZero())
    {
        // 有输入：更新方向，重置防抖计时器
        Current.Move = PendingMoveInput;
        LastValidMoveDir = PendingMoveInput;
        MoveFlickerTimer = MoveFlickerBuffer;
    }
    else if (MoveFlickerTimer > 0.f)
    {
        // 刚松手：在防抖窗口内保持上一个方向
        MoveFlickerTimer -= DeltaTime;
        Current.Move = LastValidMoveDir;
    }
    else
    {
        // 防抖窗口结束：清零
        Current.Move = FVector2D::ZeroVector;
        LastValidMoveDir = FVector2D::ZeroVector;
    }

    // 4. 视角输入（不需要防抖）
    Current.Look = PendingLookInput;

    // 5. 持续按住状态
    Current.bSprintHeld = bPendingSprint;
	Current.bForceWalkHeld = bPendingForceWalk;
	Current.bAttackHeld = bPendingAttack;
    Current.bDodgeHeld = bPendingDodge;

    // 6. 动作按键缓冲计时器
    // 按下时设为 ActionBufferTime，每帧递减
    if (bPendingAttack)
        Current.AttackBufferTimer = ActionBufferTime;
    else
        Current.AttackBufferTimer = FMath::Max(Current.AttackBufferTimer - DeltaTime, 0.f);

    if (bPendingDodge)
        Current.DodgeBufferTimer = ActionBufferTime;
    else
        Current.DodgeBufferTimer = FMath::Max(Current.DodgeBufferTimer - DeltaTime, 0.f);

    // 7. 清零瞬时输入（下一帧重新从回调写入）
	PendingLookInput = FVector2D::ZeroVector;
	bPendingAttack = false;
	bPendingDodge = false;
	// bPendingSprint/bPendingForceWalk 不清零 — 持续状态由 Completed 清零
}

void FInputPipeline::SetMoveInput(const FVector2D& Value)
{
    PendingMoveInput = Value;
}

void FInputPipeline::ClearMoveInput()
{
    PendingMoveInput = FVector2D::ZeroVector;
}

void FInputPipeline::SetLookInput(const FVector2D& Value)
{
    PendingLookInput = Value;
}

void FInputPipeline::SetAttackPressed()
{
    bPendingAttack = true;
}

void FInputPipeline::SetDodgePressed()
{
    bPendingDodge = true;
}

void FInputPipeline::SetSprintHeld(bool bHeld)
{
    bPendingSprint = bHeld;
}

void FInputPipeline::SetForceWalkHeld(bool bHeld)
{
    bPendingForceWalk = bHeld;
}