#include "AbilitySystem/Tests/GGYGOInputActivationOriginTestTypes.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Templates/UnrealTemplate.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOInputActivationOriginTestTypes)

UGGYGOInputActivationOriginTestAbility::UGGYGOInputActivationOriginTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UGGYGOInputActivationOriginTestAbility::ArmSameSpecRawTryForTest()
{
	Observation = FGGYGOInputActivationOriginCostObservation();
	bObserveChecks = true;
	bRawTryArmed = true;
}

void UGGYGOInputActivationOriginTestAbility::DisarmForTest()
{
	bRawTryArmed = false;
	bObserveChecks = false;
}

bool UGGYGOInputActivationOriginTestAbility::CheckCost(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayTagContainer* OptionalRelevantTags) const
{
	if (bObserveChecks)
	{
		UGGYGOAbilitySystemComponent* ASC = ActorInfo
			? Cast<UGGYGOAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get()) : nullptr;
		const FGameplayAbilitySpec* Spec = ASC ? ASC->FindAbilitySpecFromHandle(Handle) : nullptr;
		const bool bPrimaryMatches = Spec && Spec->GetPrimaryInstance() == this;
		if (bRawTryInFlight)
		{
			++Observation.InnerCostChecks;
			Observation.InnerHandle = Handle;
			Observation.InnerASC = ASC;
			Observation.bInnerPrimaryMatches = bPrimaryMatches;
		}
		else
		{
			++Observation.OuterCostChecks;
			Observation.OuterHandle = Handle;
			Observation.OuterASC = ASC;
			Observation.bOuterPrimaryMatches = bPrimaryMatches;
		}

		if (bRawTryArmed)
		{
			// Disarm before the real call; the inner CheckCost cannot recursively arm itself.
			bRawTryArmed = false;
			if (!ASC || !Handle.IsValid() || !bPrimaryMatches)
			{
				Observation.SetupFailure = FString::Printf(
					TEXT("ActivationOrigin probe %s: original ASC/Handle/PrimaryInstance invalid"), *GetPathName());
				return false;
			}
			TGuardValue<bool> RawTryGuard(bRawTryInFlight, true);
			++Observation.RawTryCalls;
			Observation.bRawTrySucceeded = ASC->TryActivateAbility(Handle);
			Observation.bRawTryReturned = true;
		}
	}
	// No fabricated rejection or failure tag: GAS costs and project group checks decide.
	return Super::CheckCost(Handle, ActorInfo, OptionalRelevantTags);
}

