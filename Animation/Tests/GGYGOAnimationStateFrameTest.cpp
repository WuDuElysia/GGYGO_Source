/**
 * @file GGYGOAnimationStateFrameTest.cpp
 * @brief AnimationStateFrame 兼容映射的自动化测试
 */
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Animation/Debug/GGYGOAnimationDebugFrame.h"
#include "Animation/Runtime/GGYGOAnimationStateFrame.h"
#include "Animation/zzzAnim/Capture/ZZZAnimSnapshotCapture.h"
#include "Animation/zzzAnim/Data/ZZZAnimSnapshot.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGGYGOAnimationStateFrameCompatibilityTest,
	"GGYGO.Animation.StateFrame.CompatibilityAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAnimationStateFrameCompatibilityTest::RunTest(const FString& Parameters)
{
	FGGYGOAnimationStateFrame State;
	State.HorizontalSpeed = 321.0f;
	State.WorldVelocityDirection = FVector(0.0, 1.0, 0.0);
	State.LocalVelocityAngle = 90.0f;
	State.LocalVelocityBlend = FVector2D(1.0, 0.25);
	State.Gait = EGGYGOGait::Run;
	State.TurnBackPhase = EGGYGOTurnBackPhase::RunOut;
	State.bHasMoveInput = true;
	State.bGrounded = false;
	State.bMovementBlocked = true;
	State.bTurnBackRunOut = true;

	FGGYGOAnimationDebugFrame Debug;
	Debug.CurveVelocity = FVector(100.0, 20.0, 0.0);
	Debug.CurveVelocityDirection = FVector(1.0, 0.0, 0.0);
	Debug.CurveVelocityAngle = 12.5f;
	Debug.InputForwardDot = -0.75f;

	FZZZAnimSnapshot Snapshot;
	FZZZAnimSnapshotCapture Adapter;
	Adapter.Capture(Snapshot, State, Debug);

	TestEqual(TEXT("步态"), Snapshot.Gait, State.Gait);
	TestEqual(TEXT("移动输入"), Snapshot.bShouldMove, State.bHasMoveInput);
	TestEqual(TEXT("移动禁止"), Snapshot.bBlockMove, State.bMovementBlocked);
	TestEqual(TEXT("地面状态"), Snapshot.bGrounded, State.bGrounded);
	TestEqual(TEXT("水平速度"), Snapshot.VelocityLength, State.HorizontalSpeed);
	TestEqual(TEXT("世界速度方向"), Snapshot.ActualVelocityDirection, State.WorldVelocityDirection);
	TestEqual(TEXT("局部速度角"), Snapshot.ActualVelocityAngle, State.LocalVelocityAngle);
	TestEqual(TEXT("局部混合 X"), Snapshot.ActualVelocityBlendX, static_cast<float>(State.LocalVelocityBlend.X));
	TestEqual(TEXT("局部混合 Y"), Snapshot.ActualVelocityBlendY, static_cast<float>(State.LocalVelocityBlend.Y));
	TestEqual(TEXT("旧混合 X"), Snapshot.AnimBlendX, static_cast<float>(State.LocalVelocityBlend.X));
	TestEqual(TEXT("旧混合 Y"), Snapshot.AnimBlendY, static_cast<float>(State.LocalVelocityBlend.Y));
	TestEqual(TEXT("转身阶段"), Snapshot.TurnBackPhase, State.TurnBackPhase);
	TestEqual(TEXT("转身 RunOut"), Snapshot.bTurnBackRunOut, State.bTurnBackRunOut);
	TestEqual(TEXT("输入点积"), Snapshot.InputForwardDot, Debug.InputForwardDot);
	TestEqual(TEXT("曲线速度"), Snapshot.AnimCurveVelocity, Debug.CurveVelocity);
	TestEqual(TEXT("曲线方向"), Snapshot.AnimCurveVelocityDirection, Debug.CurveVelocityDirection);
	TestEqual(TEXT("曲线角"), Snapshot.AnimCurveVelocityAngle, Debug.CurveVelocityAngle);

	return true;
}

#endif
