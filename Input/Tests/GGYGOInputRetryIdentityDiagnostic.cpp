#include "Input/Tests/GGYGOInputTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/Tests/GGYGOAbilityAdmissionTestTypes.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr const TCHAR* IdentityDiagnosticFlag = TEXT("GGYGOInputRetryIdentityDiagnostic");
	constexpr const TCHAR* IdentityDiagnosticCase = TEXT("SameTimeReentrantPhysicalRequest");

	enum class EIdentityStage : uint8 { InitialPress, OldRetry, NewPressConsumption };

	struct FIdentityObservation
	{
		EIdentityStage Stage = EIdentityStage::InitialPress;
		const UGameplayAbility* LastFailedInstance = nullptr;
		UGGYGOAbilityAdmissionQueuedTestAbility* LateObserver = nullptr;
		int32 InitialQueuedFailures = 0;
		int32 InitialRetryNotices = 0;
		int32 OldQueuedFailures = 0;
		int32 OldRetryNotices = 0;
		int32 ReentryCount = 0;
		int32 ReleaseBindings = 0;
		int32 PressBindings = 0;
		int32 NewPressFailures = 0;
		int32 LateFailures = 0;
		int32 LateQueuedFailures = 0;
		int32 GroupFreedCount = 0;
		int32 UnexpectedNotices = 0;
		bool bReentryArmed = false;
		double InitialNoticeDeadline = -2.0;
		double OldDeadline = -2.0;
		double RepressTime = -1.0;
		TArray<double> LateDeadlines;
	};

	// Declared after the observation: captured observation storage outlives teardown.
	struct FIdentityScenario
	{
		FGGYGOInputTestFixture Fixture;
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		UGGYGOInputTestHeroComponent* Hero = nullptr;
		TStrongObjectPtr<UGGYGOAbilityAdmissionTestGroupConfig> GroupConfig;
		TArray<FGameplayAbilitySpecHandle> OwnedSpecs;
		FDelegateHandle FailureHandle;
		FDelegateHandle RetryHandle;
		FDelegateHandle GroupFreedHandle;

		~FIdentityScenario()
		{
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
			GroupConfig.Reset();
			Fixture.Shutdown();
		}

		FGameplayAbilitySpecHandle GrantProbe(bool bPhysicalInput)
		{
			FGameplayAbilitySpec Spec(UGGYGOAbilityAdmissionQueuedTestAbility::StaticClass(), 1);
			if (bPhysicalInput)
			{
				Spec.GetDynamicSpecSourceTags().AddTag(GGYGOGameplayTags::InputTag_Attack_Light);
			}
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

	bool RunSameTimeIdentityDiagnostic(FAutomationTestBase& Test)
	{
		FIdentityObservation Observation;
		const FGameplayTag InputTag = GGYGOGameplayTags::InputTag_Attack_Light;
		const FGameplayTag GroupTag = GGYGOGameplayTags::AbilityGroup_Attack;
		FIdentityScenario Scenario;
		if (!Scenario.Fixture.Initialize(Test)) { return false; }
		Scenario.ASC = Scenario.Fixture.GetPawn()->GetASCForTest();
		Scenario.Hero = Scenario.Fixture.GetPawn()->GetHeroForTest();
		UWorld* World = Scenario.Fixture.GetWorld();
		Scenario.GroupConfig.Reset(NewObject<UGGYGOAbilityAdmissionTestGroupConfig>(Scenario.ASC));
		Scenario.GroupConfig->SetRuleForTest(GroupTag, EGGYGOAbilityGroupRule::SingleInstanceQueued);
		Scenario.ASC->SetAbilityGroupConfig(Scenario.GroupConfig.Get());
		if (!Test.TestEqual(TEXT("Real Queued group config"),
			Scenario.ASC->GetAbilityGroupConfig()->GetRuleForGroup(GroupTag).Rule,
			EGGYGOAbilityGroupRule::SingleInstanceQueued)) { return false; }

		// Positive control uses the same existing probe class and real binding/PC path.
		const FGameplayAbilitySpecHandle ControlHandle = Scenario.GrantProbe(true);
		UGGYGOAbilityAdmissionQueuedTestAbility* Control = Scenario.GetProbe(ControlHandle);
		if (!Test.TestNotNull(TEXT("Control has a real GAS primary instance"), Control)
			|| !Test.TestEqual(TEXT("Control Triggered binding"),
				Scenario.Fixture.ExecuteAbilityBinding(ETriggerEvent::Triggered), 1)) { return false; }
		Scenario.Fixture.GetController()->ConsumeInputForTest();
		if (!Test.TestEqual(TEXT("Control binding reaches ordinary activation"),
			Control->GetBusinessActivationCountForTest(), 1)
			|| !Test.TestTrue(TEXT("Control is active"), Control->IsActive())
			|| !Test.TestEqual(TEXT("Control Completed binding"),
				Scenario.Fixture.ExecuteAbilityBinding(ETriggerEvent::Completed), 1)) { return false; }
		Scenario.Fixture.GetController()->ConsumeInputForTest();
		Control->FinishForTest();
		Scenario.ASC->ClearAbility(ControlHandle);
		if (!Test.TestEqual(TEXT("Control leaves group empty"),
			Scenario.ASC->GetActiveAbilityCountInGroup(GroupTag), 0)
			|| !Test.TestNull(TEXT("Control Spec removed"),
				Scenario.ASC->FindAbilitySpecFromHandle(ControlHandle))) { return false; }

		const FGameplayAbilitySpecHandle OccupantHandle = Scenario.GrantProbe(false);
		const FGameplayAbilitySpecHandle OriginalHandle = Scenario.GrantProbe(true);
		UGGYGOAbilityAdmissionQueuedTestAbility* Occupant = Scenario.GetProbe(OccupantHandle);
		UGGYGOAbilityAdmissionQueuedTestAbility* Original = Scenario.GetProbe(OriginalHandle);
		if (!Test.TestNotNull(TEXT("Occupant primary instance"), Occupant)
			|| !Test.TestNotNull(TEXT("Original primary instance"), Original)
			|| !Test.TestTrue(TEXT("Specs have distinct real instances"), Occupant != Original)) { return false; }
		const FGGYGOAbilityActivationRequestResult OccupantActivation =
			Scenario.ASC->TryActivateAbilityWithTerminationBoundary(OccupantHandle);
		if (!Test.TestTrue(TEXT("Occupant activates through GAS"), OccupantActivation.bNativeAccepted)
			|| !Test.TestTrue(TEXT("Occupant is locally active"), Occupant->IsActive())
			|| !Test.TestEqual(TEXT("Exactly one real occupant after initial activation"),
				Scenario.ASC->GetActiveAbilityCountInGroup(GroupTag), 1)) { return false; }
		EGGYGOAbilityGroupBlockReason BlockReason = EGGYGOAbilityGroupBlockReason::NotBlocked;
		if (!Test.TestTrue(TEXT("Original is blocked by real group occupancy"),
			Scenario.ASC->IsActivationBlockedByGroup(Original, BlockReason))
			|| !Test.TestEqual(TEXT("Real Queued rejection reason"), BlockReason,
				EGGYGOAbilityGroupBlockReason::GroupOccupiedQueued)) { return false; }

		Scenario.FailureHandle = Scenario.ASC->AbilityFailedCallbacks.AddLambda(
			[&Observation, &Scenario, Original](const UGameplayAbility* Failed, const FGameplayTagContainer& Reasons)
			{
				Observation.LastFailedInstance = Failed;
				const bool bQueued = Reasons.HasTagExact(GGYGOGameplayTags::Ability_ActivateFail_ActivationGroupQueued);
				if (Failed == Original)
				{
					if (Observation.Stage == EIdentityStage::InitialPress && bQueued)
					{
						++Observation.InitialQueuedFailures;
					}
					else if (Observation.Stage == EIdentityStage::OldRetry && bQueued)
					{
						++Observation.OldQueuedFailures;
						if (Observation.bReentryArmed)
						{
							Observation.bReentryArmed = false;
							++Observation.ReentryCount;
							Observation.ReleaseBindings = Scenario.Fixture.ExecuteAbilityBinding(ETriggerEvent::Completed);
							Observation.PressBindings = Scenario.Fixture.ExecuteAbilityBinding(ETriggerEvent::Triggered);
							Observation.RepressTime = Scenario.Fixture.GetWorld()->GetTimeSeconds();
						}
					}
					else if (Observation.Stage == EIdentityStage::NewPressConsumption)
					{
						++Observation.NewPressFailures;
					}
				}
				else if (Observation.LateObserver && Failed == Observation.LateObserver)
				{
					++Observation.LateFailures;
					if (bQueued) { ++Observation.LateQueuedFailures; }
				}
			});
		Scenario.RetryHandle = Scenario.ASC->OnAbilityInputRetryable.AddLambda(
			[&Observation, Original, InputTag](const FGGYGOAbilityInputRetryRequest& OriginalRequest)
			{
				const FGameplayTag Tag = OriginalRequest.InputTag;
				const double Deadline = OriginalRequest.OriginalDeadline;
				if (Tag != InputTag) { ++Observation.UnexpectedNotices; return; }
				if (Observation.Stage == EIdentityStage::InitialPress && Observation.LastFailedInstance == Original)
				{
					++Observation.InitialRetryNotices;
					Observation.InitialNoticeDeadline = Deadline;
				}
				else if (Observation.Stage == EIdentityStage::OldRetry && Observation.LastFailedInstance == Original)
				{
					++Observation.OldRetryNotices;
					Observation.OldDeadline = Deadline;
				}
				else if (Observation.Stage == EIdentityStage::NewPressConsumption
					&& Observation.LateObserver && Observation.LastFailedInstance == Observation.LateObserver)
				{
					Observation.LateDeadlines.Add(Deadline);
				}
				else { ++Observation.UnexpectedNotices; }
			});
		Scenario.GroupFreedHandle = Scenario.ASC->OnAbilityGroupFreed.AddLambda(
			[&Observation, GroupTag](FGameplayTag Freed)
			{
				if (Freed == GroupTag) { ++Observation.GroupFreedCount; }
			});

		const double OldPressTime = World->GetTimeSeconds();
		if (!Test.TestEqual(TEXT("Initial Triggered binding"),
			Scenario.Fixture.ExecuteAbilityBinding(ETriggerEvent::Triggered), 1)) { return false; }
		Scenario.Fixture.GetController()->ConsumeInputForTest();
		if (!Test.TestEqual(TEXT("Initial actual Queued failure"), Observation.InitialQueuedFailures, 1)
			|| !Test.TestEqual(TEXT("Initial physical failure notice"), Observation.InitialRetryNotices, 1)
			|| !Test.TestTrue(TEXT("Initial failure retains its first finite observation deadline"),
				FMath::IsFinite(Observation.InitialNoticeDeadline) && Observation.InitialNoticeDeadline > OldPressTime)
			|| !Test.TestEqual(TEXT("Initial deadline is exactly the first observation window"), Observation.InitialNoticeDeadline,
				OldPressTime + static_cast<double>(Scenario.Hero->InputBufferWindow))
			|| !Test.TestEqual(TEXT("Queued rejection does not activate Original"),
				Original->GetBusinessActivationCountForTest(), 0)) { return false; }

		// Only the real empty-group notification wakes ASC's original waiting request.
		Occupant->FinishForTest();
		if (!Test.TestEqual(TEXT("Initial real GroupFreed"), Observation.GroupFreedCount, 1)) { return false; }
		const FGGYGOAbilityActivationRequestResult OccupantReactivation =
			Scenario.ASC->TryActivateAbilityWithTerminationBoundary(OccupantHandle);
		if (!Test.TestTrue(TEXT("Occupant reactivates before queued retry consumption"), OccupantReactivation.bNativeAccepted)
			|| !Test.TestTrue(TEXT("Reactivated occupant is locally active"), Occupant->IsActive())
			|| !Test.TestEqual(TEXT("Exactly one real occupant before old retry"),
				Scenario.ASC->GetActiveAbilityCountInGroup(GroupTag), 1)) { return false; }
		Observation.Stage = EIdentityStage::OldRetry;
		Observation.bReentryArmed = true;
		Scenario.Fixture.GetController()->ConsumeInputForTest();
		if (!Test.TestEqual(TEXT("Old queued retry reaches real GAS failure"), Observation.OldQueuedFailures, 1)
			|| !Test.TestEqual(TEXT("Super failure callback reentry occurs once"), Observation.ReentryCount, 1)
			|| !Test.TestEqual(TEXT("Reentrant Completed uses one real binding"), Observation.ReleaseBindings, 1)
			|| !Test.TestEqual(TEXT("Reentrant Triggered uses one real binding"), Observation.PressBindings, 1)
			|| !Test.TestTrue(TEXT("Reentrant new press has exactly the old World time"), Observation.RepressTime == OldPressTime)
			|| !Test.TestTrue(TEXT("World was not ticked during failure notification"),
				static_cast<double>(World->GetTimeSeconds()) == OldPressTime)
			|| !Test.TestEqual(TEXT("Old failure publishes one explicit retry deadline"), Observation.OldRetryNotices, 1)
			|| !Test.TestTrue(TEXT("Original deadline is finite and not expired"),
				FMath::IsFinite(Observation.OldDeadline) && Observation.OldDeadline > OldPressTime)
			|| !Test.TestEqual(TEXT("Old retry keeps the exact first deadline"), Observation.OldDeadline, Observation.InitialNoticeDeadline)
			|| !Test.TestEqual(TEXT("New press has not been consumed in the old retry snapshot"),
				Original->GetBusinessActivationCountForTest(), 0)
			|| !Test.TestEqual(TEXT("No unrelated retry notices"), Observation.UnexpectedNotices, 0)) { return false; }
		// Same Hero, unchanged window, no time advance: the frozen Now + Window rule
		// yields the same deadline. Observe the old value publicly, never inspect buffers.
		const double UnchangedWindow = Observation.OldDeadline - OldPressTime;
		if (!Test.TestTrue(TEXT("Same-time new physical request has the same derived deadline"),
			Observation.RepressTime + UnchangedWindow == Observation.OldDeadline)) { return false; }

		// This Spec did not exist when the reentrant press populated physical handles.
		const FGameplayAbilitySpecHandle LateHandle = Scenario.GrantProbe(true);
		Observation.LateObserver = Scenario.GetProbe(LateHandle);
		if (!Test.TestNotNull(TEXT("Late observer has its own primary instance"), Observation.LateObserver)
			|| !Test.TestTrue(TEXT("Late observer is distinct from Original and Occupant"),
				Observation.LateObserver != Original && Observation.LateObserver != Occupant)
			|| !Test.TestEqual(TEXT("Late observer has never activated"),
				Observation.LateObserver->GetBusinessActivationCountForTest(), 0)) { return false; }
		Observation.Stage = EIdentityStage::NewPressConsumption;
		Occupant->FinishForTest();
		if (!Test.TestEqual(TEXT("Second real GroupFreed"), Observation.GroupFreedCount, 2)
			|| !Test.TestEqual(TEXT("Group is empty before new press consumption"),
				Scenario.ASC->GetActiveAbilityCountInGroup(GroupTag), 0)) { return false; }
		Scenario.Fixture.GetController()->ConsumeInputForTest();

		bool bResult = Test.TestEqual(TEXT("Legitimate new press activates Original once"),
			Original->GetBusinessActivationCountForTest(), 1);
		bResult &= Test.TestTrue(TEXT("Legitimate new request owns the group"), Original->IsActive());
		bResult &= Test.TestEqual(TEXT("Legitimate new press did not fail"), Observation.NewPressFailures, 0);
		bResult &= Test.TestTrue(TEXT("World time remains exactly equal"),
			static_cast<double>(World->GetTimeSeconds()) == OldPressTime);
		bResult &= Test.TestFalse(TEXT("Fixture still has not begun play"), World->HasBegunPlay());
		bResult &= Test.TestEqual(TEXT("No unrelated retry notification was substituted"), Observation.UnexpectedNotices, 0);
		if (Observation.LateFailures > 0 || !Observation.LateDeadlines.IsEmpty())
		{
			// Evidence checks do not invert the strict zero expectations below.
			bResult &= Test.TestEqual(TEXT("Observed late failure is the real Queued rejection"),
				Observation.LateQueuedFailures, Observation.LateFailures);
			bResult &= Test.TestEqual(TEXT("Each late failure has one public retry notice"),
				Observation.LateDeadlines.Num(), Observation.LateFailures);
			for (double Deadline : Observation.LateDeadlines)
			{
				bResult &= Test.TestTrue(TEXT("Observed late notification carries the old explicit deadline"),
					Deadline == Observation.OldDeadline);
			}
		}
		Test.AddInfo(FString::Printf(TEXT("Identity evidence: oldTime=%.17g newTime=%.17g oldDeadline=%.17g reentry=%d lateFailures=%d lateNotices=%d"),
			OldPressTime, Observation.RepressTime, Observation.OldDeadline, Observation.ReentryCount,
			Observation.LateFailures, Observation.LateDeadlines.Num()));
		bResult &= Test.TestEqual(TEXT("Old retry failure must not attempt a Spec absent from the new physical press"),
			Observation.LateFailures, 0);
		bResult &= Test.TestEqual(TEXT("Old retry failure must not publish a late Spec retry deadline"),
			Observation.LateDeadlines.Num(), 0);
		bResult &= Test.TestEqual(TEXT("Late observer receives no activation"),
			Observation.LateObserver->GetBusinessActivationCountForTest(), 0);
		return bResult;
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FGGYGOInputRetryIdentityDiagnostic,
	"ProjectDiagnostics.Input.RetryIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FGGYGOInputRetryIdentityDiagnostic::GetTests(TArray<FString>& OutBeautifiedNames,
	TArray<FString>& OutTestCommands) const
{
	if (FParse::Param(FCommandLine::Get(), IdentityDiagnosticFlag))
	{
		OutBeautifiedNames.Add(IdentityDiagnosticCase);
		OutTestCommands.Add(IdentityDiagnosticCase);
	}
}

bool FGGYGOInputRetryIdentityDiagnostic::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Explicit identity diagnostic launch flag"),
		FParse::Param(FCommandLine::Get(), IdentityDiagnosticFlag))
		|| !TestEqual(TEXT("Explicit identity diagnostic case"), Parameters,
			FString(IdentityDiagnosticCase))) { return false; }
	return RunSameTimeIdentityDiagnostic(*this);
}
#endif // WITH_DEV_AUTOMATION_TESTS
