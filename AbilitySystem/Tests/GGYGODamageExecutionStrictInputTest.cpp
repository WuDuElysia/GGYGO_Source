/** Real GAS fixtures for the production damage execution's base-input contract. */
#include "AbilitySystem/Tests/GGYGOHitSemanticsTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "AbilitySystem/Attributes/GGYGOCombatSet.h"
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOGameplayEffectContext.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/CriticalSection.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include <limits>

namespace
{
	struct FStrictInputCase
	{
		const TCHAR* Name;
		bool bHasCombatSet;
		float CapturedDamage;
		float CapturedPoise;
		TOptional<float> DamageOverride;
		TOptional<float> PoiseOverride;
		float ExpectedDamage;
		float ExpectedPoise;
		bool bReject;
		const TCHAR* DamageReason;
		const TCHAR* PoiseReason;
		bool bApply = false;
	};

	uint32 StrictInputFloatBits(float Value)
	{
		uint32 Bits;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return Bits;
	}

	struct FStrictInputWorld
	{
		UEngine* Engine = GEngine;
		UWorld* World = nullptr;
		AActor* SourceActor = nullptr;
		AActor* TargetActor = nullptr;
		UGGYGOAbilitySystemComponent* SourceASC = nullptr;
		UGGYGOAbilitySystemComponent* TargetASC = nullptr;
		TStrongObjectPtr<UGGYGOCombatSet> CombatSet;
		TStrongObjectPtr<UGGYGOHealthSet> HealthSet;

