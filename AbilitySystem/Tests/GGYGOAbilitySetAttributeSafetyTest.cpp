#include "AbilitySystem/Tests/GGYGOAbilitySetAttributeSafetyTestTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilitySetAttributeSafetyTestTypes)

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AbilitySystem/Attributes/GGYGOCombatSet.h"
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/DataValidation.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	FString ExpectedInvalidClassError(int32 Index, UClass* SetClass)
	{
		return FString::Printf(TEXT("GrantedAttributes[%d] 的属性集类 [%s] 为空、无效或抽象。"),
			Index, *GetNameSafe(SetClass));
	}

	FString ExpectedConflictError(int32 FirstIndex, UClass* FirstClass, int32 SecondIndex,
		UClass* SecondClass, const FString& Reason)
	{
		return FString::Printf(TEXT("GrantedAttributes[%d] [%s] 与 GrantedAttributes[%d] [%s] 冲突：%s。"),
			FirstIndex, *GetNameSafe(FirstClass), SecondIndex, *GetNameSafe(SecondClass), *Reason);
	}

	struct FAttributeAdmissionTestFixture
	{
		UEngine* Engine = nullptr;
		UWorld* World = nullptr;
		AGGYGOAbilitySetAttributeSafetyTestHost* Host = nullptr;
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		bool bHasWorldContext = false;
		TStrongObjectPtr<UGGYGOAbilitySetAttributeSafetyTestAsset> FirstAsset{nullptr};
		TStrongObjectPtr<UGGYGOAbilitySetAttributeSafetyTestAsset> SecondAsset{nullptr};
		FGGYGOAbilitySetAttributeSafetyHandlesView FirstHandles;
		FGGYGOAbilitySetAttributeSafetyHandlesView SecondHandles;
		// Read-only observations are used only while this fixture's World is alive.
		TArray<const UAttributeSet*> Baseline;
		const UAttributeSet* HealthSet = nullptr;
		const UAttributeSet* CombatSet = nullptr;

		bool Initialize(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("fixture runs on game thread"), IsInGameThread())) { return false; }
			Engine = GEngine;
			if (!Test.TestNotNull(TEXT("fixture GEngine"), Engine)) { return false; }

			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Init);
			if (!Test.TestTrue(TEXT("fixture valid World"), IsValid(World))) { return false; }
			Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			bHasWorldContext = true;
			const FWorldContext* Context = Engine->GetWorldContextFromWorld(World);
			bool bReady = Test.TestTrue(TEXT("fixture Game World"), World->WorldType == EWorldType::Game);
			bReady &= Test.TestNotNull(TEXT("fixture WorldContext"), Context);
			if (Context)
			{
				bReady &= Test.TestTrue(TEXT("fixture current World"), Context->World() == World);
			}
			if (!bReady) { return false; }

			Host = World->SpawnActor<AGGYGOAbilitySetAttributeSafetyTestHost>();
			if (!Test.TestTrue(TEXT("fixture valid spawned host"), IsValid(Host))) { return false; }
			bReady &= Test.TestTrue(TEXT("fixture host World"), Host->GetWorld() == World);
			bReady &= Test.TestFalse(TEXT("fixture host is not destroying"), Host->IsActorBeingDestroyed());
			bReady &= Test.TestTrue(TEXT("fixture host Authority"), Host->HasAuthority());
			bReady &= Test.TestTrue(TEXT("fixture host authority role"), Host->GetLocalRole() == ROLE_Authority);
			ASC = Host->GetGGYGOAbilitySystemComponent();
			bReady &= Test.TestTrue(TEXT("fixture valid production ASC"), IsValid(ASC));
			if (!bReady) { return false; }

			// Use public Actor lifecycle only when SpawnActor has not completed it.
			AActor* ActorLifecycle = Host;
			if (!ActorLifecycle->HasActorRegisteredAllComponents())
			{
				ActorLifecycle->RegisterAllComponents();
			}
			bReady &= Test.TestTrue(TEXT("fixture host components registered"), Host->HasActorRegisteredAllComponents());
			bReady &= Test.TestTrue(TEXT("fixture ASC registered"), ASC->IsRegistered());
			if (!bReady) { return false; }
			if (!ActorLifecycle->IsActorInitialized())
			{
				ActorLifecycle->PreInitializeComponents();
				ActorLifecycle->InitializeComponents();
				if (!Test.TestTrue(TEXT("fixture ASC initialized before PostInitialize"), ASC->HasBeenInitialized()))
				{
					return false;
				}
				ActorLifecycle->PostInitializeComponents();
			}
			bReady &= Test.TestTrue(TEXT("fixture host initialized"), Host->IsActorInitialized());
			bReady &= Test.TestTrue(TEXT("fixture ASC initialized"), ASC->HasBeenInitialized());
			bReady &= Test.TestFalse(TEXT("fixture ASC is not destroying"), ASC->IsBeingDestroyed());
			bReady &= Test.TestTrue(TEXT("fixture ASC World"), ASC->GetWorld() == World);
			bReady &= Test.TestTrue(TEXT("fixture component Owner"), ASC->GetOwner() == Host);
			bReady &= Test.TestTrue(TEXT("fixture ActorInfo Owner"), ASC->GetOwnerActor() == Host);
			bReady &= Test.TestTrue(TEXT("fixture no Avatar"), ASC->GetAvatarActor() == nullptr);
			bReady &= Test.TestTrue(TEXT("fixture ASC cached Authority"), ASC->IsOwnerActorAuthoritative());
			if (!bReady) { return false; }

			// Native ASC InitializeComponent must discover the production default subobjects.
			for (const UAttributeSet* Set : ASC->GetSpawnedAttributes()) { Baseline.Add(Set); }
			HealthSet = ASC->GetAttributeSet(UGGYGOHealthSet::StaticClass());
			CombatSet = ASC->GetAttributeSet(UGGYGOCombatSet::StaticClass());
			bReady &= Test.TestEqual(TEXT("fixture exactly two default sets"), Baseline.Num(), 2);
			bReady &= Test.TestTrue(TEXT("fixture valid default Health"), IsValid(HealthSet));
			bReady &= Test.TestTrue(TEXT("fixture valid default Combat"), IsValid(CombatSet));
			bReady &= Test.TestTrue(TEXT("fixture distinct default pointers"), HealthSet != CombatSet);
			bReady &= Test.TestTrue(TEXT("fixture list contains default Health"), Baseline.Contains(HealthSet));
			bReady &= Test.TestTrue(TEXT("fixture list contains default Combat"), Baseline.Contains(CombatSet));
			if (IsValid(HealthSet) && IsValid(CombatSet))
			{
				bReady &= Test.TestTrue(TEXT("fixture exact Health class"), HealthSet->GetClass() == UGGYGOHealthSet::StaticClass());
				bReady &= Test.TestTrue(TEXT("fixture exact Combat class"), CombatSet->GetClass() == UGGYGOCombatSet::StaticClass());
				bReady &= Test.TestTrue(TEXT("fixture Health Outer"), HealthSet->GetOuter() == Host);
				bReady &= Test.TestTrue(TEXT("fixture Combat Outer"), CombatSet->GetOuter() == Host);
				bReady &= Test.TestTrue(TEXT("fixture Health default subobject"), HealthSet->HasAnyFlags(RF_DefaultSubObject));
				bReady &= Test.TestTrue(TEXT("fixture Combat default subobject"), CombatSet->HasAnyFlags(RF_DefaultSubObject));
			}
			if (!bReady) { return false; }

			FirstAsset.Reset(NewObject<UGGYGOAbilitySetAttributeSafetyTestAsset>(GetTransientPackage()));
			SecondAsset.Reset(NewObject<UGGYGOAbilitySetAttributeSafetyTestAsset>(GetTransientPackage()));
			bReady &= Test.TestTrue(TEXT("fixture valid first asset"), IsValid(FirstAsset.Get()));
			bReady &= Test.TestTrue(TEXT("fixture valid second asset"), IsValid(SecondAsset.Get()));
			bReady &= Test.TestTrue(TEXT("fixture distinct assets"), FirstAsset.Get() != SecondAsset.Get());
			bReady &= Test.TestEqual(TEXT("fixture first Handles initially empty"), FirstHandles.GetAttributeSetCountForTest(), 0);
			bReady &= Test.TestEqual(TEXT("fixture second Handles initially empty"), SecondHandles.GetAttributeSetCountForTest(), 0);
			return bReady;
		}

		~FAttributeAdmissionTestFixture()
		{
			// Teardown only; removal ownership, repeat Take and GC require T2c1.
			if (IsValid(ASC) && !ASC->IsBeingDestroyed() && IsValid(Host)
				&& !Host->IsActorBeingDestroyed() && Host->HasAuthority()
				&& ASC->IsOwnerActorAuthoritative() && ASC->GetOwner() == Host)
			{
				SecondHandles.TakeFromAbilitySystem(ASC);
				FirstHandles.TakeFromAbilitySystem(ASC);
			}
			if (World)
			{
				World->DestroyWorld(false);
				if (bHasWorldContext) { Engine->DestroyWorldContext(World); }
				if (UPackage* Package = World->GetPackage()) { Package->SetDirtyFlag(false); }
			}
		}
	};

	bool CheckRuntimeAttributes(FAutomationTestBase& Test, const TCHAR* CaseName,
		const UGGYGOAbilitySystemComponent* ASC, const TArray<const UAttributeSet*>& Expected)
	{
		const TArray<UAttributeSet*>& Actual = ASC->GetSpawnedAttributes();
		bool bPassed = Test.TestEqual(FString::Printf(TEXT("%s: live set count"), CaseName), Actual.Num(), Expected.Num());
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: live pointer at %d"), CaseName, Index),
				Actual.IsValidIndex(Index) && Actual[Index] == Expected[Index]);
		}
		return bPassed;
	}

	bool CheckRuntimeHandles(FAutomationTestBase& Test, const TCHAR* CaseName,
		const FGGYGOAbilitySetAttributeSafetyHandlesView& Handles, const TArray<const UAttributeSet*>& Expected)
	{
		bool bPassed = Test.TestEqual(FString::Printf(TEXT("%s: handle count"), CaseName),
			Handles.GetAttributeSetCountForTest(), Expected.Num());
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: handle pointer at %d"), CaseName, Index),
				Handles.GetAttributeSetForTest(Index) == Expected[Index]);
		}
		return bPassed;
	}

	bool CheckSingleRuntimeGrant(FAutomationTestBase& Test, const TCHAR* CaseName,
		const FAttributeAdmissionTestFixture& Fixture, UClass* SetClass, const UAttributeSet*& OutNewSet)
	{
		OutNewSet = nullptr;
		TArray<const UAttributeSet*> NewSets;
		for (const UAttributeSet* Set : Fixture.ASC->GetSpawnedAttributes())
		{
			if (!Fixture.Baseline.Contains(Set)) { NewSets.Add(Set); }
		}
		bool bPassed = Test.TestEqual(FString::Printf(TEXT("%s: exactly one new live object"), CaseName), NewSets.Num(), 1);
		if (NewSets.Num() != 1) { return false; }
		OutNewSet = NewSets[0];
		if (!Test.TestTrue(FString::Printf(TEXT("%s: valid new object"), CaseName), IsValid(OutNewSet)))
		{
			return false;
		}
		bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: exact new class"), CaseName), OutNewSet->GetClass() == SetClass);
		bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: new Outer is original host"), CaseName), OutNewSet->GetOuter() == Fixture.Host);
		bPassed &= Test.TestFalse(FString::Printf(TEXT("%s: new object is not a default subobject"), CaseName),
			OutNewSet->HasAnyFlags(RF_DefaultSubObject));
		bPassed &= Test.TestTrue(FString::Printf(TEXT("%s: public lookup returns new object"), CaseName),
			Fixture.ASC->GetAttributeSet(SetClass) == OutNewSet);
		TArray<const UAttributeSet*> Expected = Fixture.Baseline;
		Expected.Add(OutNewSet);
		bPassed &= CheckRuntimeAttributes(Test, CaseName, Fixture.ASC, Expected);
		bPassed &= CheckRuntimeHandles(Test, CaseName, Fixture.FirstHandles, {OutNewSet});
		bPassed &= CheckRuntimeHandles(Test, TEXT("unused second batch"), Fixture.SecondHandles, {});
		return bPassed;
	}

	void ExpectRuntimeSameClassRejection(FAutomationTestBase& Test,
		const UGGYGOAbilitySetAttributeSafetyTestAsset* Asset, UClass* SetClass, const UAttributeSet* ExistingSet)
	{
		const FString Message = FString::Printf(
			TEXT("AbilitySet [%s] 的 GrantedAttributes[0] [%s] 与ASC已有属性集 [%s] 类 [%s] 冲突：同一属性集类重复配置，已跳过。"),
			*GetNameSafe(Asset), *GetNameSafe(SetClass), *GetNameSafe(ExistingSet), *GetNameSafe(ExistingSet->GetClass()));
		Test.AddExpectedMessage(Message, ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, 1, false);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAbilitySetAttributeConfigValidationTest,
	"GGYGO.AbilitySystem.AbilitySet.AttributeConfigValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAbilitySetAttributeConfigValidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const auto ValidateCase = [this](const TCHAR* CaseName,
		const TArray<TSubclassOf<UAttributeSet>>& SetClasses, EDataValidationResult ExpectedResult,
		const TArray<FString>& ExpectedErrors)
	{
		TStrongObjectPtr<UGGYGOAbilitySetAttributeSafetyTestAsset> Asset(
			NewObject<UGGYGOAbilitySetAttributeSafetyTestAsset>(GetTransientPackage()));
		if (!TestNotNull(FString::Printf(TEXT("%s: transient asset"), CaseName), Asset.Get()))
		{
			return false;
		}
		Asset->SetAttributeClassesForTest(SetClasses);

		FDataValidationContext Context;
		const EDataValidationResult Result = Asset->IsDataValid(Context);
		TArray<FText> Warnings;
		TArray<FText> Errors;
		Context.SplitIssues(Warnings, Errors);

		bool bCasePassed = TestEqual(FString::Printf(TEXT("%s: validation result"), CaseName),
			static_cast<int32>(Result), static_cast<int32>(ExpectedResult));
		bCasePassed &= TestEqual(FString::Printf(TEXT("%s: context error count"), CaseName),
			Context.GetNumErrors(), static_cast<uint32>(ExpectedErrors.Num()));
		bCasePassed &= TestEqual(FString::Printf(TEXT("%s: extracted error count"), CaseName),
			Errors.Num(), ExpectedErrors.Num());
		bCasePassed &= TestEqual(FString::Printf(TEXT("%s: context warning count"), CaseName),
			Context.GetNumWarnings(), uint32(0));
		bCasePassed &= TestEqual(FString::Printf(TEXT("%s: extracted warning count"), CaseName),
			Warnings.Num(), 0);
		for (int32 ErrorIndex = 0; ErrorIndex < ExpectedErrors.Num(); ++ErrorIndex)
		{
			if (Errors.IsValidIndex(ErrorIndex))
			{
				// Full equality verifies actual configuration indices, classes and conflict reason.
				bCasePassed &= TestEqual(FString::Printf(TEXT("%s: diagnostic %d"), CaseName, ErrorIndex),
					Errors[ErrorIndex].ToString(), ExpectedErrors[ErrorIndex]);
			}
		}
		return bCasePassed;
	};

	UClass* Shared = UGGYGOAbilitySetAttributeSafetySharedSet::StaticClass();
	UClass* SharedA = UGGYGOAbilitySetAttributeSafetySharedSiblingA::StaticClass();
	UClass* SharedB = UGGYGOAbilitySetAttributeSafetySharedSiblingB::StaticClass();
	UClass* IndependentA = UGGYGOAbilitySetAttributeSafetyIndependentA::StaticClass();
	UClass* IndependentB = UGGYGOAbilitySetAttributeSafetyIndependentB::StaticClass();
	UClass* Abstract = UGGYGOAbilitySetAttributeSafetyAbstractSet::StaticClass();
	const TSubclassOf<UAttributeSet> NullClass;
	const FString SameClassReason = TEXT("同一属性集类重复配置");
	const FString InheritanceReason = TEXT("父子继承关系导致属性集查找不唯一");
	const FString SharedFieldReason = FString::Printf(TEXT("共享属性字段 [%s.SharedValue] 导致重复存储"),
		*GetNameSafe(Shared));

	bool bPassed = true;
	bPassed &= ValidateCase(TEXT("Empty"), {}, EDataValidationResult::Valid, {});
	bPassed &= ValidateCase(TEXT("SingleConcrete"), {Shared}, EDataValidationResult::Valid, {});
	bPassed &= ValidateCase(TEXT("NullAtIndex1"), {IndependentA, NullClass, IndependentB},
		EDataValidationResult::Invalid, {ExpectedInvalidClassError(1, nullptr)});
	bPassed &= ValidateCase(TEXT("AbstractAtIndex2"), {IndependentA, IndependentB, Abstract},
		EDataValidationResult::Invalid, {ExpectedInvalidClassError(2, Abstract)});
	bPassed &= ValidateCase(TEXT("TwoInvalidIndices"), {IndependentA, NullClass, IndependentB, Abstract},
		EDataValidationResult::Invalid, {ExpectedInvalidClassError(1, nullptr), ExpectedInvalidClassError(3, Abstract)});
	bPassed &= ValidateCase(TEXT("DuplicateAtIndices1And2"), {IndependentA, Shared, Shared},
		EDataValidationResult::Invalid, {ExpectedConflictError(1, Shared, 2, Shared, SameClassReason)});
	bPassed &= ValidateCase(TEXT("ParentThenChild"), {Shared, SharedA},
		EDataValidationResult::Invalid, {ExpectedConflictError(0, Shared, 1, SharedA, InheritanceReason)});
	bPassed &= ValidateCase(TEXT("ChildThenParent"), {SharedA, Shared},
		EDataValidationResult::Invalid, {ExpectedConflictError(0, SharedA, 1, Shared, InheritanceReason)});
	bPassed &= ValidateCase(TEXT("SharedSiblingIndices0And2"), {SharedA, IndependentA, SharedB},
		EDataValidationResult::Invalid, {ExpectedConflictError(0, SharedA, 2, SharedB, SharedFieldReason)});
	bPassed &= ValidateCase(TEXT("EmptyAncestorIndependentSiblings"), {IndependentA, IndependentB},
		EDataValidationResult::Valid, {});
	bPassed &= ValidateCase(TEXT("ProductionHealthAndCombat"),
		{UGGYGOHealthSet::StaticClass(), UGGYGOCombatSet::StaticClass()}, EDataValidationResult::Valid, {});
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAbilitySetAttributeRuntimeAdmissionTest,
	"GGYGO.AbilitySystem.AbilitySet.AttributeRuntimeAdmissionAndNewHandles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAbilitySetAttributeRuntimeAdmissionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UClass* Independent = UGGYGOAbilitySetAttributeSafetyIndependentA::StaticClass();
	bool bPassed = true;
	{
		FAttributeAdmissionTestFixture Fixture;
		if (!Fixture.Initialize(*this)) { return false; }
		Fixture.FirstAsset->SetAttributeClassesForTest({Independent});
		Fixture.FirstAsset->GiveToAbilitySystem(Fixture.ASC, &Fixture.FirstHandles);
		const UAttributeSet* NewSet = nullptr;
		if (!CheckSingleRuntimeGrant(*this, TEXT("LegalIndependent"), Fixture, Independent, NewSet)) { return false; }
	}
	{
		FAttributeAdmissionTestFixture Fixture;
		if (!Fixture.Initialize(*this)) { return false; }
		Fixture.FirstAsset->SetAttributeClassesForTest({Independent});
		Fixture.FirstAsset->GiveToAbilitySystem(Fixture.ASC, &Fixture.FirstHandles);
		const UAttributeSet* NewSet = nullptr;
		if (!CheckSingleRuntimeGrant(*this, TEXT("SameAssetFirstGive"), Fixture, Independent, NewSet)) { return false; }
		TArray<const UAttributeSet*> Expected = Fixture.Baseline;
		Expected.Add(NewSet);

		ExpectRuntimeSameClassRejection(*this, Fixture.FirstAsset.Get(), Independent, NewSet);
		Fixture.FirstAsset->GiveToAbilitySystem(Fixture.ASC, &Fixture.SecondHandles);
		bPassed &= CheckRuntimeAttributes(*this, TEXT("SameAssetRepeatedGive"), Fixture.ASC, Expected);
		bPassed &= CheckRuntimeHandles(*this, TEXT("SameAssetFirstBatchRetained"), Fixture.FirstHandles, {NewSet});
		bPassed &= CheckRuntimeHandles(*this, TEXT("SameAssetSecondBatchEmpty"), Fixture.SecondHandles, {});
	}
	{
		FAttributeAdmissionTestFixture Fixture;
		if (!Fixture.Initialize(*this)) { return false; }
		Fixture.FirstAsset->SetAttributeClassesForTest({Independent});
		Fixture.SecondAsset->SetAttributeClassesForTest({Independent});
		Fixture.FirstAsset->GiveToAbilitySystem(Fixture.ASC, &Fixture.FirstHandles);
		const UAttributeSet* NewSet = nullptr;
		if (!CheckSingleRuntimeGrant(*this, TEXT("CrossAssetFirstGive"), Fixture, Independent, NewSet)) { return false; }
		TArray<const UAttributeSet*> Expected = Fixture.Baseline;
		Expected.Add(NewSet);

		ExpectRuntimeSameClassRejection(*this, Fixture.SecondAsset.Get(), Independent, NewSet);
		Fixture.SecondAsset->GiveToAbilitySystem(Fixture.ASC, &Fixture.SecondHandles);
		bPassed &= CheckRuntimeAttributes(*this, TEXT("CrossAssetRepeatedClass"), Fixture.ASC, Expected);
		bPassed &= CheckRuntimeHandles(*this, TEXT("CrossAssetFirstBatchRetained"), Fixture.FirstHandles, {NewSet});
		bPassed &= CheckRuntimeHandles(*this, TEXT("CrossAssetSecondBatchEmpty"), Fixture.SecondHandles, {});
	}
	{
		FAttributeAdmissionTestFixture Fixture;
		if (!Fixture.Initialize(*this)) { return false; }
		Fixture.FirstAsset->SetAttributeClassesForTest({UGGYGOHealthSet::StaticClass()});
		Fixture.SecondAsset->SetAttributeClassesForTest({UGGYGOCombatSet::StaticClass()});

		ExpectRuntimeSameClassRejection(*this, Fixture.FirstAsset.Get(), UGGYGOHealthSet::StaticClass(), Fixture.HealthSet);
		Fixture.FirstAsset->GiveToAbilitySystem(Fixture.ASC, &Fixture.FirstHandles);
		bPassed &= CheckRuntimeAttributes(*this, TEXT("DefaultHealthConflict"), Fixture.ASC, Fixture.Baseline);
		bPassed &= CheckRuntimeHandles(*this, TEXT("DefaultHealthBatchEmpty"), Fixture.FirstHandles, {});
		bPassed &= CheckRuntimeHandles(*this, TEXT("DefaultHealthOtherBatchEmpty"), Fixture.SecondHandles, {});

		ExpectRuntimeSameClassRejection(*this, Fixture.SecondAsset.Get(), UGGYGOCombatSet::StaticClass(), Fixture.CombatSet);
		Fixture.SecondAsset->GiveToAbilitySystem(Fixture.ASC, &Fixture.SecondHandles);
		bPassed &= CheckRuntimeAttributes(*this, TEXT("DefaultCombatConflict"), Fixture.ASC, Fixture.Baseline);
		bPassed &= CheckRuntimeHandles(*this, TEXT("DefaultCombatOtherBatchEmpty"), Fixture.FirstHandles, {});
		bPassed &= CheckRuntimeHandles(*this, TEXT("DefaultCombatBatchEmpty"), Fixture.SecondHandles, {});
	}
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAbilitySetAttributeTakeOwnershipTest,
	"GGYGO.AbilitySystem.AbilitySet.AttributeTakeOwnershipAndRepeatedTake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAbilitySetAttributeTakeOwnershipTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Declared before the fixture: observation references outlive its entire teardown.
	TStrongObjectPtr<UAttributeSet> KeepA{nullptr};
	TStrongObjectPtr<UAttributeSet> KeepB{nullptr};
	FAttributeAdmissionTestFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	UClass* ClassA = UGGYGOAbilitySetAttributeSafetyIndependentA::StaticClass();
	UClass* ClassB = UGGYGOAbilitySetAttributeSafetyIndependentB::StaticClass();

	const auto CheckContext = [this, &Fixture](const TCHAR* Stage)
	{
		bool bPassed = TestTrue(FString::Printf(TEXT("%s: valid original host"), Stage), IsValid(Fixture.Host));
		bPassed &= TestTrue(FString::Printf(TEXT("%s: valid original ASC"), Stage), IsValid(Fixture.ASC));
		if (!bPassed) { return false; }
		bPassed &= TestFalse(FString::Printf(TEXT("%s: host not destroying"), Stage), Fixture.Host->IsActorBeingDestroyed());
		bPassed &= TestFalse(FString::Printf(TEXT("%s: ASC not destroying"), Stage), Fixture.ASC->IsBeingDestroyed());
		bPassed &= TestTrue(FString::Printf(TEXT("%s: host initialized"), Stage), Fixture.Host->IsActorInitialized());
		bPassed &= TestTrue(FString::Printf(TEXT("%s: ASC registered and initialized"), Stage),
			Fixture.ASC->IsRegistered() && Fixture.ASC->HasBeenInitialized());
		bPassed &= TestTrue(FString::Printf(TEXT("%s: original World"), Stage),
			Fixture.Host->GetWorld() == Fixture.World && Fixture.ASC->GetWorld() == Fixture.World);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: original component Owner"), Stage), Fixture.ASC->GetOwner() == Fixture.Host);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: original ActorInfo Owner"), Stage), Fixture.ASC->GetOwnerActor() == Fixture.Host);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: host authority"), Stage),
			Fixture.Host->HasAuthority() && Fixture.Host->GetLocalRole() == ROLE_Authority);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: ASC authority"), Stage), Fixture.ASC->IsOwnerActorAuthoritative());
		return bPassed;
	};

	const auto CheckState = [this, &Fixture, &KeepA, &KeepB, ClassA, ClassB, &CheckContext](
		const TCHAR* Stage, const TArray<const UAttributeSet*>& ExpectedSets,
		const TArray<const UAttributeSet*>& ExpectedFirst, const TArray<const UAttributeSet*>& ExpectedSecond,
		const UAttributeSet* ExpectedA, const UAttributeSet* ExpectedB)
	{
		if (!CheckContext(Stage)) { return false; }
		bool bPassed = CheckRuntimeAttributes(*this, Stage, Fixture.ASC, ExpectedSets);
		bPassed &= CheckRuntimeHandles(*this, *FString::Printf(TEXT("%s first batch"), Stage), Fixture.FirstHandles, ExpectedFirst);
		bPassed &= CheckRuntimeHandles(*this, *FString::Printf(TEXT("%s second batch"), Stage), Fixture.SecondHandles, ExpectedSecond);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: public Health lookup"), Stage),
			Fixture.ASC->GetAttributeSet(UGGYGOHealthSet::StaticClass()) == Fixture.HealthSet);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: public Combat lookup"), Stage),
			Fixture.ASC->GetAttributeSet(UGGYGOCombatSet::StaticClass()) == Fixture.CombatSet);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: public A lookup"), Stage), Fixture.ASC->GetAttributeSet(ClassA) == ExpectedA);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: public B lookup"), Stage), Fixture.ASC->GetAttributeSet(ClassB) == ExpectedB);
		for (int32 Index = 0; Index < ExpectedSets.Num(); ++Index)
		{
			const UAttributeSet* Set = ExpectedSets[Index];
			if (!TestTrue(FString::Printf(TEXT("%s: valid live object %d"), Stage, Index), IsValid(Set)))
			{
				bPassed = false;
				continue;
			}
			UClass* ExpectedClass = nullptr;
			const bool bExpectedDefault = Set == Fixture.HealthSet || Set == Fixture.CombatSet;
			if (Set == Fixture.HealthSet) { ExpectedClass = UGGYGOHealthSet::StaticClass(); }
			else if (Set == Fixture.CombatSet) { ExpectedClass = UGGYGOCombatSet::StaticClass(); }
			else if (Set == KeepA.Get()) { ExpectedClass = ClassA; }
			else if (Set == KeepB.Get()) { ExpectedClass = ClassB; }
			bPassed &= TestTrue(FString::Printf(TEXT("%s: exact live class %d"), Stage, Index), Set->GetClass() == ExpectedClass);
			bPassed &= TestTrue(FString::Printf(TEXT("%s: original live Outer %d"), Stage, Index), Set->GetOuter() == Fixture.Host);
			bPassed &= TestEqual(FString::Printf(TEXT("%s: default subobject flag %d"), Stage, Index),
				Set->HasAnyFlags(RF_DefaultSubObject), bExpectedDefault);
		}
		return bPassed;
	};

	if (!CheckState(TEXT("BeforeGiveA"), Fixture.Baseline, {}, {}, nullptr, nullptr)) { return false; }
	Fixture.FirstAsset->SetAttributeClassesForTest({ClassA});
	Fixture.FirstAsset->GiveToAbilitySystem(Fixture.ASC, &Fixture.FirstHandles);
	if (!CheckContext(TEXT("AfterGiveA"))) { return false; }
	const UAttributeSet* ObservedA = nullptr;
	if (!CheckSingleRuntimeGrant(*this, TEXT("TakeFirstGrant"), Fixture, ClassA, ObservedA)) { return false; }
	for (UAttributeSet* Set : Fixture.ASC->GetSpawnedAttributes())
	{
		if (Set == ObservedA) { KeepA.Reset(Set); break; }
	}
	if (!TestTrue(TEXT("A observation pins actual live object"), IsValid(KeepA.Get()))) { return false; }
	TArray<const UAttributeSet*> AfterA = Fixture.Baseline;
	AfterA.Add(KeepA.Get());
	if (!CheckState(TEXT("AfterGiveA"), AfterA, {KeepA.Get()}, {}, KeepA.Get(), nullptr)) { return false; }

	Fixture.SecondAsset->SetAttributeClassesForTest({ClassB});
	if (!CheckContext(TEXT("BeforeGiveB"))) { return false; }
	Fixture.SecondAsset->GiveToAbilitySystem(Fixture.ASC, &Fixture.SecondHandles);
	if (!CheckContext(TEXT("AfterGiveB"))) { return false; }
	TArray<UAttributeSet*> AddedB;
	for (UAttributeSet* Set : Fixture.ASC->GetSpawnedAttributes())
	{
		if (!AfterA.Contains(Set)) { AddedB.Add(Set); }
	}
	if (!TestEqual(TEXT("B has exactly one actual new live object"), AddedB.Num(), 1)) { return false; }
	if (!TestTrue(TEXT("valid actual new B object"), IsValid(AddedB[0]))) { return false; }
	KeepB.Reset(AddedB[0]);
	if (!TestTrue(TEXT("A and B are different actual objects"), KeepA.Get() != KeepB.Get())) { return false; }
	TArray<const UAttributeSet*> AfterBoth = AfterA;
	AfterBoth.Add(KeepB.Get());
	if (!CheckState(TEXT("AfterGiveB"), AfterBoth, {KeepA.Get()}, {KeepB.Get()}, KeepA.Get(), KeepB.Get())) { return false; }

	if (!CheckContext(TEXT("BeforeTakeFirstBatch"))) { return false; }
	Fixture.FirstHandles.TakeFromAbilitySystem(Fixture.ASC);
	TArray<const UAttributeSet*> AfterTakeA = Fixture.Baseline;
	AfterTakeA.Add(KeepB.Get());
	if (!CheckState(TEXT("AfterTakeFirstBatch"), AfterTakeA, {}, {KeepB.Get()}, nullptr, KeepB.Get())) { return false; }

	if (!CheckContext(TEXT("BeforeRepeatedTakeFirstBatch"))) { return false; }
	Fixture.FirstHandles.TakeFromAbilitySystem(Fixture.ASC);
	if (!CheckState(TEXT("AfterRepeatedTakeFirstBatch"), AfterTakeA, {}, {KeepB.Get()}, nullptr, KeepB.Get())) { return false; }

	if (!CheckContext(TEXT("BeforeTakeSecondBatch"))) { return false; }
	Fixture.SecondHandles.TakeFromAbilitySystem(Fixture.ASC);
	if (!CheckState(TEXT("AfterTakeSecondBatch"), Fixture.Baseline, {}, {}, nullptr, nullptr)) { return false; }
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAbilitySetAttributePartialFailureTest,
	"GGYGO.AbilitySystem.AbilitySet.AttributePartialFailureContinuesAndNewHandles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAbilitySetAttributePartialFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// Declared before the fixture: observation references outlive its entire teardown.
	TStrongObjectPtr<UAttributeSet> KeepA{nullptr};
	TStrongObjectPtr<UAttributeSet> KeepB{nullptr};
	FAttributeAdmissionTestFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	UClass* ClassA = UGGYGOAbilitySetAttributeSafetyIndependentA::StaticClass();
	UClass* ClassB = UGGYGOAbilitySetAttributeSafetyIndependentB::StaticClass();
	UClass* HealthClass = UGGYGOHealthSet::StaticClass();

	const auto CheckContext = [this, &Fixture](const TCHAR* Stage)
	{
		bool bPassed = TestTrue(FString::Printf(TEXT("%s: valid original host"), Stage), IsValid(Fixture.Host));
		bPassed &= TestTrue(FString::Printf(TEXT("%s: valid original ASC"), Stage), IsValid(Fixture.ASC));
		if (!bPassed) { return false; }
		bPassed &= TestFalse(FString::Printf(TEXT("%s: host not destroying"), Stage), Fixture.Host->IsActorBeingDestroyed());
		bPassed &= TestFalse(FString::Printf(TEXT("%s: ASC not destroying"), Stage), Fixture.ASC->IsBeingDestroyed());
		bPassed &= TestTrue(FString::Printf(TEXT("%s: host initialized"), Stage), Fixture.Host->IsActorInitialized());
		bPassed &= TestTrue(FString::Printf(TEXT("%s: ASC registered and initialized"), Stage),
			Fixture.ASC->IsRegistered() && Fixture.ASC->HasBeenInitialized());
		bPassed &= TestTrue(FString::Printf(TEXT("%s: original World"), Stage),
			Fixture.Host->GetWorld() == Fixture.World && Fixture.ASC->GetWorld() == Fixture.World);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: original component Owner"), Stage), Fixture.ASC->GetOwner() == Fixture.Host);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: original ActorInfo Owner"), Stage), Fixture.ASC->GetOwnerActor() == Fixture.Host);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: host authority"), Stage),
			Fixture.Host->HasAuthority() && Fixture.Host->GetLocalRole() == ROLE_Authority);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: ASC authority"), Stage), Fixture.ASC->IsOwnerActorAuthoritative());
		return bPassed;
	};

	const auto CheckState = [this, &Fixture, &KeepA, &KeepB, ClassA, ClassB, &CheckContext](
		const TCHAR* Stage, const TArray<const UAttributeSet*>& ExpectedSets,
		const TArray<const UAttributeSet*>& ExpectedFirst, const UAttributeSet* ExpectedA,
		const UAttributeSet* ExpectedB)
	{
		if (!CheckContext(Stage)) { return false; }
		bool bPassed = CheckRuntimeAttributes(*this, Stage, Fixture.ASC, ExpectedSets);
		bPassed &= CheckRuntimeHandles(*this, *FString::Printf(TEXT("%s first batch"), Stage), Fixture.FirstHandles, ExpectedFirst);
		bPassed &= CheckRuntimeHandles(*this, *FString::Printf(TEXT("%s unused second batch"), Stage), Fixture.SecondHandles, {});
		bPassed &= TestTrue(FString::Printf(TEXT("%s: public Health lookup"), Stage),
			Fixture.ASC->GetAttributeSet(UGGYGOHealthSet::StaticClass()) == Fixture.HealthSet);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: public Combat lookup"), Stage),
			Fixture.ASC->GetAttributeSet(UGGYGOCombatSet::StaticClass()) == Fixture.CombatSet);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: public A lookup"), Stage), Fixture.ASC->GetAttributeSet(ClassA) == ExpectedA);
		bPassed &= TestTrue(FString::Printf(TEXT("%s: public B lookup"), Stage), Fixture.ASC->GetAttributeSet(ClassB) == ExpectedB);
		for (int32 Index = 0; Index < ExpectedSets.Num(); ++Index)
		{
			const UAttributeSet* Set = ExpectedSets[Index];
			if (!TestTrue(FString::Printf(TEXT("%s: valid live object %d"), Stage, Index), IsValid(Set)))
			{
				bPassed = false;
				continue;
			}
			UClass* ExpectedClass = nullptr;
			const bool bExpectedDefault = Set == Fixture.HealthSet || Set == Fixture.CombatSet;
			if (Set == Fixture.HealthSet) { ExpectedClass = UGGYGOHealthSet::StaticClass(); }
			else if (Set == Fixture.CombatSet) { ExpectedClass = UGGYGOCombatSet::StaticClass(); }
			else if (Set == KeepA.Get()) { ExpectedClass = ClassA; }
			else if (Set == KeepB.Get()) { ExpectedClass = ClassB; }
			bPassed &= TestTrue(FString::Printf(TEXT("%s: exact live class %d"), Stage, Index), Set->GetClass() == ExpectedClass);
			bPassed &= TestTrue(FString::Printf(TEXT("%s: original live Outer %d"), Stage, Index), Set->GetOuter() == Fixture.Host);
			bPassed &= TestEqual(FString::Printf(TEXT("%s: default subobject flag %d"), Stage, Index),
				Set->HasAnyFlags(RF_DefaultSubObject), bExpectedDefault);
		}
		return bPassed;
	};

	if (!CheckState(TEXT("BeforePartialGive"), Fixture.Baseline, {}, nullptr, nullptr)) { return false; }
	Fixture.FirstAsset->SetAttributeClassesForTest({ClassA, HealthClass, ClassB});
	if (!CheckContext(TEXT("BeforePartialGiveOperation"))) { return false; }
	const FString ExpectedRejection = FString::Printf(
		TEXT("AbilitySet [%s] 的 GrantedAttributes[1] [%s] 与ASC已有属性集 [%s] 类 [%s] 冲突：同一属性集类重复配置，已跳过。"),
		*GetNameSafe(Fixture.FirstAsset.Get()), *GetNameSafe(HealthClass),
		*GetNameSafe(Fixture.HealthSet), *GetNameSafe(Fixture.HealthSet->GetClass()));
	AddExpectedMessage(ExpectedRejection, ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, 1, false);
	Fixture.FirstAsset->GiveToAbilitySystem(Fixture.ASC, &Fixture.FirstHandles);
	if (!CheckContext(TEXT("AfterPartialGive"))) { return false; }

	TArray<UAttributeSet*> NewSets;
	for (UAttributeSet* Set : Fixture.ASC->GetSpawnedAttributes())
	{
		if (!Fixture.Baseline.Contains(Set)) { NewSets.Add(Set); }
	}
	if (!TestEqual(TEXT("partial Give has exactly two actual new live objects"), NewSets.Num(), 2)) { return false; }
	bool bNewSetsValid = TestTrue(TEXT("valid actual new A object"), IsValid(NewSets[0]));
	bNewSetsValid &= TestTrue(TEXT("valid actual new B object"), IsValid(NewSets[1]));
	if (!bNewSetsValid) { return false; }
	KeepA.Reset(NewSets[0]);
	KeepB.Reset(NewSets[1]);
	if (!TestTrue(TEXT("partial Give creates distinct A and B objects"), KeepA.Get() != KeepB.Get())) { return false; }

	TArray<const UAttributeSet*> AfterPartialGive = Fixture.Baseline;
	AfterPartialGive.Add(KeepA.Get());
	AfterPartialGive.Add(KeepB.Get());
	if (!CheckState(TEXT("AfterPartialGive"), AfterPartialGive, {KeepA.Get(), KeepB.Get()}, KeepA.Get(), KeepB.Get()))
	{
		return false;
	}

	if (!CheckContext(TEXT("BeforeTakePartialBatch"))) { return false; }
	Fixture.FirstHandles.TakeFromAbilitySystem(Fixture.ASC);
	if (!CheckState(TEXT("AfterTakePartialBatch"), Fixture.Baseline, {}, nullptr, nullptr)) { return false; }
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
