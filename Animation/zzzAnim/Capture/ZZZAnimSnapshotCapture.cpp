/**
 * @file ZZZAnimSnapshotCapture.cpp
 * @brief 旧 ZZZ 动画快照适配实现
 */
#include "Animation/zzzAnim/Capture/ZZZAnimSnapshotCapture.h"

#include "Animation/Debug/GGYGOAnimationDebugFrame.h"
#include "Animation/Runtime/GGYGOAnimationStateFrame.h"
#include "Animation/zzzAnim/Data/ZZZAnimSnapshot.h"

void FZZZAnimSnapshotCapture::Capture(
	FZZZAnimSnapshot& OutSnap,
	const FGGYGOAnimationStateFrame& InState,
	const FGGYGOAnimationDebugFrame& InDebug) const
{
	OutSnap = FZZZAnimSnapshot();

	OutSnap.Gait = InState.Gait;
	OutSnap.bShouldMove = InState.bHasMoveInput;
	OutSnap.bBlockMove = InState.bMovementBlocked;
	OutSnap.bGrounded = InState.bGrounded;
	OutSnap.VelocityLength = InState.HorizontalSpeed;
	OutSnap.ActualVelocityDirection = InState.WorldVelocityDirection;
	OutSnap.ActualVelocityAngle = InState.LocalVelocityAngle;
	OutSnap.ActualVelocityBlendX = InState.LocalVelocityBlend.X;
	OutSnap.ActualVelocityBlendY = InState.LocalVelocityBlend.Y;
	OutSnap.AnimBlendX = InState.LocalVelocityBlend.X;
	OutSnap.AnimBlendY = InState.LocalVelocityBlend.Y;
	OutSnap.TurnBackPhase = InState.TurnBackPhase;
	OutSnap.bTurnBackRunOut = InState.bTurnBackRunOut;

	OutSnap.InputForwardDot = InDebug.InputForwardDot;
	OutSnap.AnimCurveVelocity = InDebug.CurveVelocity;
	OutSnap.AnimCurveVelocityDirection = InDebug.CurveVelocityDirection;
	OutSnap.AnimCurveVelocityAngle = InDebug.CurveVelocityAngle;
}
