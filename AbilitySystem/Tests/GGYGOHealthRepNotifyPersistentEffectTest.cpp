#include "AbilitySystem/Tests/GGYGOHealthRepNotifyPersistentEffectTestTypes.h"

#include "GameplayEffectExtension.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHealthRepNotifyPersistentEffectTestTypes)

// UCLASS overrides remain defined when automation tests are disabled.
void UGGYGOHealthRepNotifyPersistentEffectTestSet::OnAttributeAggregatorCreated(
	const FGameplayAttribute& Attribute, FAggregator* NewAggregator) const
{
	Observations.Add({ EGGYGOHealthRepNotifyObservation::AggregatorCreated, Attribute,
		0.0f, 0.0f, NewAggregator != nullptr });
	Super::OnAttributeAggregatorCreated(Attribute, NewAggregator);
}

void UGGYGOHealthRepNotifyPersistentEffectTestSet::PostAttributeChange(
	const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
	Observations.Add({ EGGYGOHealthRepNotifyObservation::PostAttributeChange, Attribute, OldValue, NewValue });
	Super::PostAttributeChange(Attribute, OldValue, NewValue);
}

bool UGGYGOHealthRepNotifyPersistentEffectTestSet::PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data)
{
	Observations.Add({ EGGYGOHealthRepNotifyObservation::PreGameplayEffectExecute, Data.EvaluatedData.Attribute });
	return Super::PreGameplayEffectExecute(Data);
}

