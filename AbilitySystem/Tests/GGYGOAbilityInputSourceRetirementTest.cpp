/** @file GGYGOAbilityInputSourceRetirementTest.cpp @brief 输入请求按来源会话结束：只退役该来源签发的请求 */
#include "Input/Tests/GGYGOInputTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"

namespace
{
	struct FInputSourceRetirementWorld
	{
		UWorld* World = nullptr;

		FInputSourceRetirementWorld()
		{
			if (!GEngine) { return; }
			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
			if (World) { GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World); }
		}

		~FInputSourceRetirementWorld()
		{
			if (!World) { return; }
			UPackage* Package = World->GetPackage();
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
			if (Package) { Package->SetDirtyFlag(false); }
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAbilityInputSourceRetirementTest,
	"GGYGO.AbilitySystem.InputRequest.EndRequestsFromSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAbilityInputSourceRetirementTest::RunTest(const FString& Parameters)
{
	FInputSourceRetirementWorld TestWorld;
	if (!TestNotNull(TEXT("独立测试世界"), TestWorld.World)) { return false; }
	AActor* Owner = TestWorld.World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("ASC Owner/Avatar"), Owner)) { return false; }
	UGGYGOAbilitySystemComponent* ASC = NewObject<UGGYGOAbilitySystemComponent>(Owner);
	ASC->RegisterComponent();
	ASC->InitAbilityActorInfo(Owner, Owner);

	const FGameplayTag InputTag = GGYGOGameplayTags::InputTag_Attack_Light;
	FGameplayAbilitySpec Spec(UGGYGOInputTestAbility::StaticClass(), 1);
	Spec.GetDynamicSpecSourceTags().AddTag(InputTag);
	const FGameplayAbilitySpecHandle Handle = ASC->GiveAbility(Spec);
	if (!TestTrue(TEXT("授予带输入 Tag 的能力"), Handle.IsValid())) { return false; }
	ON_SCOPE_EXIT { ASC->ClearAbility(Handle); };

	// 两个来源会话各按下一次：同一来源对象的不同代号，对应同一 Hero 的前后两次输入绑定。
	const FGGYGOAbilityInputSourceKey SourceA{Owner, 0};
	const FGGYGOAbilityInputSourceKey SourceB{Owner, 1};
	const double Deadline = TestWorld.World->GetTimeSeconds() + 1.0;
	const FGGYGOAbilityInputRequestResult A = ASC->ReceiveAbilityInputRequest(
		InputTag, FGGYGOAbilityInputRequestIdentity{}, Deadline, FGGYGOMovementInputEvaluationToken{}, SourceA);
	const FGGYGOAbilityInputRequestResult B = ASC->ReceiveAbilityInputRequest(
		InputTag, FGGYGOAbilityInputRequestIdentity{}, Deadline, FGGYGOMovementInputEvaluationToken{}, SourceB);
	if (!TestTrue(TEXT("两个来源各自取得请求身份"),
		A.Outcome == EGGYGOAbilityInputRequestOutcome::Accepted && A.Identity.IsAssigned()
		&& B.Outcome == EGGYGOAbilityInputRequestOutcome::Accepted && B.Identity.IsAssigned()
		&& A.Identity != B.Identity)) { return false; }

	// 已结束请求的复核会记一次 Warning 诊断（Stale/UnknownRequest）。
	AddExpectedMessagePlain(TEXT("Reason=UnknownRequest"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	ASC->EndAbilityInputRequestsFromSource(SourceA);
	const FGGYGOAbilityInputRequestResult RecheckA = ASC->ReceiveAbilityInputRequest(InputTag, A.Identity, Deadline);
	const FGGYGOAbilityInputRequestResult RecheckB = ASC->ReceiveAbilityInputRequest(InputTag, B.Identity, Deadline);
	TestTrue(TEXT("按来源结束后，该来源的请求不再有效"), RecheckA.Outcome == EGGYGOAbilityInputRequestOutcome::Stale);
	TestTrue(TEXT("其它来源的请求不受影响"), RecheckB.Outcome == EGGYGOAbilityInputRequestOutcome::AlreadyApplied
		&& RecheckB.Identity == B.Identity);

	// 未设置来源（Owner 为空）不结束任何请求。
	ASC->EndAbilityInputRequestsFromSource(FGGYGOAbilityInputSourceKey{});
	TestTrue(TEXT("空来源不结束其它请求"), ASC->ReceiveAbilityInputRequest(InputTag, B.Identity, Deadline).Outcome
		== EGGYGOAbilityInputRequestOutcome::AlreadyApplied);
	ASC->EndAbilityInputRequestsFromSource(SourceB);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
