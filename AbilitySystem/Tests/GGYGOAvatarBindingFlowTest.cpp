#include "AbilitySystem/Tests/GGYGOAvatarBindingFlowTestTypes.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAvatarBindingFlowTestTypes)

UGGYGOAvatarBindingHeldTestAbility::UGGYGOAvatarBindingHeldTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
}

void UGGYGOAvatarBindingHeldTestAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	// 保持运行，直到绑定流程取消它。
}

void UGGYGOAvatarBindingHeldTestAbility::CleanupAbilityResourcesForTermination(
	const FGGYGOAbilityTerminationContext& Context)
{
	++CleanupCalls;
	Super::CleanupAbilityResourcesForTermination(Context);
}

UGGYGOAvatarBindingSurvivingTestAbility::UGGYGOAvatarBindingSurvivingTestAbility(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	FGameplayTagContainer Tags;
	Tags.AddTag(GGYGOGameplayTags::Ability_Behavior_SurvivesDeath);
	SetAssetTags(Tags);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Combatants/Tests/GGYGOCombatantBindingLifecycleTestTypes.h"
#include "GameplayAbilitySpec.h"
#include "Misc/AutomationTest.h"
#include "NativeGameplayTags.h"

UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_GGYGO_Tests_AvatarBinding_Cue, "GameplayCue.GGYGO.Tests.AvatarBinding.Probe");
UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_GGYGO_Tests_AvatarBinding_Input, "InputTag.GGYGO.Tests.AvatarBinding");

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAvatarBindingRebindContractTest,
	"GGYGO.AbilitySystem.AvatarBinding.BindReleaseRebindContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAvatarBindingRebindContractTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOCombatantBindingTestWorld Fixture;
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }
	AGGYGOCombatantBindingTestState* Host = Fixture.SpawnHost();
	AGGYGOCombatantBindingTestPawn* PawnA = Fixture.SpawnPawn();
	AGGYGOCombatantBindingTestPawn* PawnB = Fixture.SpawnPawn();
	if (!TestNotNull(TEXT("host"), Host) || !TestNotNull(TEXT("pawn A"), PawnA) || !TestNotNull(TEXT("pawn B"), PawnB))
	{
		return false;
	}
	UGGYGOAbilitySystemComponent* ASC = Host->GetGGYGOAbilitySystemComponent();
	UGGYGOPawnExtensionComponent* ExtA = PawnA->GetPawnExtensionForTest();
	UGGYGOPawnExtensionComponent* ExtB = PawnB->GetPawnExtensionForTest();
	FGGYGOPawnASCNoticeLog LogA;
	FGGYGOPawnASCNoticeLog LogB;
	FGGYGOLocalASCSubscription SubA;
	FGGYGOLocalASCSubscription SubB;
	LogA.Observe(SubA, ExtA);
	LogB.Observe(SubB, ExtB);
	TestTrue(TEXT("unbound Host keeps an owner-only ActorInfo"),
		ASC->GetOwnerActor() == Host && !ASC->GetAvatarActor() && LogA.Notices.IsEmpty() && LogB.Notices.IsEmpty());

	FGameplayAbilitySpec HeldSpec(UGGYGOAvatarBindingHeldTestAbility::StaticClass(), 1);
	HeldSpec.GetDynamicSpecSourceTags().AddTag(TAG_GGYGO_Tests_AvatarBinding_Input);
	const FGameplayAbilitySpecHandle Held = ASC->GiveAbility(HeldSpec);
	const FGameplayAbilitySpecHandle Surviving = ASC->GiveAbility(
		FGameplayAbilitySpec(UGGYGOAvatarBindingSurvivingTestAbility::StaticClass(), 1));

	if (!TestTrue(TEXT("first bind succeeds"), Host->AttachAvatar(PawnA) == EGGYGOAvatarBindResult::Bound)) { return false; }
	const FGGYGOPawnASCResourceHandle ResourceA = ExtA->GetCurrentLocalAbilitySystemResource();
	TestTrue(TEXT("ActorInfo points at Host and pawn A"), ASC->GetOwnerActor() == Host && ASC->GetAvatarActor() == PawnA);
	TestTrue(TEXT("pawn A receives exactly one Ready for its resource"),
		LogA.Notices.Num() == 1 && LogA.Count(EGGYGOPawnASCNoticeKind::Ready) == 1
		&& LogA.Notices[0].Resource.HasSameResource(ResourceA));
	TestTrue(TEXT("pawn A exposes the bound ASC"),
		ExtA->GetGGYGOAbilitySystemComponent() == ASC && SubA.GetReadyASC() == ASC && LogB.Notices.IsEmpty());

	// 旧 Avatar 上的运行能力、Cue 与输入请求。
	if (!TestTrue(TEXT("held ability activates"), ASC->TryActivateAbility(Held))
		|| !TestTrue(TEXT("surviving ability activates"), ASC->TryActivateAbility(Surviving))) { return false; }
	ASC->AddGameplayCue(TAG_GGYGO_Tests_AvatarBinding_Cue);
	TestTrue(TEXT("Cue active on the old Avatar"), ASC->IsGameplayCueActive(TAG_GGYGO_Tests_AvatarBinding_Cue));
	const double Deadline = Fixture.World->GetTimeSeconds() + 10.0;
	const FGGYGOAbilityInputRequestResult Input =
		ASC->ReceiveAbilityInputRequest(TAG_GGYGO_Tests_AvatarBinding_Input, FGGYGOAbilityInputRequestIdentity{}, Deadline);
	TestTrue(TEXT("input request accepted on the old Avatar"),
		Input.Outcome == EGGYGOAbilityInputRequestOutcome::Accepted && Input.Identity.IsAssigned());

	if (!TestTrue(TEXT("rebind to pawn B succeeds"), Host->AttachAvatar(PawnB) == EGGYGOAvatarBindResult::Bound)) { return false; }
	const FGGYGOPawnASCResourceHandle ResourceB = ExtB->GetCurrentLocalAbilitySystemResource();
	TestTrue(TEXT("ActorInfo now points at pawn B with the same owner"),
		ASC->GetOwnerActor() == Host && ASC->GetAvatarActor() == PawnB && Host->GetAvatarPawn() == PawnB);
	TestTrue(TEXT("pawn A receives Released for its own resource"),
		LogA.Count(EGGYGOPawnASCNoticeKind::Released) == 1 && LogA.Notices.Last().Resource.HasSameResource(ResourceA));
	TestTrue(TEXT("pawn B receives Ready for a new resource"),
		LogB.Count(EGGYGOPawnASCNoticeKind::Ready) == 1 && LogB.Notices[0].Resource.HasSameResource(ResourceB)
		&& !ResourceB.HasSameResource(ResourceA));
	TestTrue(TEXT("pawn A no longer exposes the ASC"),
		!ExtA->GetGGYGOAbilitySystemComponent() && !SubA.GetReadyASC() && !SubA.GetConsumedResource().HasResource());
	const FGameplayAbilitySpec* HeldAfter = ASC->FindAbilitySpecFromHandle(Held);
	const FGameplayAbilitySpec* SurvivingAfter = ASC->FindAbilitySpecFromHandle(Surviving);
	TestTrue(TEXT("old Avatar abilities are canceled"), HeldAfter && !HeldAfter->IsActive());
	TestTrue(TEXT("SurvivesDeath abilities keep running across the release"), SurvivingAfter && SurvivingAfter->IsActive());
	TestFalse(TEXT("release removes the old Avatar Cues"), ASC->IsGameplayCueActive(TAG_GGYGO_Tests_AvatarBinding_Cue));
	AddExpectedMessagePlain(TEXT("Reason=WrongRevision"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FGGYGOAbilityInputRequestResult Recheck =
		ASC->ReceiveAbilityInputRequest(TAG_GGYGO_Tests_AvatarBinding_Input, Input.Identity, Deadline);
	TestTrue(TEXT("old input request no longer exists"), Recheck.Outcome == EGGYGOAbilityInputRequestOutcome::Stale);

	TestTrue(TEXT("repeated bind of the current Avatar is a no-op"),
		Host->AttachAvatar(PawnB) == EGGYGOAvatarBindResult::AlreadyBound && LogB.Notices.Num() == 1);

	Host->DetachAvatar(PawnB);
	TestTrue(TEXT("detach releases pawn B and keeps the owner"),
		LogB.Count(EGGYGOPawnASCNoticeKind::Released) == 1 && !Host->GetAvatarPawn()
		&& ASC->GetOwnerActor() == Host && !ASC->GetAvatarActor() && !ExtB->GetGGYGOAbilitySystemComponent());
	ASC->CancelAbilityHandle(Surviving);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAvatarBindingNestedRequestTest,
	"GGYGO.AbilitySystem.AvatarBinding.NestedRequestRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAvatarBindingNestedRequestTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOCombatantBindingTestWorld Fixture;
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }
	AGGYGOCombatantBindingTestState* Host = Fixture.SpawnHost();
	AGGYGOCombatantBindingTestPawn* PawnA = Fixture.SpawnPawn();
	AGGYGOCombatantBindingTestPawn* PawnB = Fixture.SpawnPawn();
	if (!TestNotNull(TEXT("host"), Host) || !TestNotNull(TEXT("pawn A"), PawnA) || !TestNotNull(TEXT("pawn B"), PawnB))
	{
		return false;
	}
	UGGYGOAbilitySystemComponent* ASC = Host->GetGGYGOAbilitySystemComponent();
	TOptional<EGGYGOAvatarBindResult> NestedResult;
	FGGYGOLocalASCSubscription Reentrant;
	Reentrant.Subscribe(PawnA->GetPawnExtensionForTest(), [&](const FGGYGOPawnASCNotice& Notice)
	{
		if (Notice.Kind == EGGYGOPawnASCNoticeKind::Ready && !NestedResult.IsSet())
		{
			NestedResult = Host->AttachAvatar(PawnB);
		}
	});
	AddExpectedMessagePlain(TEXT("Reason=NestedRequestDuringBinding"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	TestTrue(TEXT("outer bind completes"), Host->AttachAvatar(PawnA) == EGGYGOAvatarBindResult::Bound);
	TestTrue(TEXT("Ready callback's nested bind is rejected"),
		NestedResult.IsSet() && NestedResult.GetValue() == EGGYGOAvatarBindResult::Reentrant);
	TestTrue(TEXT("outer binding is unchanged"),
		ASC->GetAvatarActor() == PawnA && Host->GetAvatarPawn() == PawnA
		&& PawnA->GetPawnExtensionForTest()->GetGGYGOAbilitySystemComponent() == ASC
		&& !PawnB->GetPawnExtensionForTest()->GetCurrentLocalAbilitySystemResource().HasResource());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAvatarBindingLateReleaseTest,
	"GGYGO.AbilitySystem.AvatarBinding.ReplacedPawnLateReleaseIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAvatarBindingLateReleaseTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOCombatantBindingTestWorld Fixture;
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }
	AGGYGOCombatantBindingTestState* Host = Fixture.SpawnHost();
	AGGYGOCombatantBindingTestPawn* PawnA = Fixture.SpawnPawn();
	AGGYGOCombatantBindingTestPawn* PawnB = Fixture.SpawnPawn();
	if (!TestNotNull(TEXT("host"), Host) || !TestNotNull(TEXT("pawn A"), PawnA) || !TestNotNull(TEXT("pawn B"), PawnB))
	{
		return false;
	}
	UGGYGOAbilitySystemComponent* ASC = Host->GetGGYGOAbilitySystemComponent();
	UGGYGOPawnExtensionComponent* ExtB = PawnB->GetPawnExtensionForTest();
	Host->AttachAvatar(PawnA);
	Host->AttachAvatar(PawnB);
	const FGGYGOPawnASCResourceHandle ResourceB = ExtB->GetCurrentLocalAbilitySystemResource();
	FGGYGOPawnASCNoticeLog LogB;
	FGGYGOLocalASCSubscription SubB;
	LogB.Observe(SubB, ExtB);
	if (!TestTrue(TEXT("pawn B is the current binding"), ASC->GetAvatarActor() == PawnB
		&& ExtB->IsLocalAbilitySystemResourceReady(ResourceB) && LogB.Count(EGGYGOPawnASCNoticeKind::Ready) == 1)) { return false; }

	// 已被替换的 A 迟到的释放请求与销毁都不影响 B。
	Host->ReleaseAvatarBinding(PawnA);
	PawnA->Destroy();
	TestTrue(TEXT("late release from the replaced pawn is ignored"),
		ASC->GetAvatarActor() == PawnB && Host->GetAvatarPawn() == PawnB
		&& ExtB->IsLocalAbilitySystemResourceReady(ResourceB) && LogB.Count(EGGYGOPawnASCNoticeKind::Released) == 0);

	// 当前 Avatar 被销毁时宿主释放它。
	PawnB->Destroy();
	TestTrue(TEXT("destroying the current Avatar releases the binding"),
		LogB.Count(EGGYGOPawnASCNoticeKind::Released) == 1 && !Host->GetAvatarPawn()
		&& ASC->GetOwnerActor() == Host && !ASC->GetAvatarActor());
	return true;
}
#endif
