/** @file GGYGOMontageWindowFXTest.cpp @brief 作者窗声明的准入、失败原子性和显式关闭模式。 */
#include "AbilitySystem/Abilities/GGYGOMontageActionResources.h"
#include "Animation/Notifies/GGYGOAnimNotifyState_GameplayEventWindow.h"
#include "System/GGYGOGameplayTags.h"

#include "Animation/AnimMontage.h"
#include "Misc/AutomationTest.h"
#include "NiagaraSystem.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOMontageFXWindowDeclarationTest,
	"GGYGO.AbilitySystem.MontageWindowFX.Declaration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOMontageFXWindowDeclarationTest::RunTest(const FString& Parameters)
{
	UAnimMontage* Montage = NewObject<UAnimMontage>();
	UNiagaraSystem* System = NewObject<UNiagaraSystem>();
	auto* Notify = NewObject<UGGYGOAnimNotifyState_GameplayEventWindow>(Montage);
	Notify->InitializeEventTags(GGYGOGameplayTags::Event_Montage_FXWindowBegin,
		GGYGOGameplayTags::Event_Montage_FXWindowEnd);
	Notify->NotifyStateBehaviorFlags = static_cast<uint8>(EAnimNotifyStateBehaviorFlags::NoMergeOnConcurrentPlay);
	FAnimNotifyEvent& Event = Montage->Notifies.AddDefaulted_GetRef();
	Event.NotifyStateClass = Notify;
	Event.MontageTickType = EMontageNotifyTickType::Queued;
	Event.Link(Montage, 0.25f);
	Event.SetDuration(0.5f);

	FGGYGOComboStepFX Config;
	Config.Mode = EGGYGOComboFXMode::MontageWindow;
	Config.System = System;
	Config.BeginEventTag = GGYGOGameplayTags::Event_Montage_FXWindowBegin;
	Config.EndEventTag = GGYGOGameplayTags::Event_Montage_FXWindowEnd;
	Config.Space = EGGYGONiagaraEffectSpace::World;
	Config.NormalStopMode = EGGYGONiagaraEffectStopMode::DestroyImmediately;
	Config.AbortStopMode = EGGYGONiagaraEffectStopMode::DestroyImmediately;
	Config.RemoteSeekDeltaSeconds = 0.01f;
	Config.RemoteMaxSimTimeSeconds = 0.002f;
	FGGYGOMontageFXWindowDeclaration Declaration;
	FString Error;
	TestTrue(TEXT("唯一有效作者窗返回声明"),
		FGGYGOMontageActionResources::ValidateFXConfiguration(Montage, Config, 0.0f, 1.0f, Error, &Declaration));
	TestEqual(TEXT("声明起点保留作者坐标"), Declaration.BeginSeconds, 0.25f);
	TestEqual(TEXT("声明终点保留作者时长"), Declaration.EndSeconds, 0.75f);

	Config.RemoteSeekDeltaSeconds = 0.0f;
	TestFalse(TEXT("启用窗口不能借零步长补远端追赶配置"),
		FGGYGOMontageActionResources::ValidateFXConfiguration(Montage, Config, 0.0f, 1.0f, Error, &Declaration));
	TestEqual(TEXT("追赶配置缺失不遗留作者窗"), Declaration.EndSeconds, 0.0f);
	Config.RemoteSeekDeltaSeconds = 0.01f;
	Config.RemoteMaxSimTimeSeconds = std::numeric_limits<float>::infinity();
	TestFalse(TEXT("非有限追赶预算不能作为有效配置"),
		FGGYGOMontageActionResources::ValidateFXConfiguration(Montage, Config, 0.0f, 1.0f, Error, &Declaration));
	TestEqual(TEXT("非法追赶预算清空作者窗"), Declaration.EndSeconds, 0.0f);
	Config.RemoteMaxSimTimeSeconds = 0.002f;

	const FAnimNotifyEvent Duplicate = Montage->Notifies[0];
	Montage->Notifies.Add(Duplicate);
	TestFalse(TEXT("重复作者窗拒绝"),
		FGGYGOMontageActionResources::ValidateFXConfiguration(Montage, Config, 0.0f, 1.0f, Error, &Declaration));
	TestEqual(TEXT("失败不发布已扫描起点"), Declaration.BeginSeconds, 0.0f);
	TestEqual(TEXT("失败不发布已扫描终点"), Declaration.EndSeconds, 0.0f);
	Montage->Notifies.SetNum(1);

	Declaration.BeginSeconds = 10.0f;
	Declaration.EndSeconds = 20.0f;
	TestFalse(TEXT("NaN 绑定范围不能绕过作者窗边界"),
		FGGYGOMontageActionResources::ValidateFXConfiguration(Montage, Config,
			std::numeric_limits<float>::quiet_NaN(), 1.0f, Error, &Declaration));
	TestEqual(TEXT("非法范围清空起点"), Declaration.BeginSeconds, 0.0f);
	TestEqual(TEXT("非法范围清空终点"), Declaration.EndSeconds, 0.0f);

	Notify->NotifyStateBehaviorFlags = 0;
	FGGYGOMontageWindowDeclaration GenericDeclaration;
	TestTrue(TEXT("公共声明保留通用窗口的实际 NoMerge 取值"),
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindowDeclaration(
			Montage, Config.BeginEventTag, Config.EndEventTag, 0.0f, 1.0f, GenericDeclaration, Error));
	TestFalse(TEXT("通用窗口不被强改为不可合并"), GenericDeclaration.bNoMergeOnConcurrentPlay);
	TestFalse(TEXT("可合并作者窗仍拒绝"),
		FGGYGOMontageActionResources::ValidateFXConfiguration(Montage, Config, 0.0f, 1.0f, Error, &Declaration));
	TestEqual(TEXT("非法窗口清空声明"), Declaration.EndSeconds, 0.0f);

	Config.Mode = EGGYGOComboFXMode::Disabled;
	Config.System = nullptr;
	Config.RemoteSeekDeltaSeconds = 0.0f;
	Config.RemoteMaxSimTimeSeconds = 0.0f;
	Declaration.BeginSeconds = 10.0f;
	Declaration.EndSeconds = 20.0f;
	TestTrue(TEXT("显式关闭不要求未使用资产"),
		FGGYGOMontageActionResources::ValidateFXConfiguration(nullptr, Config, 0.0f, 0.0f, Error, &Declaration));
	TestEqual(TEXT("关闭模式不留下旧声明起点"), Declaration.BeginSeconds, 0.0f);
	TestEqual(TEXT("关闭模式不留下旧声明终点"), Declaration.EndSeconds, 0.0f);
	return true;
}
#endif
