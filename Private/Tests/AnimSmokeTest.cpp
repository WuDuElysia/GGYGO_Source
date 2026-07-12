/**
 * @file AnimSmokeTest.cpp
 * @brief 动画决策层测试基础设施冒烟测试
 *
 * 验证 UE Automation 框架（FAutomationTestBase）用例可被发现与运行，
 * 并顺带验证随机生成器工具 FAnimTestGen 可用且输出落在预期定义域内。
 * 本测试是后续属性测试的基线，确认脚手架、包含路径与生成器均正确接入。
 */

#include "Misc/AutomationTest.h"
#include "Math/RandomStream.h"
#include "AnimTestGenerators.h"

#if WITH_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGGYGOAnimSmokeTest,
	"GGYGO.Anim.Smoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAnimSmokeTest::RunTest(const FString& Parameters)
{
	// 1. 平凡断言：确认 FAutomationTestBase 用例可被发现与运行。
	TestTrue(TEXT("Automation 框架可运行冒烟测试"), true);

	// 2. 用固定种子构造随机流，保证可复现。
	FRandomStream Rng(1337);

	// 3. 逐项验证生成器输出落在预期定义域内。
	const float Angle = FAnimTestGen::RandAngleDeg(Rng);
	TestTrue(TEXT("随机角度落在 [-180,180]"), Angle >= -180.f && Angle <= 180.f);

	const float Phase = FAnimTestGen::RandPhase(Rng);
	TestTrue(TEXT("随机相位落在 [0,1)"), Phase >= 0.f && Phase < 1.f);

	// 4. 生成完整快照并断言其相位字段仍在 [0,1)。
	const FAnimSnapshot Snap = FAnimTestGen::RandSnapshot(Rng);
	TestTrue(TEXT("快照相位落在 [0,1)"), Snap.LocomotionPhase >= 0.f && Snap.LocomotionPhase < 1.f);
	TestTrue(TEXT("快照角度落在 [-180,180]"), Snap.MoveAngleDeg >= -180.f && Snap.MoveAngleDeg <= 180.f);

	return true;
}

#endif // WITH_AUTOMATION_TESTS
