/** @file GGYGOBossSelectionTest.cpp @brief Boss ActionSet 校验、选招权重与一次性 Spec 身份自动化测试 */
#include "AI/Boss/Tests/GGYGOBossSelectionTestTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossSelectionTestTypes)

#if WITH_DEV_AUTOMATION_TESTS

#include "AI/Boss/GGYGOBossActionSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"

#include <limits>
#include <initializer_list>

namespace
{
	struct FBossSelectionTestWorld
	{
		UWorld* World = nullptr;
		UEngine* Engine = nullptr;

		bool Initialize()
		{
			Engine = GEngine;
			if (!Engine)
			{
				return false;
			}

			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Init);
			if (!World)
			{
				return false;
			}

			Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			return true;
		}

		~FBossSelectionTestWorld()
		{
			if (World)
			{
				// Keep this fixture's context alive through actor and component teardown.
				World->DestroyWorld(false);
				if (Engine)
				{
					Engine->DestroyWorldContext(World);
				}
				if (World->GetPackage())
				{
					World->GetPackage()->SetDirtyFlag(false);
				}
			}
		}
	};

	static FGGYGOBossActionDefinition MakeAction(FGameplayTag ActionTag, UClass* AbilityClass,
		float BaseWeight = 1.0f)
	{
		FGGYGOBossActionDefinition Action;
		Action.ActionTag = ActionTag;
		Action.AbilityClass = AbilityClass;
		Action.BaseWeight = BaseWeight;
		return Action;
	}

	static void ReplaceActions(UGGYGOBossActionSet* ActionSet,
		std::initializer_list<FGGYGOBossActionDefinition> Actions)
	{
		ActionSet->Actions.Reset();
		for (const FGGYGOBossActionDefinition& Action : Actions)
		{
			ActionSet->Actions.Add(Action);
		}
	}

	static FGameplayAbilitySpecHandle GiveAbility(UGGYGOAbilitySystemComponent* ASC, UClass* AbilityClass)
	{
		return ASC->GiveAbility(FGameplayAbilitySpec(AbilityClass, 1));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBossSelectionConfigurationTest,
	"GGYGO.BossAI.Selection.ConfigurationValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOBossSelectionConfigurationTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UGGYGOBossActionSet> ActionSet(NewObject<UGGYGOBossActionSet>(GetTransientPackage()));
	if (!TestNotNull(TEXT("瞬态 ActionSet"), ActionSet.Get()))
	{
		return false;
	}

	FString Error;
	TestTrue(TEXT("空 ActionSet 合法"), ActionSet->ValidateConfiguration(Error));
	TestTrue(TEXT("空集合不报告错误"), Error.IsEmpty());

	FGGYGOBossActionDefinition ZeroWeight = MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee,
		UGGYGOBossSelectionMeleeAbility::StaticClass(), 0.0f);
	ReplaceActions(ActionSet.Get(), { ZeroWeight });
	TestTrue(TEXT("全零基础权重合法"), ActionSet->ValidateConfiguration(Error));
	TestTrue(TEXT("全零集合不报告错误"), Error.IsEmpty());

	FGGYGOBossActionDefinition Valid = MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee,
		UGGYGOBossSelectionMeleeAbility::StaticClass(), 2.0f);
	Valid.MaxWeight = 0.25f;
	ReplaceActions(ActionSet.Get(), { Valid });
	TestTrue(TEXT("MaxWeight 小于 BaseWeight 仍合法"), ActionSet->ValidateConfiguration(Error));

	auto ExpectInvalid = [this, &ActionSet, &Error](const TCHAR* Label,
		const FGGYGOBossActionDefinition& InvalidAction)
	{
		ReplaceActions(ActionSet.Get(), { InvalidAction });
		Error.Reset();
		TestFalse(Label, ActionSet->ValidateConfiguration(Error));
		TestFalse(FString::Printf(TEXT("%s 给出诊断"), Label), Error.IsEmpty());
	};

	FGGYGOBossActionDefinition Invalid = Valid;
	Invalid.AbilityClass = nullptr;
	ExpectInvalid(TEXT("空能力类整体拒绝"), Invalid);

	Invalid = Valid;
	Invalid.AbilityClass = UGGYGOBossSelectionAbstractAbility::StaticClass();
	ExpectInvalid(TEXT("抽象能力类整体拒绝"), Invalid);

	Invalid = Valid;
	Invalid.ActionTag = GGYGOGameplayTags::BossAction_Attack_Melee_Ice01;
	ExpectInvalid(TEXT("CDO ActionTag 不匹配整体拒绝"), Invalid);

	Invalid = Valid;
	Invalid.ActionTag = GGYGOGameplayTags::State_Boss_Phase_One;
	ExpectInvalid(TEXT("非 BossAction 命名空间整体拒绝"), Invalid);

	Invalid = Valid;
	Invalid.BaseWeight = std::numeric_limits<float>::quiet_NaN();
	ExpectInvalid(TEXT("NaN 整体拒绝"), Invalid);

	Invalid = Valid;
	Invalid.MinDistance = std::numeric_limits<float>::infinity();
	ExpectInvalid(TEXT("Inf 整体拒绝"), Invalid);

	Invalid = Valid;
	Invalid.MinDistance = 10.0f;
	Invalid.MaxDistance = 9.0f;
	ExpectInvalid(TEXT("最大距离小于最小距离整体拒绝"), Invalid);

	Invalid = Valid;
	Invalid.MaxFacingAngle = 181.0f;
	ExpectInvalid(TEXT("朝向角超出范围整体拒绝"), Invalid);

	Invalid = Valid;
	Invalid.RepeatPenalty = -0.01f;
	ExpectInvalid(TEXT("负重复惩罚整体拒绝"), Invalid);

	Invalid = Valid;
	Invalid.RepeatPenalty = 1.01f;
	ExpectInvalid(TEXT("重复惩罚超过一整体拒绝"), Invalid);

	Invalid = Valid;
	Invalid.UnusedWeightGain = -0.01f;
	ExpectInvalid(TEXT("负未选权重增长整体拒绝"), Invalid);

	Invalid = Valid;
	Invalid.MaxWeight = std::numeric_limits<float>::infinity();
	ExpectInvalid(TEXT("非有限权重上限整体拒绝"), Invalid);

	ReplaceActions(ActionSet.Get(), { Valid, Valid });
	Error.Reset();
	TestFalse(TEXT("重复 ActionTag 整体拒绝"), ActionSet->ValidateConfiguration(Error));
	TestNull(TEXT("FindAction 不掩盖重复 Tag"), ActionSet->FindAction(Valid.ActionTag));
	TestFalse(TEXT("重复 Tag 给出诊断"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBossSelectionWeightsTest,
	"GGYGO.BossAI.Selection.EligibleWeightsAndDeterminism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOBossSelectionWeightsTest::RunTest(const FString& Parameters)
{
	FBossSelectionTestWorld TestWorld;
	if (!TestNotNull(TEXT("UEngine"), GEngine) || !TestTrue(TEXT("创建含独立 WorldContext 的测试世界"), TestWorld.Initialize()))
	{
		return false;
	}

	AGGYGOBossSelectionTestController* Controller = TestWorld.World->SpawnActor<AGGYGOBossSelectionTestController>();
	AGGYGOBossSelectionTestController* SameSeedController = TestWorld.World->SpawnActor<AGGYGOBossSelectionTestController>();
	if (!TestNotNull(TEXT("选择测试 Controller"), Controller)
		|| !TestNotNull(TEXT("同种子 Controller"), SameSeedController))
	{
		return false;
	}

	TStrongObjectPtr<UGGYGOBossActionSet> EmptySet(NewObject<UGGYGOBossActionSet>(GetTransientPackage()));
	FString EmptySetError;
	TestTrue(TEXT("空集有效"), EmptySet->ValidateConfiguration(EmptySetError));
	TestNull(TEXT("无候选时不选择动作"), Controller->SelectAction(EmptySet.Get(), {}));

	TStrongObjectPtr<UGGYGOBossActionSet> ZeroSet(NewObject<UGGYGOBossActionSet>(GetTransientPackage()));
	ReplaceActions(ZeroSet.Get(), {
		MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee, UGGYGOBossSelectionMeleeAbility::StaticClass(), 0.0f)
	});
	FString Error;
	TestTrue(TEXT("全零集校验合法"), ZeroSet->ValidateConfiguration(Error));
	TestNull(TEXT("全零基础权重不产生选择"), Controller->SelectAction(ZeroSet.Get(),
		{ GGYGOGameplayTags::BossAction_Attack_Melee }));

	TStrongObjectPtr<UGGYGOBossActionSet> InvalidSet(NewObject<UGGYGOBossActionSet>(GetTransientPackage()));
	ReplaceActions(InvalidSet.Get(), {
		MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee, UGGYGOBossSelectionMeleeAbility::StaticClass()),
		MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee_Ice01, UGGYGOBossSelectionMeleeAbility::StaticClass())
	});
	AddExpectedError(TEXT("BossAIController"), EAutomationExpectedErrorFlags::Contains, 1);
	TestNull(TEXT("一个坏行使整组不参与选择"), Controller->SelectAction(InvalidSet.Get(),
		{ GGYGOGameplayTags::BossAction_Attack_Melee }));

	TStrongObjectPtr<UGGYGOBossActionSet> SingleZeroPenalty(NewObject<UGGYGOBossActionSet>(GetTransientPackage()));
	FGGYGOBossActionDefinition SingleAction = MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee,
		UGGYGOBossSelectionMeleeAbility::StaticClass());
	SingleAction.RepeatPenalty = 0.0f;
	SingleAction.UnusedWeightGain = 0.0f;
	ReplaceActions(SingleZeroPenalty.Get(), { SingleAction });
	Controller->InitializeDecisionStream(812);
	for (int32 Index = 0; Index < 64; ++Index)
	{
		const FGGYGOBossActionDefinition* Selected = Controller->SelectAction(SingleZeroPenalty.Get(),
			{ SingleAction.ActionTag });
		if (!TestNotNull(TEXT("penalty=0 的唯一合法动作不会永久饿死"), Selected))
		{
			return false;
		}
		TestTrue(TEXT("单招始终选择自身"), Selected->ActionTag == SingleAction.ActionTag);
	}

	TStrongObjectPtr<UGGYGOBossActionSet> UnderflowSet(NewObject<UGGYGOBossActionSet>(GetTransientPackage()));
	FGGYGOBossActionDefinition UnderflowAction = SingleAction;
	UnderflowAction.RepeatPenalty = 0.5f;
	ReplaceActions(UnderflowSet.Get(), { UnderflowAction });
	Controller->InitializeDecisionStream(813);
	for (int32 Index = 0; Index < 2048; ++Index)
	{
		if (!TestNotNull(TEXT("长序列下溢后仍能选择唯一合法动作"),
			Controller->SelectAction(UnderflowSet.Get(), { UnderflowAction.ActionTag })))
		{
			return false;
		}
	}

	TStrongObjectPtr<UGGYGOBossActionSet> ExhaustedSet(NewObject<UGGYGOBossActionSet>(GetTransientPackage()));
	FGGYGOBossActionDefinition ExhaustedA = MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee,
		UGGYGOBossSelectionMeleeAbility::StaticClass());
	FGGYGOBossActionDefinition ExhaustedB = MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee_Ice01,
		UGGYGOBossSelectionIceOneAbility::StaticClass(), 3.0f);
	FGGYGOBossActionDefinition ExhaustedC = MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee_Ice02,
		UGGYGOBossSelectionIceTwoAbility::StaticClass(), 7.0f);
	ExhaustedA.RepeatPenalty = 0.0f;
	ExhaustedB.RepeatPenalty = 0.0f;
	ExhaustedA.UnusedWeightGain = 0.0f;
	ExhaustedB.UnusedWeightGain = 2.0f;
	ExhaustedC.UnusedWeightGain = 4.0f;
	ExhaustedA.MaxWeight = 20.0f;
	ExhaustedB.MaxWeight = 20.0f;
	ExhaustedC.MaxWeight = 20.0f;
	ReplaceActions(ExhaustedSet.Get(), { ExhaustedA, ExhaustedB, ExhaustedC });
	Controller->InitializeDecisionStream(814);
	Controller->SetWeightSourceForTest(ExhaustedSet.Get());
	Controller->SetRuntimeWeightForTest(ExhaustedA.ActionTag, 0.0f);
	Controller->SetRuntimeWeightForTest(ExhaustedB.ActionTag, 0.0f);
	Controller->SetRuntimeWeightForTest(ExhaustedC.ActionTag, 0.0f);
	const FGGYGOBossActionDefinition* EligibleOnly = Controller->SelectAction(ExhaustedSet.Get(),
		{ ExhaustedB.ActionTag });
	if (!TestNotNull(TEXT("多招惩罚耗尽后恢复基础权重"), EligibleOnly))
	{
		return false;
	}
	TestTrue(TEXT("回退只在调用方给出的合法候选中选择"), EligibleOnly->ActionTag == ExhaustedB.ActionTag);

	Controller->SetRuntimeWeightForTest(ExhaustedA.ActionTag, 0.0f);
	Controller->SetRuntimeWeightForTest(ExhaustedB.ActionTag, 0.0f);
	Controller->SetRuntimeWeightForTest(ExhaustedC.ActionTag, 0.0f);
	const FGGYGOBossActionDefinition* AllEligibleExhausted = Controller->SelectAction(ExhaustedSet.Get(),
		{ ExhaustedA.ActionTag, ExhaustedB.ActionTag });
	if (!TestNotNull(TEXT("多个候选全部耗尽后按基础权重恢复"), AllEligibleExhausted))
	{
		return false;
	}
	TestTrue(TEXT("全耗尽恢复仍选择合法候选"), AllEligibleExhausted->ActionTag == ExhaustedA.ActionTag
		|| AllEligibleExhausted->ActionTag == ExhaustedB.ActionTag);
	const FGGYGOBossActionDefinition& UnselectedEligibleAction = AllEligibleExhausted->ActionTag == ExhaustedA.ActionTag
		? ExhaustedB : ExhaustedA;
	const float ExpectedRestoredEligibleWeight = UnselectedEligibleAction.BaseWeight
		+ UnselectedEligibleAction.UnusedWeightGain;
	float RestoredEligibleRuntimeWeight = 0.0f;
	TestTrue(TEXT("全耗尽时未选中的合法候选先恢复基础权重再累加未选增长"),
		Controller->TryGetRuntimeWeightForTest(UnselectedEligibleAction.ActionTag, RestoredEligibleRuntimeWeight)
		&& FMath::IsNearlyEqual(RestoredEligibleRuntimeWeight, ExpectedRestoredEligibleWeight));
	float IneligibleRuntimeWeight = 0.0f;
	TestTrue(TEXT("未提供给选择器的行只按既有未选增长更新"),
		Controller->TryGetRuntimeWeightForTest(ExhaustedC.ActionTag, IneligibleRuntimeWeight)
		&& FMath::IsNearlyEqual(IneligibleRuntimeWeight, ExhaustedC.UnusedWeightGain));

	TStrongObjectPtr<UGGYGOBossActionSet> WeightCapSet(NewObject<UGGYGOBossActionSet>(GetTransientPackage()));
	FGGYGOBossActionDefinition WeightCapA = MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee,
		UGGYGOBossSelectionMeleeAbility::StaticClass(), 2.0f);
	FGGYGOBossActionDefinition WeightCapB = MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee_Ice01,
		UGGYGOBossSelectionIceOneAbility::StaticClass(), 2.0f);
	WeightCapA.RepeatPenalty = 0.25f;
	WeightCapB.RepeatPenalty = 0.25f;
	WeightCapA.UnusedWeightGain = 10.0f;
	WeightCapB.UnusedWeightGain = 10.0f;
	WeightCapA.MaxWeight = 0.5f;
	WeightCapB.MaxWeight = 0.5f;
	ReplaceActions(WeightCapSet.Get(), { WeightCapA, WeightCapB });
	TestTrue(TEXT("MaxWeight 小于 BaseWeight 的权重集有效"), WeightCapSet->ValidateConfiguration(Error));
	Controller->SetWeightSourceForTest(ExhaustedSet.Get());
	Controller->SetRuntimeWeightForTest(WeightCapA.ActionTag, 99.0f);
	Controller->SetRuntimeWeightForTest(WeightCapB.ActionTag, 99.0f);
	const FGGYGOBossActionDefinition* WeightCapSelected = Controller->SelectAction(WeightCapSet.Get(),
		{ WeightCapA.ActionTag, WeightCapB.ActionTag });
	if (!TestNotNull(TEXT("来源变化后仍正常选招"), WeightCapSelected))
	{
		return false;
	}
	const float WeightA = Controller->GetActionWeight(WeightCapA);
	const float WeightB = Controller->GetActionWeight(WeightCapB);
	TestTrue(TEXT("不同 ActionSet 清除旧派生权重且上限不低于基础权重"),
		(FMath::IsNearlyEqual(WeightA, 0.5f) && FMath::IsNearlyEqual(WeightB, 2.0f))
		|| (FMath::IsNearlyEqual(WeightB, 0.5f) && FMath::IsNearlyEqual(WeightA, 2.0f)));

	TStrongObjectPtr<UGGYGOBossActionSet> LargeFiniteSet(NewObject<UGGYGOBossActionSet>(GetTransientPackage()));
	const float LargeWeight = std::numeric_limits<float>::max() * 0.75f;
	FGGYGOBossActionDefinition LargeA = MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee,
		UGGYGOBossSelectionMeleeAbility::StaticClass(), LargeWeight);
	FGGYGOBossActionDefinition LargeB = MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee_Ice01,
		UGGYGOBossSelectionIceOneAbility::StaticClass(), LargeWeight);
	LargeA.RepeatPenalty = 1.0f;
	LargeB.RepeatPenalty = 1.0f;
	LargeA.UnusedWeightGain = 0.0f;
	LargeB.UnusedWeightGain = 0.0f;
	ReplaceActions(LargeFiniteSet.Get(), { LargeA, LargeB });
	TestTrue(TEXT("大有限值集通过配置校验"), LargeFiniteSet->ValidateConfiguration(Error));
	const FGGYGOBossActionDefinition* LargeSelected = Controller->SelectAction(LargeFiniteSet.Get(),
		{ LargeA.ActionTag, LargeB.ActionTag });
	if (!TestNotNull(TEXT("双精度累计处理会溢出 float 总和的大有限权重"), LargeSelected))
	{
		return false;
	}
	TestTrue(TEXT("大权重更新保持有限"), FMath::IsFinite(Controller->GetActionWeight(LargeA))
		&& FMath::IsFinite(Controller->GetActionWeight(LargeB)));

	TStrongObjectPtr<UGGYGOBossActionSet> ReproducibleSet(NewObject<UGGYGOBossActionSet>(GetTransientPackage()));
	FGGYGOBossActionDefinition ReproA = MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee,
		UGGYGOBossSelectionMeleeAbility::StaticClass());
	FGGYGOBossActionDefinition ReproB = MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee_Ice01,
		UGGYGOBossSelectionIceOneAbility::StaticClass());
	ReplaceActions(ReproducibleSet.Get(), { ReproA, ReproB });
	Controller->InitializeDecisionStream(20260929);
	SameSeedController->InitializeDecisionStream(20260929);
	for (int32 Index = 0; Index < 128; ++Index)
	{
		const FGGYGOBossActionDefinition* First = Controller->SelectAction(ReproducibleSet.Get(),
			{ ReproA.ActionTag, ReproB.ActionTag });
		const FGGYGOBossActionDefinition* Second = SameSeedController->SelectAction(ReproducibleSet.Get(),
			{ ReproA.ActionTag, ReproB.ActionTag });
		if (!TestNotNull(TEXT("首个同种子结果"), First) || !TestNotNull(TEXT("第二个同种子结果"), Second))
		{
			return false;
		}
		TestTrue(TEXT("同 seed 产生相同选招序列"), First->ActionTag == Second->ActionTag);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBossSelectionPendingIdentityTest,
	"GGYGO.BossAI.Selection.PendingSpecIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOBossSelectionPendingIdentityTest::RunTest(const FString& Parameters)
{
	FBossSelectionTestWorld TestWorld;
	if (!TestNotNull(TEXT("UEngine"), GEngine) || !TestTrue(TEXT("创建含独立 WorldContext 的测试世界"), TestWorld.Initialize()))
	{
		return false;
	}

	AGGYGOBossSelectionTestController* Controller = TestWorld.World->SpawnActor<AGGYGOBossSelectionTestController>();
	AActor* Owner = TestWorld.World->SpawnActor<AActor>();
	AActor* ReplacementAvatar = TestWorld.World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("选择测试 Controller"), Controller)
		|| !TestNotNull(TEXT("ASC Owner/Avatar"), Owner)
		|| !TestNotNull(TEXT("替换 Avatar"), ReplacementAvatar))
	{
		return false;
	}

	UGGYGOAbilitySystemComponent* ASC = NewObject<UGGYGOAbilitySystemComponent>(Owner);
	ASC->RegisterComponent();
	ASC->InitAbilityActorInfo(Owner, Owner);
	UGGYGOAbilitySystemComponent* OtherASC = NewObject<UGGYGOAbilitySystemComponent>(ReplacementAvatar);
	OtherASC->RegisterComponent();
	OtherASC->InitAbilityActorInfo(ReplacementAvatar, ReplacementAvatar);

	FGameplayAbilitySpecHandle MeleeHandle = GiveAbility(ASC, UGGYGOBossSelectionMeleeAbility::StaticClass());
	const FGameplayAbilitySpecHandle AlternateMeleeHandle = GiveAbility(ASC,
		UGGYGOBossSelectionMeleeAlternateAbility::StaticClass());
	const FGameplayAbilitySpecHandle IceHandle = GiveAbility(ASC, UGGYGOBossSelectionIceOneAbility::StaticClass());
	const FGameplayAbilitySpecHandle OtherASCHandle = GiveAbility(OtherASC, UGGYGOBossSelectionMeleeAbility::StaticClass());
	if (!TestTrue(TEXT("主 ASC 已授予测试 Spec"), MeleeHandle.IsValid())
		|| !TestTrue(TEXT("同 Tag 的另一能力类已授予"), AlternateMeleeHandle.IsValid())
		|| !TestTrue(TEXT("第二个 Action Tag 已授予"), IceHandle.IsValid())
		|| !TestTrue(TEXT("第二 ASC 已授予匹配 Spec"), OtherASCHandle.IsValid()))
	{
		return false;
	}

	TStrongObjectPtr<UGGYGOBossActionSet> ActionSet(NewObject<UGGYGOBossActionSet>(GetTransientPackage()));
	ReplaceActions(ActionSet.Get(), {
		MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee, UGGYGOBossSelectionMeleeAbility::StaticClass()),
		MakeAction(GGYGOGameplayTags::BossAction_Attack_Melee_Ice01, UGGYGOBossSelectionIceOneAbility::StaticClass())
	});
	TStrongObjectPtr<UGGYGOBossActionSet> EquivalentSet(NewObject<UGGYGOBossActionSet>(GetTransientPackage()));
	EquivalentSet->Actions = ActionSet->Actions;
	const FGameplayTag Phase = GGYGOGameplayTags::State_Boss_Phase_One;
	const FGameplayTag OtherPhase = GGYGOGameplayTags::State_Boss_Phase;
	const FGameplayTag MeleeTag = GGYGOGameplayTags::BossAction_Attack_Melee;
	const FGameplayTag IceTag = GGYGOGameplayTags::BossAction_Attack_Melee_Ice01;

	FGameplayAbilitySpec* GrantedMeleeSpec = ASC->FindAbilitySpecFromHandle(MeleeHandle);
	if (!TestNotNull(TEXT("可检查的能力 Spec"), GrantedMeleeSpec))
	{
		return false;
	}
	TestFalse(TEXT("尚未激活的测试能力"), ASC->FindAbilitySpecFromHandle(MeleeHandle)->IsActive());
	ActionSet->Actions[0].BaseWeight = 0.0f;
	TestFalse(TEXT("已禁用的零基础权重动作不能保存执行请求"), Controller->StoreActionSelection(ActionSet.Get(),
		Phase, ASC, MeleeTag, MeleeHandle));
	ActionSet->Actions[0].BaseWeight = 1.0f;
	TestTrue(TEXT("保存选择只记录请求，不激活能力"), Controller->StoreActionSelection(ActionSet.Get(), Phase,
		ASC, MeleeTag, MeleeHandle));
	TestFalse(TEXT("Store 不激活 GA"), ASC->FindAbilitySpecFromHandle(MeleeHandle)->IsActive());
	ActionSet->Actions[0].BaseWeight = 0.0f;
	FGameplayAbilitySpecHandle DisabledOutput = MeleeHandle;
	TestFalse(TEXT("保存后基础权重变零使请求消费失败"), Controller->ConsumeActionSelection(ActionSet.Get(),
		Phase, ASC, MeleeTag, DisabledOutput));
	TestFalse(TEXT("禁用动作消费失败清空输出"), DisabledOutput.IsValid());
	ActionSet->Actions[0].BaseWeight = 1.0f;
	FGameplayAbilitySpecHandle DisabledRequestConsumed;
	TestFalse(TEXT("禁用动作的失败消费清除旧请求"), Controller->ConsumeActionSelection(ActionSet.Get(),
		Phase, ASC, MeleeTag, DisabledRequestConsumed));
	TestTrue(TEXT("恢复有效配置后可保存新请求"), Controller->StoreActionSelection(ActionSet.Get(), Phase,
		ASC, MeleeTag, MeleeHandle));
	FGameplayAbilitySpecHandle ConsumedHandle;
	TestTrue(TEXT("正确来源消费成功"), Controller->ConsumeActionSelection(ActionSet.Get(), Phase,
		ASC, MeleeTag, ConsumedHandle));
	TestTrue(TEXT("返回保存时的原始 Spec"), ConsumedHandle == MeleeHandle);
	ConsumedHandle = MeleeHandle;
	TestFalse(TEXT("成功消费后请求不可重复消费"), Controller->ConsumeActionSelection(ActionSet.Get(), Phase,
		ASC, MeleeTag, ConsumedHandle));
	TestFalse(TEXT("重复消费失败清空输出 Handle"), ConsumedHandle.IsValid());

	auto VerifyFailureConsumesRequest = [this, Controller, ASC, ActionSet = ActionSet.Get(), Phase, MeleeTag,
		MeleeHandle](const TCHAR* Label, const UGGYGOBossActionSet* RequestedSet, FGameplayTag RequestedPhase,
		UGGYGOAbilitySystemComponent* RequestedASC, FGameplayTag RequestedTag)
	{
		TestTrue(FString::Printf(TEXT("%s：保存原请求"), Label), Controller->StoreActionSelection(ActionSet,
			Phase, ASC, MeleeTag, MeleeHandle));
		FGameplayAbilitySpecHandle MismatchedOutput = MeleeHandle;
		TestFalse(FString::Printf(TEXT("%s：来源身份不匹配"), Label), Controller->ConsumeActionSelection(
			RequestedSet, RequestedPhase, RequestedASC, RequestedTag, MismatchedOutput));
		TestFalse(FString::Printf(TEXT("%s：失败清空输出"), Label), MismatchedOutput.IsValid());
		FGameplayAbilitySpecHandle OriginalOutput = MeleeHandle;
		TestFalse(FString::Printf(TEXT("%s：失败也消费原请求"), Label), Controller->ConsumeActionSelection(
			ActionSet, Phase, ASC, MeleeTag, OriginalOutput));
	};

	VerifyFailureConsumesRequest(TEXT("ActionSet 身份"), EquivalentSet.Get(), Phase, ASC, MeleeTag);
	VerifyFailureConsumesRequest(TEXT("Phase 身份"), ActionSet.Get(), OtherPhase, ASC, MeleeTag);
	VerifyFailureConsumesRequest(TEXT("ASC 身份"), ActionSet.Get(), Phase, OtherASC, MeleeTag);
	VerifyFailureConsumesRequest(TEXT("ActionTag 身份"), ActionSet.Get(), Phase, ASC, IceTag);

	TestTrue(TEXT("保存后能力类变化前的请求"), Controller->StoreActionSelection(ActionSet.Get(), Phase,
		ASC, MeleeTag, MeleeHandle));
	ActionSet->Actions[0].AbilityClass = UGGYGOBossSelectionMeleeAlternateAbility::StaticClass();
	FGameplayAbilitySpecHandle ClassMismatchOutput = MeleeHandle;
	TestFalse(TEXT("同 ActionTag 换成另一能力类仍拒绝"), Controller->ConsumeActionSelection(ActionSet.Get(),
		Phase, ASC, MeleeTag, ClassMismatchOutput));
	TestFalse(TEXT("能力类不匹配失败清空输出"), ClassMismatchOutput.IsValid());
	FGameplayAbilitySpecHandle AfterClassMismatch = MeleeHandle;
	TestFalse(TEXT("能力类不匹配失败消费请求"), Controller->ConsumeActionSelection(ActionSet.Get(),
		Phase, ASC, MeleeTag, AfterClassMismatch));
	ActionSet->Actions[0].AbilityClass = UGGYGOBossSelectionMeleeAbility::StaticClass();

	TestTrue(TEXT("保存 Avatar 变更前的请求"), Controller->StoreActionSelection(ActionSet.Get(), Phase,
		ASC, MeleeTag, MeleeHandle));
	ASC->InitAbilityActorInfo(Owner, ReplacementAvatar);
	FGameplayAbilitySpecHandle AvatarMismatchOutput = MeleeHandle;
	TestFalse(TEXT("ASC Avatar 变化使请求失效"), Controller->ConsumeActionSelection(ActionSet.Get(),
		Phase, ASC, MeleeTag, AvatarMismatchOutput));
	TestFalse(TEXT("Avatar 不匹配失败清空输出"), AvatarMismatchOutput.IsValid());
	ASC->InitAbilityActorInfo(Owner, Owner);

	TestTrue(TEXT("保存 Spec 替换前的请求"), Controller->StoreActionSelection(ActionSet.Get(), Phase,
		ASC, MeleeTag, MeleeHandle));
	ASC->ClearAbility(MeleeHandle);
	const FGameplayAbilitySpecHandle ReplacementMeleeHandle = GiveAbility(ASC,
		UGGYGOBossSelectionMeleeAbility::StaticClass());
	TestTrue(TEXT("重授同类能力得到新的 Spec Handle"), ReplacementMeleeHandle.IsValid()
		&& ReplacementMeleeHandle != MeleeHandle);
	FGameplayAbilitySpecHandle RegrantedOutput = ReplacementMeleeHandle;
	TestFalse(TEXT("移除並重授同類能力不能替换原请求"), Controller->ConsumeActionSelection(ActionSet.Get(),
		Phase, ASC, MeleeTag, RegrantedOutput));
	TestFalse(TEXT("Spec 已替换时失败清空输出"), RegrantedOutput.IsValid());
	MeleeHandle = ReplacementMeleeHandle;

	TestTrue(TEXT("保存后显式清除"), Controller->StoreActionSelection(ActionSet.Get(), Phase,
		ASC, MeleeTag, MeleeHandle));
	Controller->ClearActionSelection();
	FGameplayAbilitySpecHandle ClearedOutput = MeleeHandle;
	TestFalse(TEXT("ClearActionSelection 使请求失效"), Controller->ConsumeActionSelection(ActionSet.Get(),
		Phase, ASC, MeleeTag, ClearedOutput));
	TestFalse(TEXT("显式清除后的输出无效"), ClearedOutput.IsValid());

	TestTrue(TEXT("保存后重新初始化随机流"), Controller->StoreActionSelection(ActionSet.Get(), Phase,
		ASC, MeleeTag, MeleeHandle));
	Controller->InitializeDecisionStream(919);
	FGameplayAbilitySpecHandle ReinitializedOutput = MeleeHandle;
	TestFalse(TEXT("重新初始化决策流使请求失效"), Controller->ConsumeActionSelection(ActionSet.Get(),
		Phase, ASC, MeleeTag, ReinitializedOutput));

	TestTrue(TEXT("保存后重新选招"), Controller->StoreActionSelection(ActionSet.Get(), Phase,
		ASC, MeleeTag, MeleeHandle));
	const FGGYGOBossActionDefinition* Reselected = Controller->SelectAction(ActionSet.Get(), { MeleeTag });
	if (!TestNotNull(TEXT("重选仍能返回生产候选"), Reselected))
	{
		return false;
	}
	FGameplayAbilitySpecHandle ReselectionOutput = MeleeHandle;
	TestFalse(TEXT("重选使旧执行请求失效"), Controller->ConsumeActionSelection(ActionSet.Get(),
		Phase, ASC, MeleeTag, ReselectionOutput));

	TestTrue(TEXT("保存后 UnPossess"), Controller->StoreActionSelection(ActionSet.Get(), Phase,
		ASC, MeleeTag, MeleeHandle));
	Controller->InvokeOnUnPossessForTest();
	FGameplayAbilitySpecHandle UnpossessedOutput = MeleeHandle;
	TestFalse(TEXT("OnUnPossess 清理待执行请求"), Controller->ConsumeActionSelection(ActionSet.Get(),
		Phase, ASC, MeleeTag, UnpossessedOutput));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
