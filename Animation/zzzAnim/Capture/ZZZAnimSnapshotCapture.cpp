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
	// Movement/CMC uses the standard local axes (X=Forward, Y=Right); the existing
	// Pyrios presentation fields retain their established (X=Right, Y=Forward) layout.
	OutSnap.ActualVelocityBlendX = InState.LocalVelocityBlend.Y;
	OutSnap.ActualVelocityBlendY = InState.LocalVelocityBlend.X;
	OutSnap.AnimBlendX = OutSnap.ActualVelocityBlendX;
	OutSnap.AnimBlendY = OutSnap.ActualVelocityBlendY;
	OutSnap.WalkRunBlendAlpha = InState.WalkRunBlendAlpha;
	OutSnap.StopMotionType = InState.StopMotionType;
	OutSnap.TurnBackPhase = InState.TurnBackPhase;
	OutSnap.bTurnBackRunOut = InState.bTurnBackRunOut;

	OutSnap.InputForwardDot = InDebug.InputForwardDot;
	OutSnap.AnimCurveVelocity = InDebug.CurveVelocity;
	OutSnap.AnimCurveVelocityDirection = InDebug.CurveVelocityDirection;
	OutSnap.AnimCurveVelocityAngle = InDebug.CurveVelocityAngle;
}