void UGGYGOHealthRepNotifyPersistentEffectTestSet::PreAttributeBaseChange(
	const FGameplayAttribute& Attribute, float& NewValue) const
{
	Observations.Add({ EGGYGOHealthRepNotifyObservation::PreAttributeBaseChange, Attribute, 0.0f, NewValue });
	Super::PreAttributeBaseChange(Attribute, NewValue);
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "Messages/GGYGOVerbMessage.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"

namespace
{
	constexpr const TCHAR* DiagnosticFlag = TEXT("GGYGOHealthRepNotifyPersistentEffectDiagnostic");

	struct FNativeRecord
	{
		float OldValue;
		float NewValue;
		int32 ObservationCount;
		bool bHasModData;
	};

	struct FProjectRecord
	{
		float OldValue;
		float NewValue;
		float Magnitude;
		int32 Sequence;
		bool bHasSource;
	};

	/** Owns both callback storage and resources, including all partial-initialization exits. */
	struct FLateCreateFixture
	{
		UEngine* Engine = nullptr;
		UGameInstance* GameInstance = nullptr;
		UWorld* World = nullptr;
		AGGYGOHealthMessageTestActor* Actor = nullptr;
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		UGGYGOHealthRepNotifyPersistentEffectTestSet* Set = nullptr;
		UGameplayEffect* Effect = nullptr;
		UGameplayMessageSubsystem* Router = nullptr;
		FGameplayAttribute Attribute;
		FActiveGameplayEffectHandle ActiveHandle;
		FDelegateHandle NativeHandle;
		FDelegateHandle ChangedHandle;
		FDelegateHandle EdgeHandle;
		TArray<FGameplayMessageListenerHandle> MessageHandles;
		TArray<FNativeRecord> NativeRecords;
		TArray<FProjectRecord> Changes;
		TArray<FProjectRecord> Edges;
		TArray<FGGYGOVerbMessage> BreakMessages;
		int32 DamageMessageCount = 0;
		int32 ProjectSequence = 0;
		bool bAttemptedApply = false;
		bool bHealth = false;

		float Current() const { return bHealth ? Set->GetHealth() : Set->GetPoise(); }
		FGGYGOAttributeEvent& ChangedEvent() const { return bHealth ? Set->OnHealthChanged : Set->OnPoiseChanged; }
		FGGYGOAttributeEvent& EdgeEvent() const { return bHealth ? Set->OnOutOfHealth : Set->OnPoiseBroken; }

		bool Initialize(bool bInHealth)
		{
			bHealth = bInHealth;
			Attribute = bHealth ? UGGYGOHealthSet::GetHealthAttribute() : UGGYGOHealthSet::GetPoiseAttribute();
			Engine = GEngine;
			if (!Engine) { return false; }
			GameInstance = NewObject<UGameInstance>(Engine);
			if (!GameInstance) { return false; }
			GameInstance->InitializeStandalone(FName(bHealth
				? TEXT("GGYGOHealthLateCreateDiagnosticWorld") : TEXT("GGYGOPoiseLateCreateDiagnosticWorld")));
			World = GameInstance->GetWorld();
			if (!World || World->GetGameInstance() != GameInstance) { return false; }
			Router = UGameInstance::GetSubsystem<UGameplayMessageSubsystem>(GameInstance);
			if (!Router) { return false; }
			Actor = World->SpawnActor<AGGYGOHealthMessageTestActor>();
			if (!Actor) { return false; }
			ASC = Actor->GetProjectAbilitySystemComponent();
			if (!ASC) { return false; }
			ASC->InitAbilityActorInfo(Actor, Actor);
			Set = NewObject<UGGYGOHealthRepNotifyPersistentEffectTestSet>(Actor, NAME_None, RF_Transient);
			if (!Set) { return false; }
			ASC->AddAttributeSetSubobject(Set);
			// Init writes the data directly. No GE, capture or numeric setter establishes an aggregator.
			if (bHealth) { Set->InitHealth(10.0f); }
			else { Set->InitPoise(10.0f); }
			Effect = NewObject<UGameplayEffect>(GameInstance, NAME_None, RF_Transient);
			if (!Effect) { return false; }
			Effect->AddToRoot();
			Effect->DurationPolicy = EGameplayEffectDurationType::Infinite;
			Effect->Period = FScalableFloat(UGameplayEffect::NO_PERIOD);
			FGameplayModifierInfo& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
			Modifier.Attribute = Attribute;
			Modifier.ModifierOp = EGameplayModOp::Additive;
			Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(5.0f));
			return true;
		}

		void Bind()
		{
			ChangedHandle = ChangedEvent().AddLambda([this](AActor* Instigator, AActor* Causer,
				const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
			{
				Changes.Add({ OldValue, NewValue, Magnitude, ++ProjectSequence,
					Instigator != nullptr || Causer != nullptr || Spec != nullptr });
			});
			EdgeHandle = EdgeEvent().AddLambda([this](AActor* Instigator, AActor* Causer,
				const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
			{
				Edges.Add({ OldValue, NewValue, Magnitude, ++ProjectSequence,
					Instigator != nullptr || Causer != nullptr || Spec != nullptr });
			});
			MessageHandles.Add(Router->RegisterListener<FGGYGOVerbMessage>(GGYGOGameplayTags::Message_PoiseBreak,
				[this](FGameplayTag, const FGGYGOVerbMessage& Message) { BreakMessages.Add(Message); }));
			MessageHandles.Add(Router->RegisterListener<FGGYGOVerbMessage>(GGYGOGameplayTags::Message_Damage,
				[this](FGameplayTag, const FGGYGOVerbMessage&) { ++DamageMessageCount; }));
			// A single listener records the immutable native data before causing recursive GAS calls.
			NativeHandle = ASC->GetGameplayAttributeValueChangeDelegate(Attribute).AddLambda(
				[this](const FOnAttributeChangeData& Data)
				{
					NativeRecords.Add({ Data.OldValue, Data.NewValue, Set->Observations.Num(), Data.GEModData != nullptr });
					if (!bAttemptedApply && Data.OldValue == 10.0f && Data.NewValue == 0.0f)
					{
						bAttemptedApply = true;
						FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
						Context.AddInstigator(Actor, Actor);
						FGameplayEffectSpec Spec(Effect, Context, 1.0f);
						ActiveHandle = ASC->ApplyGameplayEffectSpecToSelf(Spec);
					}
				});
		}

		~FLateCreateFixture()
		{
			// Records are members: they outlive unbinding, GE removal and actor/world teardown.
			if (ASC && NativeHandle.IsValid()) { ASC->GetGameplayAttributeValueChangeDelegate(Attribute).Remove(NativeHandle); }
			if (Set && ChangedHandle.IsValid()) { ChangedEvent().Remove(ChangedHandle); }
			if (Set && EdgeHandle.IsValid()) { EdgeEvent().Remove(EdgeHandle); }
			for (FGameplayMessageListenerHandle& Handle : MessageHandles) { Handle.Unregister(); }
			if (ASC && ActiveHandle.IsValid() && ASC->GetActiveGameplayEffect(ActiveHandle))
			{
				ASC->RemoveActiveGameplayEffect(ActiveHandle);
			}
			if (World && Actor) { World->DestroyActor(Actor); }
			if (Effect && Effect->IsRooted()) { Effect->RemoveFromRoot(); }
			if (GameInstance) { GameInstance->Shutdown(); }
			if (World)
			{
				World->DestroyWorld(false);
				if (Engine) { Engine->DestroyWorldContext(World); }
				if (UPackage* Package = World->GetPackage()) { Package->SetDirtyFlag(false); }
			}
			if (GameInstance) { GameInstance->MarkAsGarbage(); }
		}
	};

	int32 CountPair(const TArray<FProjectRecord>& Records, float OldValue, float NewValue)
	{
		return Records.FilterByPredicate([=](const FProjectRecord& Record)
		{
			return Record.OldValue == OldValue && Record.NewValue == NewValue;
		}).Num();
	}

	bool RunLateCreate(FAutomationTestBase& Test, bool bHealth)
	{
		FLateCreateFixture Fixture;
		if (!Test.TestTrue(TEXT("Initialize independent GI World, registered project ASC and Actor-Outer Set"), Fixture.Initialize(bHealth)))
		{
			return false;
		}
		bool bPassed = true;
		bPassed &= Test.TestTrue(TEXT("Target is authoritative for public GE application/removal"), Fixture.Actor->HasAuthority());
		bPassed &= Test.TestTrue(TEXT("Registered attribute set is this fixture"),
			Fixture.ASC->GetAttributeSet(UGGYGOHealthSet::StaticClass()) == Fixture.Set);
		bPassed &= Test.TestTrue(TEXT("AttributeSet Outer is the ASC owner Actor"), Fixture.Set->GetOuter() == Fixture.Actor);
		bPassed &= Test.TestEqual(TEXT("No initial active effects"), Fixture.ASC->GetActiveEffects(FGameplayEffectQuery()).Num(), 0);
		bPassed &= Test.TestEqual(TEXT("No initial aggregator creation or numeric lifecycle hooks"), Fixture.Set->Observations.Num(), 0);
		bPassed &= Test.TestEqual(TEXT("Initial Current is 10"), Fixture.Current(), 10.0f);
		bPassed &= Test.TestTrue(TEXT("Persistent effect is Infinite"), Fixture.Effect->DurationPolicy == EGameplayEffectDurationType::Infinite);
		bPassed &= Test.TestEqual(TEXT("Persistent effect has no period"), Fixture.Effect->Period.GetValueAtLevel(1.0f), UGameplayEffect::NO_PERIOD);
		if (!bPassed) { return false; }
		Fixture.Bind();
		bPassed &= Test.TestTrue(TEXT("Native/Changed/edge listeners registered"),
			Fixture.NativeHandle.IsValid() && Fixture.ChangedHandle.IsValid() && Fixture.EdgeHandle.IsValid());
		for (const FGameplayMessageListenerHandle& Handle : Fixture.MessageHandles)
		{
			bPassed &= Test.TestTrue(TEXT("Message listener registered"), Handle.IsValid());
		}
		if (!bPassed) { return false; }

		if (bHealth) { Fixture.Set->SimulateReplicatedHealth(0.0f); }
		else { Fixture.Set->SimulateReplicatedPoise(0.0f); }

		bPassed &= Test.TestTrue(TEXT("Incoming native callback attempted the GE exactly once"), Fixture.bAttemptedApply);
		bPassed &= Test.TestTrue(TEXT("Public application returned an active GE handle"), Fixture.ActiveHandle.IsValid());
		bPassed &= Test.TestNotNull(TEXT("Applied GE is still active after OnRep"), Fixture.ASC->GetActiveGameplayEffect(Fixture.ActiveHandle));
		bPassed &= Test.TestEqual(TEXT("Exactly one persistent GE is active after OnRep"), Fixture.ASC->GetActiveEffects(FGameplayEffectQuery()).Num(), 1);
		bPassed &= Test.TestEqual(TEXT("OnRep preserves the real restored Current"), Fixture.Current(), 5.0f);
		bPassed &= Test.TestEqual(TEXT("Replication and restoration each publish a Changed fact"), Fixture.Changes.Num(), 2);
		bPassed &= Test.TestEqual(TEXT("One immutable replicated 10->0 fact"), CountPair(Fixture.Changes, 10.0f, 0.0f), 1);
		bPassed &= Test.TestEqual(TEXT("Late GE restoration 0->5 must not be suppressed as rewind"), CountPair(Fixture.Changes, 0.0f, 5.0f), 1);
		bPassed &= Test.TestEqual(TEXT("One historical replicated zero edge"), Fixture.Edges.Num(), 1);
		bPassed &= Test.TestEqual(TEXT("Replicated edge retains 10->0"), CountPair(Fixture.Edges, 10.0f, 0.0f), 1);
		bPassed &= Test.TestEqual(TEXT("Replication/restoration do not publish PoiseBreak messages"), Fixture.BreakMessages.Num(), 0);
		bPassed &= Test.TestEqual(TEXT("Incoming and nested restoration each notify native listeners"), Fixture.NativeRecords.Num(), 2);
		bPassed &= Test.TestEqual(TEXT("Only creation and the real restoration Post occurred"), Fixture.Set->Observations.Num(), 2);

		// Keep removing after project assertions fail: this independently diagnoses the lost restore latch.
		if (!Fixture.ActiveHandle.IsValid() || !Fixture.ASC->GetActiveGameplayEffect(Fixture.ActiveHandle)) { return false; }
		const bool bRemoved = Fixture.ASC->RemoveActiveGameplayEffect(Fixture.ActiveHandle);
		bPassed &= Test.TestTrue(TEXT("Public removal of the real GE succeeded after OnRep returned"), bRemoved);
		bPassed &= Test.TestNull(TEXT("Removed GE is no longer active"), Fixture.ASC->GetActiveGameplayEffect(Fixture.ActiveHandle));
		bPassed &= Test.TestEqual(TEXT("No persistent GE remains after removal"), Fixture.ASC->GetActiveEffects(FGameplayEffectQuery()).Num(), 0);
		bPassed &= Test.TestEqual(TEXT("Removal returns Current to zero"), Fixture.Current(), 0.0f);
		bPassed &= Test.TestEqual(TEXT("Replication, restoration and removal produce three Changed facts"), Fixture.Changes.Num(), 3);
		bPassed &= Test.TestEqual(TEXT("Replicated fact remains immutable after removal"), CountPair(Fixture.Changes, 10.0f, 0.0f), 1);
		bPassed &= Test.TestEqual(TEXT("Restoration fact exists exactly once"), CountPair(Fixture.Changes, 0.0f, 5.0f), 1);
		bPassed &= Test.TestEqual(TEXT("Removal has its own 5->0 fact"), CountPair(Fixture.Changes, 5.0f, 0.0f), 1);
		bPassed &= Test.TestEqual(TEXT("Restoration reopens the latch for a second real zero edge"), Fixture.Edges.Num(), 2);
		bPassed &= Test.TestEqual(TEXT("Only one replicated zero edge"), CountPair(Fixture.Edges, 10.0f, 0.0f), 1);
		bPassed &= Test.TestEqual(TEXT("Removal produces a real second zero edge"), CountPair(Fixture.Edges, 5.0f, 0.0f), 1);
		bPassed &= Test.TestEqual(TEXT("Native notification count is three"), Fixture.NativeRecords.Num(), 3);
		bPassed &= Test.TestEqual(TEXT("Only creation plus two real Posts occurred"), Fixture.Set->Observations.Num(), 3);
		bPassed &= Test.TestEqual(TEXT("No Damage message from replication or direct persistent Modifier"), Fixture.DamageMessageCount, 0);

		const float ExpectedOld[] = { 10.0f, 0.0f, 5.0f };
		const float ExpectedNew[] = { 0.0f, 5.0f, 0.0f };
		const int32 ExpectedObservationCounts[] = { 0, 2, 3 };
		for (int32 Index = 0; Index < 3 && Fixture.NativeRecords.IsValidIndex(Index); ++Index)
		{
			const FNativeRecord& Record = Fixture.NativeRecords[Index];
			bPassed &= Test.TestEqual(FString::Printf(TEXT("Native[%d] Old"), Index), Record.OldValue, ExpectedOld[Index]);
			bPassed &= Test.TestEqual(FString::Printf(TEXT("Native[%d] New"), Index), Record.NewValue, ExpectedNew[Index]);
			bPassed &= Test.TestEqual(FString::Printf(TEXT("Native[%d] prior hook count"), Index), Record.ObservationCount, ExpectedObservationCounts[Index]);
			bPassed &= Test.TestFalse(FString::Printf(TEXT("Native[%d] has no instant execution context"), Index), Record.bHasModData);
		}
		for (int32 Index = 0; Index < Fixture.Set->Observations.Num(); ++Index)
		{
			const FGGYGOHealthRepNotifyObservation& Record = Fixture.Set->Observations[Index];
			bPassed &= Test.TestTrue(TEXT("Observation concerns only the selected attribute"), Record.Attribute == Fixture.Attribute);
			if (Index == 0)
			{
				bPassed &= Test.TestTrue(TEXT("First hook is creation of a real aggregator after incoming native callback"),
					Record.Kind == EGGYGOHealthRepNotifyObservation::AggregatorCreated && Record.bAggregatorWasNonNull);
			}
			else
			{
				bPassed &= Test.TestTrue(TEXT("Subsequent hooks are Posts, never PreExecute/PreBase or another creation"),
					Record.Kind == EGGYGOHealthRepNotifyObservation::PostAttributeChange);
				if (Index < 3)
				{
					bPassed &= Test.TestEqual(TEXT("Real Post Old"), Record.OldValue, ExpectedOld[Index]);
					bPassed &= Test.TestEqual(TEXT("Real Post New"), Record.NewValue, ExpectedNew[Index]);
				}
			}
		}
		for (const FProjectRecord& Change : Fixture.Changes)
		{
			bPassed &= Test.TestFalse(TEXT("Changed does not fabricate persistent/replication execution sources"), Change.bHasSource);
			bPassed &= Test.TestEqual(TEXT("Changed magnitude is its observed delta"), Change.Magnitude, Change.NewValue - Change.OldValue);
		}
		for (const FProjectRecord& Edge : Fixture.Edges)
		{
			bPassed &= Test.TestFalse(TEXT("Zero edge does not fabricate sources"), Edge.bHasSource);
			const FProjectRecord* Change = Fixture.Changes.FindByPredicate([&Edge](const FProjectRecord& Record)
			{
				return Record.OldValue == Edge.OldValue && Record.NewValue == Edge.NewValue;
			});
			bPassed &= Test.TestTrue(TEXT("Matching Changed precedes each zero edge"), Change && Change->Sequence < Edge.Sequence);
		}
		bPassed &= Test.TestEqual(TEXT("Only real Poise removal publishes one Break message"), Fixture.BreakMessages.Num(), bHealth ? 0 : 1);
		for (const FGGYGOVerbMessage& Message : Fixture.BreakMessages)
		{
			bPassed &= Test.TestTrue(TEXT("Break verb and target are real"),
				Message.Verb == GGYGOGameplayTags::Message_PoiseBreak && Message.Target == Fixture.Actor);
			bPassed &= Test.TestEqual(TEXT("Real removal Break magnitude is 5"), Message.Magnitude, 5.0f);
			bPassed &= Test.TestTrue(TEXT("Break has no fabricated instigator or captured tags"),
				Message.Instigator == nullptr && Message.InstigatorTags.IsEmpty() && Message.TargetTags.IsEmpty());
		}
		return bPassed;
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FGGYGOHealthRepNotifyPersistentEffectDiagnostic,
	"ProjectDiagnostics.Health.RepNotifyPersistentEffect.LateCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FGGYGOHealthRepNotifyPersistentEffectDiagnostic::GetTests(TArray<FString>& OutBeautifiedNames,
	TArray<FString>& OutTestCommands) const
{
	if (FParse::Param(FCommandLine::Get(), DiagnosticFlag))
	{
		OutBeautifiedNames.Add(TEXT("Health"));
		OutTestCommands.Add(TEXT("Health"));
		OutBeautifiedNames.Add(TEXT("Poise"));
		OutTestCommands.Add(TEXT("Poise"));
	}
}

bool FGGYGOHealthRepNotifyPersistentEffectDiagnostic::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Explicit LateCreate diagnostic flag"), FParse::Param(FCommandLine::Get(), DiagnosticFlag))
		|| !TestTrue(TEXT("Explicit Health/Poise diagnostic case"), Parameters == TEXT("Health") || Parameters == TEXT("Poise")))
	{
		return false;
	}
	return RunLateCreate(*this, Parameters == TEXT("Health"));
}

#endif // WITH_DEV_AUTOMATION_TESTS
