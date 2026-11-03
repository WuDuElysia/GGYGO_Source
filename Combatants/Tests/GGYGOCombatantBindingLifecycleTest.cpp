#include "Combatants/Tests/GGYGOCombatantBindingLifecycleTestTypes.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCombatantBindingLifecycleTestTypes)

AGGYGOCombatantBindingTestState::AGGYGOCombatantBindingTestState(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

AGGYGOCombatantBindingTestPawn::AGGYGOCombatantBindingTestPawn(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TestRoot = CreateDefaultSubobject<USceneComponent>(TEXT("TestRoot"));
	SetRootComponent(TestRoot);
	PawnExtension = CreateDefaultSubobject<UGGYGOPawnExtensionComponent>(TEXT("PawnExtension"));
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCombatantBindingOwnershipTest,
	"GGYGO.Combatants.Binding.OwnershipAndDeathGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCombatantBindingOwnershipTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOCombatantBindingTestWorld Fixture;
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }
	AGGYGOCombatantBindingTestState* FirstHost = Fixture.SpawnHost();
	AGGYGOCombatantBindingTestState* SecondHost = Fixture.SpawnHost();
	AGGYGOCombatantBindingTestPawn* FirstPawn = Fixture.SpawnPawn();
	AGGYGOCombatantBindingTestPawn* SecondPawn = Fixture.SpawnPawn();
	if (!TestNotNull(TEXT("first host"), FirstHost) || !TestNotNull(TEXT("second host"), SecondHost)
		|| !TestNotNull(TEXT("first pawn"), FirstPawn) || !TestNotNull(TEXT("second pawn"), SecondPawn))
	{
		return false;
	}
	UGGYGOAbilitySystemComponent* FirstASC = FirstHost->GetGGYGOAbilitySystemComponent();
	UGGYGOAbilitySystemComponent* SecondASC = SecondHost->GetGGYGOAbilitySystemComponent();
	UGGYGOPawnExtensionComponent* FirstExtension = FirstPawn->GetPawnExtensionForTest();

	TestTrue(TEXT("first host binds the Pawn"), FirstHost->AttachAvatar(FirstPawn) == EGGYGOAvatarBindResult::Bound);
	TestTrue(TEXT("Pawn exposes the first host ASC"),
		FirstHost->GetAvatarPawn() == FirstPawn && FirstExtension->GetGGYGOAbilitySystemComponent() == FirstASC);

	AddExpectedMessagePlain(TEXT("Reason=PawnBoundToAnotherHost"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	TestTrue(TEXT("cross-host takeover is rejected"),
		SecondHost->AttachAvatar(FirstPawn) == EGGYGOAvatarBindResult::PawnBoundElsewhere);
	TestTrue(TEXT("rejected takeover changes neither host"),
		!SecondHost->GetAvatarPawn() && !SecondASC->GetAvatarActor()
		&& FirstASC->GetAvatarActor() == FirstPawn && FirstExtension->GetGGYGOAbilitySystemComponent() == FirstASC);

	FirstASC->SetLooseGameplayTagCount(GGYGOGameplayTags::State_Dying, 1);
	TestTrue(TEXT("dying host may repeat the current attach"),
		FirstHost->AttachAvatar(FirstPawn) == EGGYGOAvatarBindResult::AlreadyBound);
	AddExpectedMessagePlain(TEXT("Reason=HostDyingOrDead"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	TestTrue(TEXT("dying host rejects a new Avatar before releasing the current one"),
		FirstHost->AttachAvatar(SecondPawn) == EGGYGOAvatarBindResult::HostDying);
	TestTrue(TEXT("rejected dying attach keeps the current binding"),
		FirstHost->GetAvatarPawn() == FirstPawn && FirstASC->GetAvatarActor() == FirstPawn
		&& !SecondPawn->GetPawnExtensionForTest()->GetGGYGOAbilitySystemComponent());
	FirstASC->SetLooseGameplayTagCount(GGYGOGameplayTags::State_Dying, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCombatantBindingTransferTest,
	"GGYGO.Combatants.Binding.CrossHostTransferAndAvatarDestroy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCombatantBindingTransferTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOCombatantBindingTestWorld Fixture;
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }
	AGGYGOCombatantBindingTestState* OldHost = Fixture.SpawnHost();
	AGGYGOCombatantBindingTestState* NewHost = Fixture.SpawnHost();
	AGGYGOCombatantBindingTestPawn* Pawn = Fixture.SpawnPawn();
	if (!TestNotNull(TEXT("old host"), OldHost) || !TestNotNull(TEXT("new host"), NewHost) || !TestNotNull(TEXT("pawn"), Pawn))
	{
		return false;
	}
	UGGYGOAbilitySystemComponent* OldASC = OldHost->GetGGYGOAbilitySystemComponent();
	UGGYGOAbilitySystemComponent* NewASC = NewHost->GetGGYGOAbilitySystemComponent();
	UGGYGOPawnExtensionComponent* Extension = Pawn->GetPawnExtensionForTest();
	FGGYGOPawnASCNoticeLog Log;
	FGGYGOLocalASCSubscription Subscription;
	Log.Observe(Subscription, Extension);

	OldHost->AttachAvatar(Pawn);
	const FGGYGOPawnASCResourceHandle OldResource = Extension->GetCurrentLocalAbilitySystemResource();
	TestTrue(TEXT("old host binding is ready"),
		Log.Count(EGGYGOPawnASCNoticeKind::Ready) == 1 && OldResource.GetIdentity().ASC.Get() == OldASC);

	// 跨宿主移交：原宿主先释放，新宿主再绑定。
	OldHost->DetachAvatar(Pawn);
	TestTrue(TEXT("old host release keeps its owner and clears the Avatar"),
		Log.Count(EGGYGOPawnASCNoticeKind::Released) == 1 && !OldHost->GetAvatarPawn()
		&& OldASC->GetOwnerActor() == OldHost && !OldASC->GetAvatarActor());
	TestTrue(TEXT("new host binds the released Pawn"), NewHost->AttachAvatar(Pawn) == EGGYGOAvatarBindResult::Bound);
	const FGGYGOPawnASCResourceHandle NewResource = Extension->GetCurrentLocalAbilitySystemResource();
	TestTrue(TEXT("transfer publishes a new resource for the new ASC"),
		Log.Count(EGGYGOPawnASCNoticeKind::Ready) == 2 && NewResource.GetIdentity().ASC.Get() == NewASC
		&& !NewResource.HasSameResource(OldResource) && Extension->GetGGYGOAbilitySystemComponent() == NewASC);
	OldHost->DetachAvatar(Pawn);
	TestTrue(TEXT("stale detach from the old host does nothing"),
		NewASC->GetAvatarActor() == Pawn && Extension->IsLocalAbilitySystemResourceReady(NewResource));

	Pawn->Destroy();
	TestTrue(TEXT("destroying the Avatar releases the new host binding"),
		Log.Count(EGGYGOPawnASCNoticeKind::Released) == 2 && !NewHost->GetAvatarPawn()
		&& NewASC->GetOwnerActor() == NewHost && !NewASC->GetAvatarActor());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCombatantBindingHostCloseTest,
	"GGYGO.Combatants.Binding.HostCloseReleasesAvatar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCombatantBindingHostCloseTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOCombatantBindingTestWorld Fixture;
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }
	AGGYGOCombatantBindingTestState* Host = Fixture.SpawnHost();
	AGGYGOCombatantBindingTestPawn* Pawn = Fixture.SpawnPawn();
	if (!TestNotNull(TEXT("host"), Host) || !TestNotNull(TEXT("pawn"), Pawn)) { return false; }
	UGGYGOAbilitySystemComponent* ASC = Host->GetGGYGOAbilitySystemComponent();
	UGGYGOPawnExtensionComponent* Extension = Pawn->GetPawnExtensionForTest();
	FGGYGOPawnASCNoticeLog Log;
	FGGYGOLocalASCSubscription Subscription;
	Log.Observe(Subscription, Extension);
	Host->AttachAvatar(Pawn);

	Host->InvokeEndPlayForTest();
	TestTrue(TEXT("host EndPlay releases the Avatar and clears ActorInfo"),
		Log.Count(EGGYGOPawnASCNoticeKind::Released) == 1 && !Host->GetAvatarPawn()
		&& !ASC->GetOwnerActor() && !ASC->GetAvatarActor()
		&& !Extension->GetCurrentLocalAbilitySystemResource().HasResource());
	AddExpectedMessagePlain(TEXT("Reason=HostLifecycleClosed"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	TestTrue(TEXT("closed host rejects a new binding"),
		Host->AttachAvatar(Pawn) == EGGYGOAvatarBindResult::LifecycleClosed);
	TestTrue(TEXT("rejected binding publishes nothing"),
		Log.Notices.Num() == 2 && !Extension->GetCurrentLocalAbilitySystemResource().HasResource());
	Host->Destroy();
	TestEqual(TEXT("destroying the closed host does not release again"), Log.Notices.Num(), 2);
	return true;
}
#endif
