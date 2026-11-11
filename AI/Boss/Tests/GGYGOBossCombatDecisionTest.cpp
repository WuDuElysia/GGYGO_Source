/** @file GGYGOBossCombatDecisionTest.cpp @brief 配置校验与阶段权威接口，不代替蓝图业务验收 */
#include "AI/Boss/GGYGOBossDefinition.h"

#include "AI/Boss/GGYGOBossState.h"
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "BehaviorTree/BehaviorTree.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "NativeGameplayTags.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_BossDecisionTestPhaseTwo, "State.Boss.Phase.DecisionTestTwo");

namespace
{
	void ConfigureTwoPhases(UGGYGOBossDefinition* Definition)
	{
		Definition->InitialFormTag = GGYGOGameplayTags::State_Boss_Form_Default;
		Definition->InitialPhaseTag = GGYGOGameplayTags::State_Boss_Phase_One;
		FGGYGOBossFormDefinition Form;
		Form.FormTag = Definition->InitialFormTag;
		Form.AvatarPawnData = NewObject<UGGYGOPawnData>(Definition);
		Definition->Forms.Add(Form);
		FGGYGOBossPhaseDefinition First;
		First.PhaseTag = Definition->InitialPhaseTag;
		FGGYGOBossPhaseDefinition Second;
		Second.PhaseTag = TAG_BossDecisionTestPhaseTwo;
		Second.EnterHealthThreshold = 0.5f;
		Definition->Phases = { First, Second };
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBossCombatConfigurationTest, "GGYGO.BossAI.Combat.Configuration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOBossCombatConfigurationTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UGGYGOBossDefinition> Definition(NewObject<UGGYGOBossDefinition>());
	ConfigureTwoPhases(Definition.Get());
	FString Error;
	TestTrue(TEXT("两个递进阶段配置有效"), Definition->ValidateCombatConfiguration(Error));
	Definition->Phases[1].EnterHealthThreshold = 2.0f;
	TestFalse(TEXT("非法归一化阈值拒绝"), Definition->ValidateCombatConfiguration(Error));
	Definition->Phases[1].EnterHealthThreshold = 0.5f;
	Definition->Phases[1].PhaseTag = Definition->InitialPhaseTag;
	TestFalse(TEXT("重复阶段标签拒绝"), Definition->ValidateCombatConfiguration(Error));
	Definition->Phases[1].PhaseTag = TAG_BossDecisionTestPhaseTwo;
	Definition->BehaviorTree = NewObject<UBehaviorTree>(Definition.Get());
	Definition->TargetLoseDistance = Definition->TargetAcquireDistance - 1.0f;
	TestFalse(TEXT("丢失距离小于获取距离拒绝"), Definition->ValidateCombatConfiguration(Error));
	Definition->TargetLoseDistance = Definition->TargetAcquireDistance;
	TestTrue(TEXT("显式同一获取/丢失距离合法"), Definition->ValidateCombatConfiguration(Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBossHealthPhaseTest, "GGYGO.BossAI.Combat.PhaseAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOBossHealthPhaseTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("Engine"), GEngine)) { return false; }
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false)
		.ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("真实 Game World"), World)) { return false; }
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	TStrongObjectPtr<UGGYGOBossDefinition> Definition(NewObject<UGGYGOBossDefinition>());
	ConfigureTwoPhases(Definition.Get());
	AGGYGOBossState* State = World->SpawnActor<AGGYGOBossState>();
	if (TestNotNull(TEXT("BossState"), State) && TestTrue(TEXT("采纳两阶段定义"), State->InitializeFromDefinition(Definition.Get())))
	{
		UGGYGOAbilitySystemComponent* ASC = State->GetGGYGOAbilitySystemComponent();
		ASC->SetNumericAttributeBase(UGGYGOHealthSet::GetMaxHealthAttribute(), 100.0f);
		ASC->SetNumericAttributeBase(UGGYGOHealthSet::GetHealthAttribute(), 51.0f);
		TestTrue(TEXT("阈值以上维持初始阶段"), State->GetCurrentPhaseTag() == Definition->InitialPhaseTag);
		ASC->SetNumericAttributeBase(UGGYGOHealthSet::GetHealthAttribute(), 50.0f);
		TestTrue(TEXT("原生State只发生命事实，不代蓝图决定50%推进"), State->GetCurrentPhaseTag() == Definition->InitialPhaseTag);
		TestTrue(TEXT("权威接口接收已声明阶段"), State->SetCurrentPhase(TAG_BossDecisionTestPhaseTwo));
		TestFalse(TEXT("旧阶段ASC标签已归还"), ASC->HasMatchingGameplayTag(Definition->InitialPhaseTag));
		TestTrue(TEXT("新阶段ASC标签已发布"), ASC->HasMatchingGameplayTag(TAG_BossDecisionTestPhaseTwo));
		ASC->SetNumericAttributeBase(UGGYGOHealthSet::GetHealthAttribute(), 100.0f);
		TestTrue(TEXT("原生生命变化不擅自替换蓝图已选阶段"), State->GetCurrentPhaseTag() == TAG_BossDecisionTestPhaseTwo);
		TestFalse(TEXT("未声明阶段拒绝"), State->SetCurrentPhase(GGYGOGameplayTags::State_Boss_Phase));
		ASC->SetNumericAttributeBase(UGGYGOHealthSet::GetHealthAttribute(), 0.0f);
		TestFalse(TEXT("零生命不切新战斗阶段"), State->SetCurrentPhase(Definition->InitialPhaseTag));
	}
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	World->GetPackage()->SetDirtyFlag(false);
	return true;
}
#endif