void UGGYGOInputActivationOriginTestAbility::NativeOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const
{
	if (bObserveChecks)
	{
		if (bRawTryInFlight) { ++Observation.InnerFailureFeedback; }
		else { ++Observation.OuterFailureFeedback; }
	}
	Super::NativeOnAbilityFailedToActivate(FailedReason);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "AbilitySystemGlobals.h"
#include "Engine/World.h"
#include "Input/Tests/GGYGOInputTestTypes.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr const TCHAR* OriginDiagnosticFlag = TEXT("GGYGOInputActivationOriginDiagnostic");
	constexpr const TCHAR* OriginDiagnosticCase = TEXT("NestedSameSpecRawTry");
	constexpr const TCHAR* LostWakeDiagnosticCase = TEXT("QueuedFailureReleasesBlockerBeforeNotice");
	enum class EOriginStage : uint8 { InitialPress, ControlRetry, NestedRetry };

	struct FOriginObservation
	{
		EOriginStage Stage = EOriginStage::InitialPress;
		int32 InitialFailures = 0;
		int32 InitialQueuedFailures = 0;
		int32 InitialNotices = 0;
		int32 ControlFailures = 0;
		int32 ControlQueuedFailures = 0;
		int32 ControlNotices = 0;
		int32 InnerFailures = 0;
		int32 InnerQueuedFailures = 0;
		int32 OuterFailures = 0;
		int32 OuterQueuedFailures = 0;
		int32 GroupFreedCount = 0;
		int32 UnexpectedFailures = 0;
		int32 UnexpectedNotices = 0;
		double InitialDeadline = -2.0;
		double ControlDeadline = -2.0;
		TArray<double> InnerDeadlines;
		TArray<double> OuterDeadlines;
	};

	// Observation is declared before this owner so captures survive every teardown path.
	struct FOriginScenario
	{
		FGGYGOInputTestFixture Fixture;
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		UGGYGOInputTestHeroComponent* Hero = nullptr;
		TStrongObjectPtr<UGGYGOAbilityAdmissionTestGroupConfig> GroupConfig;
		TWeakObjectPtr<UGGYGOInputActivationOriginTestAbility> NestedProbe;
		TArray<FGameplayAbilitySpecHandle> OwnedSpecs;
		FDelegateHandle FailureHandle;
		FDelegateHandle RetryHandle;
		FDelegateHandle GroupFreedHandle;

		~FOriginScenario()
		{
			if (UGGYGOInputActivationOriginTestAbility* Probe = NestedProbe.Get()) { Probe->DisarmForTest(); }
			if (ASC)
			{
				ASC->AbilityFailedCallbacks.Remove(FailureHandle);
				ASC->OnAbilityInputRetryable.Remove(RetryHandle);
				ASC->OnAbilityGroupFreed.Remove(GroupFreedHandle);
			}
			if (Hero) { Hero->ReleasePlayerInput(); }
			if (ASC)
			{
				for (FGameplayAbilitySpecHandle Handle : OwnedSpecs)
				{
					if (UGGYGOAbilityAdmissionQueuedTestAbility* Probe = GetProbe(Handle))
					{
						if (Probe->IsActive()) { Probe->FinishForTest(); }
					}
					ASC->ClearAbility(Handle);
				}
				ASC->SetAbilityGroupConfig(nullptr);
			}
			NestedProbe.Reset();
			GroupConfig.Reset();
			Fixture.Shutdown();
		}

		FGameplayAbilitySpecHandle GrantProbe(bool bPhysicalInput, bool bNestedProbe = false)
		{
			const TSubclassOf<UGameplayAbility> AbilityClass = bNestedProbe
				? UGGYGOInputActivationOriginTestAbility::StaticClass()
				: UGGYGOAbilityAdmissionQueuedTestAbility::StaticClass();
			FGameplayAbilitySpec Spec(AbilityClass, 1);
			if (bPhysicalInput) { Spec.GetDynamicSpecSourceTags().AddTag(GGYGOGameplayTags::InputTag_Attack_Light); }
			const FGameplayAbilitySpecHandle Handle = ASC->GiveAbility(Spec);
			if (Handle.IsValid()) { OwnedSpecs.Add(Handle); }
			return Handle;
		}

		UGGYGOAbilityAdmissionQueuedTestAbility* GetProbe(FGameplayAbilitySpecHandle Handle) const
		{
			const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(Handle);
			return Spec ? Cast<UGGYGOAbilityAdmissionQueuedTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
		}
	};

	bool RunNestedSameSpecDiagnostic(FAutomationTestBase& Test)
	{
		FOriginObservation Observation;
		FOriginScenario Scenario;
		if (!Scenario.Fixture.Initialize(Test)) { return false; }
		Scenario.ASC = Scenario.Fixture.GetPawn()->GetASCForTest();
		Scenario.Hero = Scenario.Fixture.GetPawn()->GetHeroForTest();
		UWorld* World = Scenario.Fixture.GetWorld();
		const FGameplayTag InputTag = GGYGOGameplayTags::InputTag_Attack_Light;
		const FGameplayTag GroupTag = GGYGOGameplayTags::AbilityGroup_Attack;
		if (!Test.TestFalse(TEXT("Native cost checks are enabled"), UAbilitySystemGlobals::Get().ShouldIgnoreCosts())) { return false; }
		Scenario.GroupConfig.Reset(NewObject<UGGYGOAbilityAdmissionTestGroupConfig>(Scenario.ASC));
		Scenario.GroupConfig->SetRuleForTest(GroupTag, EGGYGOAbilityGroupRule::SingleInstanceQueued);
		Scenario.ASC->SetAbilityGroupConfig(Scenario.GroupConfig.Get());
		if (!Test.TestEqual(TEXT("Explicit real Queued group rule"),
			Scenario.ASC->GetAbilityGroupConfig()->GetRuleForGroup(GroupTag).Rule,
			EGGYGOAbilityGroupRule::SingleInstanceQueued)) { return false; }

		const FGameplayAbilitySpecHandle ControlHandle = Scenario.GrantProbe(true);
		UGGYGOAbilityAdmissionQueuedTestAbility* Control = Scenario.GetProbe(ControlHandle);
		if (!Test.TestNotNull(TEXT("Binding control has a real primary instance"), Control)
			|| !Test.TestEqual(TEXT("Control real Triggered binding"), Scenario.Fixture.ExecuteAbilityBinding(ETriggerEvent::Triggered), 1)) { return false; }
		Scenario.Fixture.GetController()->ConsumeInputForTest();
		if (!Test.TestEqual(TEXT("Binding reaches ordinary GAS activation"), Control->GetBusinessActivationCountForTest(), 1)
			|| !Test.TestTrue(TEXT("Binding control is active"), Control->IsActive())
			|| !Test.TestEqual(TEXT("Control real Completed binding"), Scenario.Fixture.ExecuteAbilityBinding(ETriggerEvent::Completed), 1)) { return false; }
		Scenario.Fixture.GetController()->ConsumeInputForTest();
		Control->FinishForTest();
		Scenario.ASC->ClearAbility(ControlHandle);
		if (!Test.TestNull(TEXT("Binding control removed"), Scenario.ASC->FindAbilitySpecFromHandle(ControlHandle))
			|| !Test.TestEqual(TEXT("Control leaves group empty"), Scenario.ASC->GetActiveAbilityCountInGroup(GroupTag), 0)) { return false; }

		const FGameplayAbilitySpecHandle OccupantHandle = Scenario.GrantProbe(false);
		const FGameplayAbilitySpecHandle RequestHandle = Scenario.GrantProbe(true, true);
		UGGYGOAbilityAdmissionQueuedTestAbility* Occupant = Scenario.GetProbe(OccupantHandle);
		UGGYGOInputActivationOriginTestAbility* Request = Cast<UGGYGOInputActivationOriginTestAbility>(Scenario.GetProbe(RequestHandle));
		Scenario.NestedProbe = Request;
		if (!Test.TestNotNull(TEXT("Occupant primary instance"), Occupant)
			|| !Test.TestNotNull(TEXT("Request primary instance"), Request)
			|| !Test.TestTrue(TEXT("Request and occupant are distinct instances"), Request != Occupant)
			|| !Test.TestEqual(TEXT("Request uses native per-actor instancing"), Request->GetInstancingPolicy(), EGameplayAbilityInstancingPolicy::InstancedPerActor)
			|| !Test.TestTrue(TEXT("Occupant activates through GAS"), Scenario.ASC->TryActivateAbilityWithTerminationBoundary(OccupantHandle).bNativeAccepted)
			|| !Test.TestTrue(TEXT("Initial occupant is really active"), Occupant->IsActive())
			|| !Test.TestEqual(TEXT("Initial setup has exactly one real group occupant"), Scenario.ASC->GetActiveAbilityCountInGroup(GroupTag), 1)) { return false; }
		EGGYGOAbilityGroupBlockReason BlockReason = EGGYGOAbilityGroupBlockReason::NotBlocked;
		if (!Test.TestTrue(TEXT("Request blocked by real occupancy"), Scenario.ASC->IsActivationBlockedByGroup(Request, BlockReason))
			|| !Test.TestEqual(TEXT("Real rejection is Queued"), BlockReason, EGGYGOAbilityGroupBlockReason::GroupOccupiedQueued)) { return false; }

		Scenario.FailureHandle = Scenario.ASC->AbilityFailedCallbacks.AddLambda(
			[&Observation, Request](const UGameplayAbility* Failed, const FGameplayTagContainer& Reasons)
			{
				if (Failed != Request) { ++Observation.UnexpectedFailures; return; }
				const bool bQueued = Reasons.HasTagExact(GGYGOGameplayTags::Ability_ActivateFail_ActivationGroupQueued);
				if (Observation.Stage == EOriginStage::InitialPress)
				{
					++Observation.InitialFailures;
					if (bQueued) { ++Observation.InitialQueuedFailures; }
				}
				else if (Observation.Stage == EOriginStage::ControlRetry)
				{
					++Observation.ControlFailures;
					if (bQueued) { ++Observation.ControlQueuedFailures; }
				}
				else if (Request->IsRawTryInFlightForTest())
				{
					++Observation.InnerFailures;
					if (bQueued) { ++Observation.InnerQueuedFailures; }
				}
				else
				{
					++Observation.OuterFailures;
					if (bQueued) { ++Observation.OuterQueuedFailures; }
				}
			});
		Scenario.RetryHandle = Scenario.ASC->OnAbilityInputRetryable.AddLambda(
			[&Observation, Request, InputTag](const FGGYGOAbilityInputRetryRequest& OriginalRequest)
			{
				const FGameplayTag Tag = OriginalRequest.InputTag;
				const double Deadline = OriginalRequest.OriginalDeadline;
				if (Tag != InputTag) { ++Observation.UnexpectedNotices; return; }
				if (Observation.Stage == EOriginStage::InitialPress)
				{
					++Observation.InitialNotices;
					Observation.InitialDeadline = Deadline;
				}
				else if (Observation.Stage == EOriginStage::ControlRetry)
				{
					++Observation.ControlNotices;
					Observation.ControlDeadline = Deadline;
				}
				else if (Request->IsRawTryInFlightForTest()) { Observation.InnerDeadlines.Add(Deadline); }
				else { Observation.OuterDeadlines.Add(Deadline); }
			});
		Scenario.GroupFreedHandle = Scenario.ASC->OnAbilityGroupFreed.AddLambda(
			[&Observation, GroupTag](FGameplayTag Freed)
			{
				if (Freed == GroupTag) { ++Observation.GroupFreedCount; }
			});

		const double OriginalTime = World->GetTimeSeconds();
		if (!Test.TestEqual(TEXT("Initial real Triggered binding"), Scenario.Fixture.ExecuteAbilityBinding(ETriggerEvent::Triggered), 1)) { return false; }
		Scenario.Fixture.GetController()->ConsumeInputForTest();
		if (!Test.TestEqual(TEXT("Initial native failure"), Observation.InitialFailures, 1)
			|| !Test.TestEqual(TEXT("Initial failure is real Queued"), Observation.InitialQueuedFailures, 1)
			|| !Test.TestEqual(TEXT("Initial physical failure notice"), Observation.InitialNotices, 1)
			|| !Test.TestTrue(TEXT("Initial notice carries the first finite deadline"),
				FMath::IsFinite(Observation.InitialDeadline) && Observation.InitialDeadline > OriginalTime)
			|| !Test.TestEqual(TEXT("Queued request has no business activation"), Request->GetBusinessActivationCountForTest(), 0)) { return false; }

		// Unarmed real retry exposes the original finite deadline publicly, before nesting.
		Occupant->FinishForTest();
		if (!Test.TestEqual(TEXT("Control retry has real GroupFreed"), Observation.GroupFreedCount, 1)
			|| !Test.TestTrue(TEXT("Occupant restores real rejection for control retry"), Scenario.ASC->TryActivateAbilityWithTerminationBoundary(OccupantHandle).bNativeAccepted)
			|| !Test.TestTrue(TEXT("Control restart occupant is really active"), Occupant->IsActive())
			|| !Test.TestEqual(TEXT("Control restart has exactly one real group occupant"), Scenario.ASC->GetActiveAbilityCountInGroup(GroupTag), 1)) { return false; }
		Observation.Stage = EOriginStage::ControlRetry;
		Scenario.Fixture.GetController()->ConsumeInputForTest();
		if (!Test.TestEqual(TEXT("Unarmed retry native failure"), Observation.ControlFailures, 1)
			|| !Test.TestEqual(TEXT("Unarmed retry is real Queued"), Observation.ControlQueuedFailures, 1)
			|| !Test.TestEqual(TEXT("Unarmed retry has one legitimate notice"), Observation.ControlNotices, 1)
			|| !Test.TestTrue(TEXT("Original retry deadline is finite and live"),
				FMath::IsFinite(Observation.ControlDeadline) && Observation.ControlDeadline > OriginalTime
				&& Observation.ControlDeadline == Observation.InitialDeadline)
			|| !Test.TestEqual(TEXT("Unarmed phase has no injected raw calls"), Request->GetCostObservationForTest().RawTryCalls, 0)
			|| !Test.TestEqual(TEXT("No unrelated control failures"), Observation.UnexpectedFailures, 0)
			|| !Test.TestEqual(TEXT("No unrelated control notices"), Observation.UnexpectedNotices, 0)) { return false; }

		Occupant->FinishForTest();
		if (!Test.TestEqual(TEXT("Nested retry has second real GroupFreed"), Observation.GroupFreedCount, 2)
			|| !Test.TestTrue(TEXT("Occupant restores real rejection before arming"), Scenario.ASC->TryActivateAbilityWithTerminationBoundary(OccupantHandle).bNativeAccepted)
			|| !Test.TestTrue(TEXT("Nested restart occupant is really active"), Occupant->IsActive())
			|| !Test.TestEqual(TEXT("Exactly one occupant before nested retry"), Scenario.ASC->GetActiveAbilityCountInGroup(GroupTag), 1)) { return false; }
		Observation.Stage = EOriginStage::NestedRetry;
		Request->ArmSameSpecRawTryForTest();
		Scenario.Fixture.GetController()->ConsumeInputForTest();
		const FGGYGOInputActivationOriginCostObservation& Cost = Request->GetCostObservationForTest();
		if (!Test.TestTrue(TEXT("Probe setup valid: ") + Cost.SetupFailure, Cost.SetupFailure.IsEmpty())
			|| !Test.TestEqual(TEXT("One armed outer CheckCost"), Cost.OuterCostChecks, 1)
			|| !Test.TestEqual(TEXT("One real inner CheckCost"), Cost.InnerCostChecks, 1)
			|| !Test.TestEqual(TEXT("One raw GAS TryActivateAbility call"), Cost.RawTryCalls, 1)
			|| !Test.TestTrue(TEXT("Raw GAS call returned"), Cost.bRawTryReturned)
			|| !Test.TestFalse(TEXT("Raw GAS call rejected"), Cost.bRawTrySucceeded)
			|| !Test.TestTrue(TEXT("Outer uses request Handle"), Cost.OuterHandle == RequestHandle)
			|| !Test.TestTrue(TEXT("Inner uses exactly the same Handle"), Cost.InnerHandle == RequestHandle)
			|| !Test.TestTrue(TEXT("Outer uses original ASC"), Cost.OuterASC.Get() == Scenario.ASC)
			|| !Test.TestTrue(TEXT("Inner uses original ASC"), Cost.InnerASC.Get() == Scenario.ASC)
			|| !Test.TestTrue(TEXT("Outer resolves the same primary instance"), Cost.bOuterPrimaryMatches)
			|| !Test.TestTrue(TEXT("Inner resolves the same primary instance"), Cost.bInnerPrimaryMatches)
			|| !Test.TestEqual(TEXT("Inner native GAS failure preserved"), Observation.InnerFailures, 1)
			|| !Test.TestEqual(TEXT("Inner native failure is Queued"), Observation.InnerQueuedFailures, 1)
			|| !Test.TestEqual(TEXT("Outer native GAS failure preserved"), Observation.OuterFailures, 1)
			|| !Test.TestEqual(TEXT("Outer native failure is Queued"), Observation.OuterQueuedFailures, 1)
			|| !Test.TestEqual(TEXT("Inner GA failure feedback preserved"), Cost.InnerFailureFeedback, 1)
			|| !Test.TestEqual(TEXT("Outer GA failure feedback preserved"), Cost.OuterFailureFeedback, 1)) { return false; }

		Test.AddInfo(FString::Printf(TEXT("ActivationOrigin evidence: handle=%s rawCalls=%d innerNative=%d outerNative=%d innerRetry=%d outerRetry=%d originalDeadline=%.17g"),
			*RequestHandle.ToString(), Cost.RawTryCalls, Observation.InnerFailures, Observation.OuterFailures,
			Observation.InnerDeadlines.Num(), Observation.OuterDeadlines.Num(), Observation.ControlDeadline));
		for (double Deadline : Observation.InnerDeadlines)
		{
			Test.AddInfo(FString::Printf(TEXT("Observed inner raw notice deadline=%.17g originalDeadline=%.17g"),
				Deadline, Observation.ControlDeadline));
		}
		bool bResult = Test.TestEqual(TEXT("Inner raw failure must not borrow outer input retry source"), Observation.InnerDeadlines.Num(), 0);
		bResult &= Test.TestEqual(TEXT("Legitimate outer retry keeps one notice"), Observation.OuterDeadlines.Num(), 1);
		for (double Deadline : Observation.OuterDeadlines)
		{
			bResult &= Test.TestTrue(TEXT("Outer retry keeps exactly the original finite deadline"),
				FMath::IsFinite(Deadline) && Deadline == Observation.ControlDeadline && Deadline > World->GetTimeSeconds());
		}
		bResult &= Test.TestEqual(TEXT("Request business activation remains zero"), Request->GetBusinessActivationCountForTest(), 0);
		bResult &= Test.TestFalse(TEXT("Rejected request remains inactive"), Request->IsActive());
		bResult &= Test.TestEqual(TEXT("Real occupancy remains unchanged"), Scenario.ASC->GetActiveAbilityCountInGroup(GroupTag), 1);
		bResult &= Test.TestTrue(TEXT("World has not advanced during the diagnostic"), static_cast<double>(World->GetTimeSeconds()) == OriginalTime);
		bResult &= Test.TestFalse(TEXT("Fixture has not begun play"), World->HasBegunPlay());
		bResult &= Test.TestEqual(TEXT("No unrelated native failures"), Observation.UnexpectedFailures, 0);
		bResult &= Test.TestEqual(TEXT("No unrelated input retry notices"), Observation.UnexpectedNotices, 0);
		return bResult;
	}

	bool RunQueuedFailureLostWakeDiagnostic(FAutomationTestBase& Test)
	{
		struct FLostWakeObservation
		{
			int32 QueuedFailures = 0;
			int32 UnexpectedFailures = 0;
			int32 CompletedBindings = 0;
			int32 GroupFreed = 0;
			int32 RetryNotices = 0;
			int32 NoticesAtGroupFreed = INDEX_NONE;
			int32 ActivationsAtGroupFreed = INDEX_NONE;
			bool bInsideFailure = false;
			bool bGroupFreedInsideFailure = false;
			FGGYGOAbilityInputRetryRequest OriginalRequest;
		} Observation;
		FOriginScenario Scenario;
		if (!Scenario.Fixture.Initialize(Test)) { return false; }
		Scenario.ASC = Scenario.Fixture.GetPawn()->GetASCForTest();
		Scenario.Hero = Scenario.Fixture.GetPawn()->GetHeroForTest();
		UWorld* World = Scenario.Fixture.GetWorld();
		const double OriginalTime = World->GetTimeSeconds();
		const FGameplayTag GroupTag = GGYGOGameplayTags::AbilityGroup_Attack;
		Scenario.GroupConfig.Reset(NewObject<UGGYGOAbilityAdmissionTestGroupConfig>(Scenario.ASC));
		Scenario.GroupConfig->SetRuleForTest(GroupTag, EGGYGOAbilityGroupRule::SingleInstanceQueued);
		Scenario.ASC->SetAbilityGroupConfig(Scenario.GroupConfig.Get());
		const FGameplayAbilitySpecHandle OccupantHandle = Scenario.GrantProbe(false);
		const FGameplayAbilitySpecHandle RequestHandle = Scenario.GrantProbe(true);
		UGGYGOAbilityAdmissionQueuedTestAbility* Occupant = Scenario.GetProbe(OccupantHandle);
		UGGYGOAbilityAdmissionQueuedTestAbility* Request = Scenario.GetProbe(RequestHandle);
		if (!Test.TestNotNull(TEXT("Lost-wake occupant has a real primary instance"), Occupant)
			|| !Test.TestNotNull(TEXT("Lost-wake request has a real primary instance"), Request)
			|| !Test.TestEqual(TEXT("Finite tap has no ordinary WhileHeld self-retry"),
				Request->GetActivationPolicy(), EGGYGOAbilityActivationPolicy::OnInputTriggered)
			|| !Test.TestTrue(TEXT("Lost-wake occupant activates through native GAS"),
				Scenario.ASC->TryActivateAbilityWithTerminationBoundary(OccupantHandle).bNativeAccepted)
			|| !Test.TestTrue(TEXT("Lost-wake occupant is really active"), Occupant->IsActive())
			|| !Test.TestEqual(TEXT("Lost-wake setup has exactly one real group occupant"), Scenario.ASC->GetActiveAbilityCountInGroup(GroupTag), 1)) { return false; }

		Scenario.FailureHandle = Scenario.ASC->AbilityFailedCallbacks.AddLambda(
			[&Observation, &Scenario, Occupant, Request](const UGameplayAbility* Failed,
				const FGameplayTagContainer& Reasons)
			{
				if (Failed != Request || !Reasons.HasTagExact(GGYGOGameplayTags::Ability_ActivateFail_ActivationGroupQueued))
				{
					++Observation.UnexpectedFailures;
					return;
				}
				++Observation.QueuedFailures;
				Observation.bInsideFailure = true;
				// Actual Action release and native occupant End occur before the project's retry notice.
				Observation.CompletedBindings = Scenario.Fixture.ExecuteAbilityBinding(ETriggerEvent::Completed);
				Occupant->FinishForTest();
				Observation.bInsideFailure = false;
			});
		Scenario.GroupFreedHandle = Scenario.ASC->OnAbilityGroupFreed.AddLambda(
			[&Observation, Request, GroupTag](FGameplayTag Freed)
			{
				if (Freed != GroupTag) { return; }
				++Observation.GroupFreed;
				Observation.bGroupFreedInsideFailure |= Observation.bInsideFailure;
				Observation.NoticesAtGroupFreed = Observation.RetryNotices;
				Observation.ActivationsAtGroupFreed = Request->GetBusinessActivationCountForTest();
			});
		Scenario.RetryHandle = Scenario.ASC->OnAbilityInputRetryable.AddLambda(
			[&Observation](const FGGYGOAbilityInputRetryRequest& Original)
			{
				++Observation.RetryNotices;
				Observation.OriginalRequest = Original;
			});

		if (!Test.TestEqual(TEXT("Lost-wake uses the real Triggered Action binding"),
			Scenario.Fixture.ExecuteAbilityBinding(ETriggerEvent::Triggered), 1)) { return false; }
		Scenario.Fixture.GetController()->ConsumeInputForTest();
		if (!Test.TestEqual(TEXT("Exactly one real Queued failure"), Observation.QueuedFailures, 1)
			|| !Test.TestEqual(TEXT("Failure callback uses the real Completed binding"), Observation.CompletedBindings, 1)
			|| !Test.TestEqual(TEXT("Failure callback produces the original group release"), Observation.GroupFreed, 1)
			|| !Test.TestTrue(TEXT("GroupFreed really occurs on the native failure callback stack"), Observation.bGroupFreedInsideFailure)
			|| !Test.TestEqual(TEXT("GroupFreed precedes the retry observation"), Observation.NoticesAtGroupFreed, 0)
			|| !Test.TestEqual(TEXT("GroupFreed never activates on its broadcast stack"), Observation.ActivationsAtGroupFreed, 0)
			|| !Test.TestEqual(TEXT("One post-Super retry observation"), Observation.RetryNotices, 1)
			|| !Test.TestTrue(TEXT("Retry observation retains the accepted original ID"), Observation.OriginalRequest.Identity.IsAssigned())
			|| !Test.TestTrue(TEXT("Retry observation uses the original ASC"),
				Observation.OriginalRequest.Identity.SourceASC.Get() == Scenario.ASC)
			|| !Test.TestTrue(TEXT("Retry observation carries the original finite absolute deadline"),
				FMath::IsFinite(Observation.OriginalRequest.OriginalDeadline)
				&& Observation.OriginalRequest.OriginalDeadline > OriginalTime)
			|| !Test.TestEqual(TEXT("Initial failed Process has no business activation"),
				Request->GetBusinessActivationCountForTest(), 0)
			|| !Test.TestEqual(TEXT("No fabricated or unrelated failure"), Observation.UnexpectedFailures, 0)) { return false; }

		// No manual Queue, new press, fabricated failure or second group release wakes this request.
		Scenario.Fixture.GetController()->ConsumeInputForTest();
		if (!Test.TestEqual(TEXT("Next existing Process consumes the released tap once"),
				Request->GetBusinessActivationCountForTest(), 1)
			|| !Test.TestTrue(TEXT("Original request is now active"), Request->IsActive())
			|| !Test.TestEqual(TEXT("Wake did not manufacture a second failure"), Observation.QueuedFailures, 1)
			|| !Test.TestEqual(TEXT("Wake did not manufacture a second retry observation"), Observation.RetryNotices, 1)
			|| !Test.TestTrue(TEXT("No deadline clock or world tick was added"),
				static_cast<double>(World->GetTimeSeconds()) == OriginalTime)) { return false; }
		Request->FinishForTest();
		Scenario.Fixture.GetController()->ConsumeInputForTest();
		return Test.TestEqual(TEXT("Consumed permission cannot replay the ended tap on a later group release"),
			Request->GetBusinessActivationCountForTest(), 1);
	}

}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FGGYGOInputActivationOriginDiagnostic,
	"ProjectDiagnostics.Input.ActivationOrigin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FGGYGOInputActivationOriginDiagnostic::GetTests(TArray<FString>& OutBeautifiedNames,
	TArray<FString>& OutTestCommands) const
{
	if (FParse::Param(FCommandLine::Get(), OriginDiagnosticFlag))
	{
		OutBeautifiedNames.Add(OriginDiagnosticCase);
		OutTestCommands.Add(OriginDiagnosticCase);
		OutBeautifiedNames.Add(LostWakeDiagnosticCase);
		OutTestCommands.Add(LostWakeDiagnosticCase);
	}
}

bool FGGYGOInputActivationOriginDiagnostic::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Explicit activation origin diagnostic launch flag"), FParse::Param(FCommandLine::Get(), OriginDiagnosticFlag))) { return false; }
	if (Parameters == LostWakeDiagnosticCase) { return RunQueuedFailureLostWakeDiagnostic(*this); }
	if (!TestEqual(TEXT("Explicit activation origin diagnostic case"), Parameters, FString(OriginDiagnosticCase))) { return false; }
	return RunNestedSameSpecDiagnostic(*this);
}
#endif // WITH_DEV_AUTOMATION_TESTS