		explicit FStrictInputWorld(FName Name)
		{
			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, Name, nullptr, true, ERHIFeatureLevel::Num, &Init);
			if (World) { Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World); }
		}

		~FStrictInputWorld()
		{
			if (SourceASC && CombatSet.IsValid()) { SourceASC->RemoveSpawnedAttribute(CombatSet.Get()); }
			if (TargetASC && HealthSet.IsValid()) { TargetASC->RemoveSpawnedAttribute(HealthSet.Get()); }
			CombatSet.Reset();
			HealthSet.Reset();
			if (World)
			{
				UPackage* Package = World->GetPackage();
				World->DestroyWorld(false);
				Engine->DestroyWorldContext(World);
				if (Package) { Package->SetDirtyFlag(false); }
			}
		}

		static UGGYGOAbilitySystemComponent* MakeASC(UWorld* InWorld, AActor*& Actor)
		{
			Actor = InWorld->SpawnActor<AActor>();
			if (!Actor) { return nullptr; }
			UGGYGOAbilitySystemComponent* ASC = NewObject<UGGYGOAbilitySystemComponent>(Actor);
			if (ASC)
			{
				ASC->RegisterComponent();
				ASC->InitAbilityActorInfo(Actor, Actor);
			}
			return ASC;
		}

		bool Initialize(FAutomationTestBase& Test, const FStrictInputCase& Case)
		{
			if (!Test.TestNotNull(TEXT("Fixture World"), World)
				|| !Test.TestNotNull(TEXT("Fixture WorldContext"), Engine->GetWorldContextFromWorld(World))) { return false; }
			SourceASC = MakeASC(World, SourceActor);
			TargetASC = MakeASC(World, TargetActor);
			if (!Test.TestNotNull(TEXT("Source ASC"), SourceASC)
				|| !Test.TestNotNull(TEXT("Target ASC"), TargetASC)) { return false; }
			if (!Test.TestTrue(TEXT("Registered authoritative ASC ownership and actor discovery"),
				SourceASC->IsRegistered() && TargetASC->IsRegistered()
				&& SourceASC->IsOwnerActorAuthoritative() && TargetASC->IsOwnerActorAuthoritative()
				&& SourceASC->GetOwnerActor() == SourceActor && SourceASC->GetAvatarActor() == SourceActor
				&& TargetASC->GetOwnerActor() == TargetActor && TargetASC->GetAvatarActor() == TargetActor
				&& SourceActor->FindComponentByClass<UAbilitySystemComponent>() == SourceASC
				&& TargetActor->FindComponentByClass<UAbilitySystemComponent>() == TargetASC)) { return false; }

			HealthSet.Reset(NewObject<UGGYGOHealthSet>(TargetActor, NAME_None, RF_Transient));
			if (!Test.TestNotNull(TEXT("Real HealthSet"), HealthSet.Get())) { return false; }
			HealthSet->InitMaxHealth(100.0f);
			HealthSet->InitHealth(100.0f);
			HealthSet->InitMaxPoise(50.0f);
			HealthSet->InitPoise(50.0f);
			HealthSet->InitDamage(0.0f);
			HealthSet->InitPoiseDamage(0.0f);
			HealthSet->InitHealing(0.0f);
			TargetASC->AddAttributeSetSubobject(HealthSet.Get());
			if (!Test.TestTrue(TEXT("HealthSet actual Actor Outer and ASC ownership"),
				HealthSet->GetOuter() == TargetActor && HealthSet->GetOwningActor() == TargetActor
				&& HealthSet->GetOwningAbilitySystemComponent() == TargetASC
				&& TargetASC->GetSet<UGGYGOHealthSet>() == HealthSet.Get()
				&& TargetASC->HasAttributeSetForAttribute(UGGYGOHealthSet::GetDamageAttribute())
				&& TargetASC->HasAttributeSetForAttribute(UGGYGOHealthSet::GetPoiseDamageAttribute()))) { return false; }
			if (!Test.TestTrue(TEXT("Health, Poise and all meta inputs initialized"),
				HealthSet->GetHealth() == 100.0f && HealthSet->GetMaxHealth() == 100.0f
				&& HealthSet->GetPoise() == 50.0f && HealthSet->GetMaxPoise() == 50.0f
				&& HealthSet->GetDamage() == 0.0f && HealthSet->GetPoiseDamage() == 0.0f && HealthSet->GetHealing() == 0.0f)
				|| !Test.TestFalse(TEXT("No immunity or GodMode"),
					TargetASC->HasMatchingGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity)
					|| TargetASC->HasMatchingGameplayTag(GGYGOGameplayTags::Cheat_GodMode))) { return false; }

			if (Case.bHasCombatSet)
			{
				CombatSet.Reset(NewObject<UGGYGOCombatSet>(SourceActor, NAME_None, RF_Transient));
				if (!Test.TestNotNull(TEXT("Real CombatSet"), CombatSet.Get())) { return false; }
				// Public initialization precedes registration and native snapshot/aggregator creation.
				CombatSet->InitBaseDamage(Case.CapturedDamage);
				CombatSet->InitBasePoiseDamage(Case.CapturedPoise);
				SourceASC->AddAttributeSetSubobject(CombatSet.Get());
				if (!Test.TestTrue(TEXT("CombatSet actual Actor Outer, ASC ownership and values"),
					CombatSet->GetOuter() == SourceActor && CombatSet->GetOwningActor() == SourceActor
					&& CombatSet->GetOwningAbilitySystemComponent() == SourceASC
					&& SourceASC->GetSet<UGGYGOCombatSet>() == CombatSet.Get()
					&& (FMath::IsNaN(Case.CapturedDamage) ? FMath::IsNaN(CombatSet->GetBaseDamage()) : CombatSet->GetBaseDamage() == Case.CapturedDamage)
					&& (FMath::IsNaN(Case.CapturedPoise) ? FMath::IsNaN(CombatSet->GetBasePoiseDamage()) : CombatSet->GetBasePoiseDamage() == Case.CapturedPoise))) { return false; }
			}
			return Test.TestTrue(TEXT("Native capture attribute presence matches fixture"),
				SourceASC->HasAttributeSetForAttribute(UGGYGOCombatSet::GetBaseDamageAttribute()) == Case.bHasCombatSet
				&& SourceASC->HasAttributeSetForAttribute(UGGYGOCombatSet::GetBasePoiseDamageAttribute()) == Case.bHasCombatSet
				&& (SourceASC->GetSet<UGGYGOCombatSet>() != nullptr) == Case.bHasCombatSet);
		}
	};

	struct FStrictInputLogMessage
	{
		FString Text;
		ELogVerbosity::Type Verbosity;
		FName Category;
	};

	/** Observes original category/severity without filtering or changing log settings. */
	class FStrictInputLogObservation final : public FOutputDevice
	{
	public:
		FStrictInputLogObservation() { GLog->AddOutputDevice(this); }
		virtual ~FStrictInputLogObservation() override { GLog->RemoveOutputDevice(this); }
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			FScopeLock Lock(&Mutex);
			if (bObserving && Category == FName(TEXT("LogGGYGOAbilitySystem"))
				&& (Verbosity == ELogVerbosity::Error || Verbosity == ELogVerbosity::Warning))
			{
				Messages.Add({FString(Text), Verbosity, Category});
			}
		}
		void Begin() { FScopeLock Lock(&Mutex); bObserving = true; }
		TArray<FStrictInputLogMessage> End()
		{
			FScopeLock Lock(&Mutex);
			bObserving = false;
			return Messages;
		}
	private:
		FCriticalSection Mutex;
		bool bObserving = false;
		TArray<FStrictInputLogMessage> Messages;
	};

	struct FStrictInputCapture
	{
		bool bSucceeded = false;
		float Value = 0.0f;
	};

	FStrictInputCapture ReadStrictInputCapture(const FGameplayEffectCustomExecutionParameters& Params,
		const FGameplayEffectAttributeCaptureDefinition& Definition)
	{
		FAggregatorEvaluateParameters Evaluation;
		Evaluation.SourceTags = Params.GetOwningSpec().CapturedSourceTags.GetAggregatedTags();
		Evaluation.TargetTags = Params.GetOwningSpec().CapturedTargetTags.GetAggregatedTags();
		FStrictInputCapture Result;
		Result.bSucceeded = Params.AttemptCalculateCapturedAttributeMagnitude(Definition, Evaluation, Result.Value);
		return Result;
	}

	template <typename Key>
	bool SameStrictInputMap(const TMap<Key, float>& Before, const TMap<Key, float>& After)
	{
		if (Before.Num() != After.Num()) { return false; }
		for (const auto& Pair : Before)
		{
			const float* Value = After.Find(Pair.Key);
			if (!Value || StrictInputFloatBits(Pair.Value) != StrictInputFloatBits(*Value)) { return false; }
		}
		return true;
	}

	bool RunStrictInputCase(FAutomationTestBase& Test, const FStrictInputCase& Case, bool bPrefilled)
	{
		const FString Label = FString::Printf(TEXT("%s/%s"), Case.Name, bPrefilled ? TEXT("Prefilled") : TEXT("Empty"));
		FStrictInputWorld Fixture(FName(*FString::Printf(TEXT("GGYGOStrictInput_%s_%s"),
			Case.Name, bPrefilled ? TEXT("Prefilled") : TEXT("Empty"))));
		if (!Fixture.Initialize(Test, Case)) { return false; }
		const FGameplayEffectContextHandle Context = Fixture.SourceASC->MakeEffectContext();
		const FGGYGOGameplayEffectContext* ProjectContext = FGGYGOGameplayEffectContext::ExtractEffectContext(Context);
		if (!Test.TestNotNull(Label + TEXT(" project Context factory"), ProjectContext)
			|| !Test.TestTrue(Label + TEXT(" actual source Context identity"),
				Context.GetInstigator() == Fixture.SourceActor && Context.GetEffectCauser() == Fixture.SourceActor
				&& Context.GetInstigatorAbilitySystemComponent() == Fixture.SourceASC
				&& ProjectContext->GetAbilitySource() == nullptr && Context.GetSourceObject() == nullptr)) { return false; }
		FGameplayEffectSpecHandle SpecHandle = Fixture.SourceASC->MakeOutgoingSpec(
			UGGYGOHitDistanceTestEffect::StaticClass(), 1.0f, Context);
		if (!Test.TestTrue(Label + TEXT(" real outgoing Spec"), SpecHandle.IsValid())) { return false; }
		FGameplayEffectSpec& Spec = *SpecHandle.Data.Get();
		const UGameplayEffect* Definition = Spec.Def.Get();
		if (!Test.TestNotNull(Label + TEXT(" GE definition"), Definition)
			|| !Test.TestTrue(Label + TEXT(" Instant GE selects only production execution"),
				Definition == GetDefault<UGGYGOHitDistanceTestEffect>()
				&& Definition->DurationPolicy == EGameplayEffectDurationType::Instant
				&& Definition->Modifiers.IsEmpty() && Definition->GameplayCues.IsEmpty() && Definition->Executions.Num() == 1
				&& Definition->Executions[0].CalculationClass == UGGYGODamageExecution::StaticClass()
				&& Definition->Executions[0].CalculationModifiers.IsEmpty())) { return false; }
		const UGGYGODamageExecution* Execution = GetDefault<UGGYGODamageExecution>();
		const FGameplayEffectAttributeCaptureDefinition DamageDefinition(UGGYGOCombatSet::GetBaseDamageAttribute(),
			EGameplayEffectAttributeCaptureSource::Source, true);
		const FGameplayEffectAttributeCaptureDefinition PoiseDefinition(UGGYGOCombatSet::GetBasePoiseDamageAttribute(),
			EGameplayEffectAttributeCaptureSource::Source, true);
		const auto& CaptureDefinitions = Execution->GetAttributeCaptureDefinitions();
		if (!Test.TestTrue(Label + TEXT(" production has exactly two source snapshot captures"),
			CaptureDefinitions.Num() == 2 && CaptureDefinitions.Contains(DamageDefinition) && CaptureDefinitions.Contains(PoiseDefinition))
			|| !Test.TestTrue(Label + TEXT(" factory Spec starts without overrides"),
				Spec.SetByCallerTagMagnitudes.IsEmpty() && Spec.SetByCallerNameMagnitudes.IsEmpty())) { return false; }
		if (Case.DamageOverride.IsSet()) { Spec.SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, Case.DamageOverride.GetValue()); }
		if (Case.PoiseOverride.IsSet()) { Spec.SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, Case.PoiseOverride.GetValue()); }
		const float* DamageOverride = Spec.SetByCallerTagMagnitudes.Find(GGYGOGameplayTags::SetByCaller_Damage);
		const float* PoiseOverride = Spec.SetByCallerTagMagnitudes.Find(GGYGOGameplayTags::SetByCaller_PoiseDamage);
		if (!Test.TestTrue(Label + TEXT(" exact override keys and value bits"),
			(DamageOverride != nullptr) == Case.DamageOverride.IsSet() && (PoiseOverride != nullptr) == Case.PoiseOverride.IsSet()
			&& (!DamageOverride || StrictInputFloatBits(*DamageOverride) == StrictInputFloatBits(Case.DamageOverride.GetValue()))
			&& (!PoiseOverride || StrictInputFloatBits(*PoiseOverride) == StrictInputFloatBits(Case.PoiseOverride.GetValue()))
			&& Spec.SetByCallerTagMagnitudes.Num() == int32(Case.DamageOverride.IsSet()) + int32(Case.PoiseOverride.IsSet()))) { return false; }

		FGameplayEffectCustomExecutionParameters Params(Spec, Definition->Executions[0].CalculationModifiers,
			Fixture.TargetASC, Definition->Executions[0].PassedInTags, FPredictionKey());
		const FStrictInputCapture DamageCapture = ReadStrictInputCapture(Params, DamageDefinition);
		const FStrictInputCapture PoiseCapture = ReadStrictInputCapture(Params, PoiseDefinition);
		if (!Test.TestTrue(Label + TEXT(" native execution source and target"),
			Params.GetSourceAbilitySystemComponent() == Fixture.SourceASC && Params.GetTargetAbilitySystemComponent() == Fixture.TargetASC)
			|| !Test.TestTrue(Label + TEXT(" native getters and capture validity match actual attribute presence"),
				DamageCapture.bSucceeded == Case.bHasCombatSet && PoiseCapture.bSucceeded == Case.bHasCombatSet
				&& Spec.HasValidCapturedAttributes(CaptureDefinitions) == Case.bHasCombatSet
				&& (Spec.CapturedRelevantAttributes.FindCaptureSpecByDefinition(DamageDefinition, true) != nullptr) == Case.bHasCombatSet
				&& (Spec.CapturedRelevantAttributes.FindCaptureSpecByDefinition(PoiseDefinition, true) != nullptr) == Case.bHasCombatSet
				&& (!Case.bHasCombatSet || (
					(FMath::IsNaN(Case.CapturedDamage) ? FMath::IsNaN(DamageCapture.Value) : DamageCapture.Value == Case.CapturedDamage)
					&& (FMath::IsNaN(Case.CapturedPoise) ? FMath::IsNaN(PoiseCapture.Value) : PoiseCapture.Value == Case.CapturedPoise))))) { return false; }
		if (!FMath::IsFinite(Case.CapturedDamage) || !FMath::IsFinite(Case.CapturedPoise))
		{
			if (!Test.TestTrue(Label + TEXT(" actual non-finite native capture and normal other input without SBC"),
				Case.bHasCombatSet && Case.bReject && Spec.SetByCallerTagMagnitudes.IsEmpty() && Spec.SetByCallerNameMagnitudes.IsEmpty()
				&& DamageCapture.bSucceeded && PoiseCapture.bSucceeded
				&& ((!FMath::IsFinite(Case.CapturedDamage) && !FMath::IsFinite(DamageCapture.Value)
					&& FMath::IsFinite(Case.CapturedPoise) && PoiseCapture.Value == Case.CapturedPoise)
					|| (!FMath::IsFinite(Case.CapturedPoise) && !FMath::IsFinite(PoiseCapture.Value)
						&& FMath::IsFinite(Case.CapturedDamage) && DamageCapture.Value == Case.CapturedDamage)))) { return false; }
		}

		// Only relevant public facts; native read-only evaluation may populate derived caches.
		const auto TagsBefore = Spec.SetByCallerTagMagnitudes;
		const auto NamesBefore = Spec.SetByCallerNameMagnitudes;
		const FGameplayTagContainer SourceTagsBefore = *Spec.CapturedSourceTags.GetAggregatedTags();
		const FGameplayTagContainer TargetTagsBefore = *Spec.CapturedTargetTags.GetAggregatedTags();
		const uint32 LevelBefore = StrictInputFloatBits(Spec.GetLevel());
		const int32 StackBefore = Spec.GetStackCount();
		FGameplayEffectCustomExecutionOutput Output;
		if (bPrefilled) { Output.AddOutputModifier(FGameplayModifierEvaluatedData(UGGYGOHealthSet::GetHealingAttribute(), EGameplayModOp::Additive, 123.0f)); }
		FString ExpectedRaw;
		if (Case.bReject)
		{
			const FString DamageValue = DamageOverride ? FString::Printf(TEXT("%.9g"), static_cast<double>(*DamageOverride))
				: (DamageCapture.bSucceeded ? FString::Printf(TEXT("%.9g"), static_cast<double>(DamageCapture.Value)) : TEXT("unavailable"));
			const FString PoiseValue = PoiseOverride ? FString::Printf(TEXT("%.9g"), static_cast<double>(*PoiseOverride))
				: (PoiseCapture.bSucceeded ? FString::Printf(TEXT("%.9g"), static_cast<double>(PoiseCapture.Value)) : TEXT("unavailable"));
			ExpectedRaw = FString::Printf(
				TEXT("DamageExecution [%s] 拒绝基础输入：GE=%s SourceASC=%s TargetASC=%s BaseDamage{Source=%s Reason=%s Value=%s} BasePoiseDamage{Source=%s Reason=%s Value=%s}；本Execution不新增Modifier。"),
				*Execution->GetPathName(), *GetPathNameSafe(Definition), *Fixture.SourceASC->GetPathName(), *Fixture.TargetASC->GetPathName(),
				DamageOverride ? TEXT("SetByCallerTag") : TEXT("CapturedAttribute"), Case.DamageReason, *DamageValue,
				PoiseOverride ? TEXT("SetByCallerTag") : TEXT("CapturedAttribute"), Case.PoiseReason, *PoiseValue);
		}
		TArray<FStrictInputLogMessage> Messages;
		{
			FStrictInputLogObservation Observation;
			if (!Test.TestTrue(Label + TEXT(" native log observer registered"), GLog->IsRedirectingTo(&Observation))) { return false; }
			if (Case.bReject)
			{
				Test.AddExpectedMessagePlain(TEXT("LogGGYGOAbilitySystem: ") + ExpectedRaw,
					ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, 1);
			}
			Observation.Begin();
			Execution->Execute(Params, Output);
			Messages = Observation.End();
		}
		bool bPassed = Test.TestEqual(Label + TEXT(" category Error/Warning count"), Messages.Num(), Case.bReject ? 1 : 0);
		if (Case.bReject && Messages.Num() == 1)
		{
			bPassed &= Test.TestTrue(Label + TEXT(" exact raw category, Error severity and full diagnostic"),
				Messages[0].Category == FName(TEXT("LogGGYGOAbilitySystem")) && Messages[0].Verbosity == ELogVerbosity::Error
				&& Messages[0].Text == ExpectedRaw);
		}
		const auto& Modifiers = Output.GetOutputModifiers();
		if (Case.bReject)
		{
			bPassed &= Test.TestEqual(Label + TEXT(" rejected output count preserved"), Modifiers.Num(), bPrefilled ? 1 : 0);
			if (bPrefilled && Modifiers.Num() == 1)
			{
				bPassed &= Test.TestTrue(Label + TEXT(" prefilled sentinel preserved exactly"),
					Modifiers[0].Attribute == UGGYGOHealthSet::GetHealingAttribute() && Modifiers[0].ModifierOp == EGameplayModOp::Additive
					&& StrictInputFloatBits(Modifiers[0].Magnitude) == StrictInputFloatBits(123.0f));
			}
		}
		else
		{
			const int32 ExpectedCount = int32(Case.ExpectedDamage > 0.0f) + int32(Case.ExpectedPoise > 0.0f);
			bPassed &= Test.TestEqual(Label + TEXT(" accepted output count"), Modifiers.Num(), ExpectedCount);
			if (Modifiers.Num() == ExpectedCount)
			{
				int32 Index = 0;
				for (const auto& Expected : {TPair<FGameplayAttribute, float>(UGGYGOHealthSet::GetDamageAttribute(), Case.ExpectedDamage),
					TPair<FGameplayAttribute, float>(UGGYGOHealthSet::GetPoiseDamageAttribute(), Case.ExpectedPoise)})
				{
					if (Expected.Value > 0.0f)
					{
						const auto& Modifier = Modifiers[Index++];
						bPassed &= Test.TestTrue(Label + TEXT(" literal expected meta modifier"), Modifier.Attribute == Expected.Key
							&& Modifier.ModifierOp == EGameplayModOp::Additive && Modifier.Magnitude == Expected.Value);
					}
				}
			}
		}
		const FStrictInputCapture DamageAfter = ReadStrictInputCapture(Params, DamageDefinition);
		const FStrictInputCapture PoiseAfter = ReadStrictInputCapture(Params, PoiseDefinition);
		bPassed &= Test.TestTrue(Label + TEXT(" public Spec facts and capture value bits preserved"),
			Spec.Def.Get() == Definition && Spec.GetContext().Get() == Context.Get()
			&& Spec.GetContext().GetInstigator() == Fixture.SourceActor && Spec.GetContext().GetEffectCauser() == Fixture.SourceActor
			&& Spec.GetContext().GetInstigatorAbilitySystemComponent() == Fixture.SourceASC && Spec.GetContext().GetSourceObject() == nullptr
			&& ProjectContext->GetAbilitySource() == nullptr && Params.GetSourceAbilitySystemComponent() == Fixture.SourceASC
			&& Params.GetTargetAbilitySystemComponent() == Fixture.TargetASC && StrictInputFloatBits(Spec.GetLevel()) == LevelBefore
			&& Spec.GetStackCount() == StackBefore && SameStrictInputMap(TagsBefore, Spec.SetByCallerTagMagnitudes)
			&& SameStrictInputMap(NamesBefore, Spec.SetByCallerNameMagnitudes)
			&& *Spec.CapturedSourceTags.GetAggregatedTags() == SourceTagsBefore && *Spec.CapturedTargetTags.GetAggregatedTags() == TargetTagsBefore
			&& DamageAfter.bSucceeded == DamageCapture.bSucceeded && PoiseAfter.bSucceeded == PoiseCapture.bSucceeded
			&& StrictInputFloatBits(DamageAfter.Value) == StrictInputFloatBits(DamageCapture.Value)
			&& StrictInputFloatBits(PoiseAfter.Value) == StrictInputFloatBits(PoiseCapture.Value));
		bPassed &= Test.TestTrue(Label + TEXT(" Execute only produces output; target attributes remain unchanged"),
			Fixture.HealthSet->GetHealth() == 100.0f && Fixture.HealthSet->GetPoise() == 50.0f
			&& Fixture.HealthSet->GetDamage() == 0.0f && Fixture.HealthSet->GetPoiseDamage() == 0.0f && Fixture.HealthSet->GetHealing() == 0.0f);
		if (Case.bApply && bPassed)
		{
			const FActiveGameplayEffectHandle Applied = Fixture.SourceASC->ApplyGameplayEffectSpecToTarget(Spec, Fixture.TargetASC);
			bPassed &= Test.TestTrue(Label + TEXT(" one real Instant application succeeds"), Applied.WasSuccessfullyApplied());
			bPassed &= Test.TestTrue(Label + TEXT(" real HealthSet consumes Damage12/PoiseDamage4"),
				Fixture.HealthSet->GetHealth() == 88.0f && Fixture.HealthSet->GetPoise() == 46.0f
				&& Fixture.HealthSet->GetDamage() == 0.0f && Fixture.HealthSet->GetPoiseDamage() == 0.0f && Fixture.HealthSet->GetHealing() == 0.0f);
		}
		return bPassed;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGODamageExecutionStrictInputTest,
	"GGYGO.AbilitySystem.DamageExecution.StrictBaseInputResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGODamageExecutionStrictInputTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Server damage execution enabled"), WITH_SERVER_CODE != 0)
		|| !TestNotNull(TEXT("GEngine"), GEngine) || !TestNotNull(TEXT("GLog"), GLog)) { return false; }
	// T1a/T1b/T1c: eight scenario families through the real native capture/production execution path.
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	const float Infinity = std::numeric_limits<float>::infinity();
	const FStrictInputCase Cases[] = {
		// 1. Native source snapshots and one full native target application.
		{TEXT("NormalCaptures"), true, 12, 4, {}, {}, 12, 4, false, TEXT("valid"), TEXT("valid"), true},
		// 2. Explicit inputs do not require either source attribute capture.
		{TEXT("OverridesWithoutCombatSet"), false, 0, 0, 7.0f, 2.0f, 7, 2, false, TEXT("valid"), TEXT("valid")},
		// 3. Zero is a valid input from either source mode.
		{TEXT("ZeroCaptures"), true, 0, 0, {}, {}, 0, 0, false, TEXT("valid"), TEXT("valid")},
		{TEXT("ZeroOverridesWithoutCombatSet"), false, 0, 0, 0.0f, 0.0f, 0, 0, false, TEXT("valid"), TEXT("valid")},
		// 4. Both mixed directions retain the non-overridden native capture.
		{TEXT("OverrideDamageCapturePoise"), true, 12, 4, 7.0f, {}, 7, 4, false, TEXT("valid"), TEXT("valid")},
		{TEXT("CaptureDamageOverridePoise"), true, 12, 4, {}, 2.0f, 12, 2, false, TEXT("valid"), TEXT("valid")},
		// 5. Finite negative capture still reaches the established final nonnegative clamp.
		{TEXT("NegativeDamageCapture"), true, -3, 4, {}, {}, 0, 4, false, TEXT("valid"), TEXT("valid")},
		{TEXT("NegativePoiseCapture"), true, 12, -3, {}, {}, 12, 0, false, TEXT("valid"), TEXT("valid")},
		// 6. An actually missing required capture rejects the whole execution output.
		{TEXT("MissingDamageCapture"), false, 0, 0, {}, 2.0f, 0, 0, true, TEXT("capture-failed"), TEXT("valid")},
		{TEXT("MissingPoiseCapture"), false, 0, 0, 7.0f, {}, 0, 0, true, TEXT("valid"), TEXT("capture-failed")},
		{TEXT("BothCapturesMissing"), false, 0, 0, {}, {}, 0, 0, true, TEXT("capture-failed"), TEXT("capture-failed")},
		// 7. Illegal explicit values must not fall back to the available native captures (12/4).
		{TEXT("NegativeDamageOverride"), true, 12, 4, -3.0f, {}, 0, 0, true, TEXT("negative-override"), TEXT("valid")},
		{TEXT("NaNDamageOverride"), true, 12, 4, NaN, {}, 0, 0, true, TEXT("non-finite"), TEXT("valid")},
		{TEXT("PositiveInfinityDamageOverride"), true, 12, 4, Infinity, {}, 0, 0, true, TEXT("non-finite"), TEXT("valid")},
		{TEXT("NegativeInfinityDamageOverride"), true, 12, 4, -Infinity, {}, 0, 0, true, TEXT("non-finite"), TEXT("valid")},
		{TEXT("NegativePoiseOverride"), true, 12, 4, {}, -3.0f, 0, 0, true, TEXT("valid"), TEXT("negative-override")},
		{TEXT("NaNPoiseOverride"), true, 12, 4, {}, NaN, 0, 0, true, TEXT("valid"), TEXT("non-finite")},
		{TEXT("PositiveInfinityPoiseOverride"), true, 12, 4, {}, Infinity, 0, 0, true, TEXT("valid"), TEXT("non-finite")},
		{TEXT("NegativeInfinityPoiseOverride"), true, 12, 4, {}, -Infinity, 0, 0, true, TEXT("valid"), TEXT("non-finite")},
		// 8. Native getter must succeed with a non-finite capture; the other capture remains normal.
		{TEXT("NaNDamageCapture"), true, NaN, 4, {}, {}, 0, 0, true, TEXT("non-finite"), TEXT("valid")},
		{TEXT("PositiveInfinityDamageCapture"), true, Infinity, 4, {}, {}, 0, 0, true, TEXT("non-finite"), TEXT("valid")},
		{TEXT("NegativeInfinityDamageCapture"), true, -Infinity, 4, {}, {}, 0, 0, true, TEXT("non-finite"), TEXT("valid")},
		{TEXT("NaNPoiseCapture"), true, 12, NaN, {}, {}, 0, 0, true, TEXT("valid"), TEXT("non-finite")},
		{TEXT("PositiveInfinityPoiseCapture"), true, 12, Infinity, {}, {}, 0, 0, true, TEXT("valid"), TEXT("non-finite")},
		{TEXT("NegativeInfinityPoiseCapture"), true, 12, -Infinity, {}, {}, 0, 0, true, TEXT("valid"), TEXT("non-finite")}
	};
	bool bPassed = true;
	for (const FStrictInputCase& Case : Cases)
	{
		const bool bEmptyPassed = RunStrictInputCase(*this, Case, false);
		bPassed &= bEmptyPassed;
		if (!bEmptyPassed) { return false; }
		if (Case.bReject)
		{
			const bool bPrefilledPassed = RunStrictInputCase(*this, Case, true);
			bPassed &= bPrefilledPassed;
			if (!bPrefilledPassed) { return false; }
		}
	}
	return bPassed;
}
#endif // WITH_DEV_AUTOMATION_TESTS
