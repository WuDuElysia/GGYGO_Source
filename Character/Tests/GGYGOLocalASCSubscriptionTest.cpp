#include "Character/Components/GGYGOPawnExtensionComponent.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Combatants/Tests/GGYGOCombatantBindingLifecycleTestTypes.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOLocalASCSubscriptionTest,
	"GGYGO.Character.PawnExtension.LocalASCSubscription",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOLocalASCSubscriptionTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOCombatantBindingTestWorld Fixture;
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }
	AGGYGOCombatantBindingTestState* Host = Fixture.SpawnHost();
	AGGYGOCombatantBindingTestState* OtherHost = Fixture.SpawnHost();
	AGGYGOCombatantBindingTestPawn* Pawn = Fixture.SpawnPawn();
	if (!TestNotNull(TEXT("host"), Host) || !TestNotNull(TEXT("other host"), OtherHost) || !TestNotNull(TEXT("pawn"), Pawn))
	{
		return false;
	}
	UGGYGOAbilitySystemComponent* ASC = Host->GetGGYGOAbilitySystemComponent();
	UGGYGOPawnExtensionComponent* Extension = Pawn->GetPawnExtensionForTest();

	// 订阅时已就绪：立即回放一次 Ready。
	Host->AttachAvatar(Pawn);
	const FGGYGOPawnASCResourceHandle FirstResource = Extension->GetCurrentLocalAbilitySystemResource();
	FGGYGOPawnASCNoticeLog LateLog;
	FGGYGOLocalASCSubscription Late;
	LateLog.Observe(Late, Extension);
	TestTrue(TEXT("late subscription replays the current Ready once"),
		Late.IsSubscribed() && LateLog.Notices.Num() == 1 && LateLog.Count(EGGYGOPawnASCNoticeKind::Ready) == 1
		&& LateLog.Notices[0].Resource.HasSameResource(FirstResource));
	TestTrue(TEXT("subscription exposes the consumed ready ASC"),
		Late.GetReadyASC() == ASC && Late.GetConsumedResource().HasSameResource(FirstResource));

	// 同一 ASC 再次就绪不重复广播；另一 ASC 不能接管仍绑定的 Pawn。
	TestTrue(TEXT("repeated bound notice for the same ASC is accepted"), Extension->HandleAbilitySystemBound(ASC));
	TestEqual(TEXT("repeated bound notice does not broadcast Ready again"), LateLog.Notices.Num(), 1);
	AddExpectedErrorPlain(TEXT("HandleAbilitySystemBound"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("another ASC cannot take over a bound Pawn"),
		Extension->HandleAbilitySystemBound(OtherHost->GetGGYGOAbilitySystemComponent()));
	TestTrue(TEXT("rejected takeover keeps the original resource"),
		Extension->GetCurrentLocalAbilitySystemResource().HasSameResource(FirstResource));

	// Released 前已清空当前资源；已退订的订阅不再收到通知。
	{
		FGGYGOPawnASCNoticeLog ScopedLog;
		FGGYGOLocalASCSubscription Scoped;
		ScopedLog.Observe(Scoped, Extension);
		TestEqual(TEXT("scoped subscription receives its replay"), ScopedLog.Notices.Num(), 1);
	}
	bool bReleaseSawClearedResource = false;
	FGGYGOLocalASCSubscription ReleaseProbe;
	ReleaseProbe.Subscribe(Extension, [&](const FGGYGOPawnASCNotice& Notice)
	{
		if (Notice.Kind == EGGYGOPawnASCNoticeKind::Released)
		{
			bReleaseSawClearedResource = !Extension->GetCurrentLocalAbilitySystemResource().HasResource()
				&& !Extension->GetGGYGOAbilitySystemComponent();
		}
	});
	Host->DetachAvatar(Pawn);
	TestTrue(TEXT("Released carries the consumed resource"),
		LateLog.Count(EGGYGOPawnASCNoticeKind::Released) == 1 && LateLog.Notices.Last().Resource.HasSameResource(FirstResource));
	TestTrue(TEXT("Released callbacks see the resource already cleared"), bReleaseSawClearedResource);
	TestTrue(TEXT("Released clears the consumed resource"), !Late.GetReadyASC() && !Late.GetConsumedResource().HasResource());
	Extension->HandleAbilitySystemReleased();
	TestEqual(TEXT("releasing without a resource is a no-op"), LateLog.Notices.Num(), 2);

	// 分发中退订与重新订阅：被退订者不再收到，本次分发不重复投递，新订阅自行回放一次。
	FGGYGOPawnASCNoticeLog DroppedLog;
	FGGYGOPawnASCNoticeLog OldReplacedLog;
	FGGYGOPawnASCNoticeLog NewReplacedLog;
	FGGYGOLocalASCSubscription Mutator;
	FGGYGOLocalASCSubscription Dropped;
	FGGYGOLocalASCSubscription Replaced;
	bool bMutated = false;
	Mutator.Subscribe(Extension, [&](const FGGYGOPawnASCNotice& Notice)
	{
		if (Notice.Kind != EGGYGOPawnASCNoticeKind::Ready || bMutated) { return; }
		bMutated = true;
		Dropped.Unsubscribe();
		NewReplacedLog.Observe(Replaced, Extension);
	});
	DroppedLog.Observe(Dropped, Extension);
	OldReplacedLog.Observe(Replaced, Extension);
	Host->AttachAvatar(Pawn);
	const FGGYGOPawnASCResourceHandle SecondResource = Extension->GetCurrentLocalAbilitySystemResource();
	TestTrue(TEXT("rebinding creates a new resource"), SecondResource.HasResource() && !SecondResource.HasSameResource(FirstResource));
	TestTrue(TEXT("mutation ran inside the Ready dispatch"), bMutated);
	TestEqual(TEXT("unsubscribed during dispatch receives nothing"), DroppedLog.Notices.Num(), 0);
	TestEqual(TEXT("replaced subscription's old callback receives nothing"), OldReplacedLog.Notices.Num(), 0);
	TestTrue(TEXT("re-subscription replays the new Ready exactly once"),
		NewReplacedLog.Notices.Num() == 1 && NewReplacedLog.Notices[0].Resource.HasSameResource(SecondResource));
	TestTrue(TEXT("unchanged subscriber receives the new Ready once"),
		LateLog.Count(EGGYGOPawnASCNoticeKind::Ready) == 2 && LateLog.Notices.Last().Resource.HasSameResource(SecondResource));
	Host->DetachAvatar(Pawn);
	return true;
}
#endif
