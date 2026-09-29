/** @file GGYGOComboWindowTest.cpp @brief 连段预输入与窗口边界契约 */
#if WITH_DEV_AUTOMATION_TESTS
#include "AbilitySystem/Abilities/GGYGOComboTypes.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOComboWindowTest, "GGYGO.Combat.Combo.WindowAndBuffer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOComboWindowTest::RunTest(const FString& Parameters)
{
	FGGYGOComboWindowState State;
	TestTrue(TEXT("开窗前可以预输入"), State.Store(1, 10.0, 0.35));
	TestEqual(TEXT("未开窗不得推进"), State.Consume(10.1), 0);
	State.bOpen = true;
	TestEqual(TEXT("开窗立即消费未过期预输入"), State.Consume(10.2), 1);
	TestEqual(TEXT("同一次请求不能推进两段"), State.Consume(10.2), 0);
	State.Reset();
	State.Store(2, 20.0, 0.35);
	State.bOpen = true;
	TestEqual(TEXT("过期预输入不会在开窗后补发"), State.Consume(20.36), 0);
	State.Reset();
	State.Store(3, 30.0, 0.35);
	State.Store(4, 30.1, 0.35);
	State.bOpen = true;
	TestEqual(TEXT("连续按键仅保留最后一个请求"), State.Consume(30.2), 4);
	State.Store(5, 30.2, 0.35);
	State.Close();
	TestEqual(TEXT("关窗丢弃剩余请求"), State.PendingRequestId, 0);
	TestFalse(TEXT("关窗后的按键不能延长旧连段"), State.Store(6, 30.3, 0.35));
	State.bOpen = true;
	TestEqual(TEXT("迟到 Begin 不能绕过已关闭状态"), State.Consume(30.31), 0);
	State.Reset();
	TestTrue(TEXT("新段可重新接收输入"), State.Store(7, 40.0, 0.0));
	State.bOpen = true;
	TestEqual(TEXT("零缓冲仍允许窗口内同一时刻按键"), State.Consume(40.0), 7);
	return true;
}
#endif
