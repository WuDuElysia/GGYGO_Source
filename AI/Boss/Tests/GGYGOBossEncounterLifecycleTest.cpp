#include "AI/Boss/Tests/GGYGOBossEncounterLifecycleTestTypes.h"

#include "AI/Boss/GGYGOBossAIController.h"
#include "AI/Boss/GGYGOBossDefinition.h"
#include "AI/Boss/GGYGOBossState.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossEncounterLifecycleTestTypes)

void AGGYGOEncounterLifecycleTestEncounter::HandleAvatarInitializationForTest()
{
	if (bCleanupDuringAvatarInitialization)
	{
		bInitializationCallbackRan = true;
		CleanupForTest();
		bInitializationSpawnRejected = !SpawnBoss();
	}
}

void AGGYGOEncounterLifecycleTestPawn::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	if (AGGYGOEncounterLifecycleTestEncounter* Encounter =
		Cast<AGGYGOEncounterLifecycleTestEncounter>(GetOwner()))
	{
		Encounter->HandleAvatarInitializationForTest();
	}
}

void AGGYGOEncounterLifecycleTestPawn::UnPossessed()
{
	Super::UnPossessed();
	if (Observer)
	{
		Observer->HandleUnpossessed();
	}
}

void UGGYGOEncounterLifecycleObserver::HandleStop()
{
	++StopCount;
	Events.Add(TEXT("Stop"));
	bReferencesEmptyAtStop = !Encounter->GetBossState() && !Encounter->GetBossController()
		&& !Encounter->GetBossAvatar();
	bStillBoundAtStop = Controller->GetPawn() == CurrentAvatar && State->GetAvatarPawn() == CurrentAvatar
		&& ASC->GetAvatarActor() == CurrentAvatar
		&& CurrentAvatar->GetPawnExtensionComponent()->GetGGYGOAbilitySystemComponent() == ASC;
	bCreatedActorsAliveAtStop = IsValid(State) && !State->IsActorBeingDestroyed()
		&& IsValid(Controller) && !Controller->IsActorBeingDestroyed()
		&& IsValid(CreatedAvatar) && !CreatedAvatar->IsActorBeingDestroyed();
	if (bReenterEndPlay)
	{
		Encounter->EndPlayForTest();
	}
	Encounter->CleanupForTest();
	bSpawnRejectedDuringCleanup = !Encounter->SpawnBoss();
}

void UGGYGOEncounterLifecycleObserver::HandleUnpossessed()
{
	Events.Add(TEXT("Unpossess"));
}

void UGGYGOEncounterLifecycleObserver::HandleUninitialized()
{
	++UninitializedCount;
	Events.Add(TEXT("Detach"));
	bDetachedBeforeDestroy = !Controller->GetPawn() && !CurrentAvatar->GetController()
		&& !ASC->GetAvatarActor()
		&& !CurrentAvatar->GetPawnExtensionComponent()->GetGGYGOAbilitySystemComponent()
		&& !State->IsActorBeingDestroyed() && !Controller->IsActorBeingDestroyed()
		&& !CreatedAvatar->IsActorBeingDestroyed();
}

void UGGYGOEncounterLifecycleObserver::HandleDestroyed(AActor* Actor)
{
	if (Actor == CreatedAvatar.Get()) { Events.Add(TEXT("DestroyAvatar")); }
	else if (Actor == Controller.Get()) { Events.Add(TEXT("DestroyController")); }
	else if (Actor == State.Get()) { Events.Add(TEXT("DestroyState")); }
}

void UGGYGOEncounterLifecycleTestBrain::StopLogic(const FString& Reason)
{
	if (Observer) { Observer->HandleStop(); }
	Super::StopLogic(Reason);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	struct FEncounterTestWorld
	{
		UWorld* World = nullptr;
		UEngine* Engine = nullptr;
		TArray<TPair<TWeakObjectPtr<UGGYGOPawnExtensionComponent>, FDelegateHandle>> LocalNoticeHandles;
		bool Initialize()
		{
			Engine = GEngine;
			if (!Engine) { return false; }
			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Init);
			if (!World) { return false; }
			Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			// Enable PostInitializeComponents for real Spawn/FinishSpawning without BeginPlay.
			World->InitializeActorsForPlay(FURL());
			return World->AreActorsInitialized() && !World->HasBegunPlay();
		}
		~FEncounterTestWorld()
		{
			for (const auto& Registration : LocalNoticeHandles)
			{
				if (UGGYGOPawnExtensionComponent* Extension = Registration.Key.Get())
				{
					Extension->UnregisterLocalAbilitySystemNotice(Registration.Value);
				}
			}
			if (World)
			{
				// No BeginPlay: explicitly release production resources before DestroyWorld.
				for (TActorIterator<AGGYGOEncounterLifecycleTestEncounter> It(World); It; ++It)
				{
					It->CleanupForTest();
				}
				World->DestroyWorld(false);
				Engine->DestroyWorldContext(World);
				if (UPackage* Package = World->GetPackage()) { Package->SetDirtyFlag(false); }
			}
		}
	};

	struct FEncounterTestConfig
	{
		TStrongObjectPtr<UGGYGOPawnData> PawnData{NewObject<UGGYGOPawnData>(GetTransientPackage())};
		TStrongObjectPtr<UGGYGOBossDefinition> Definition{NewObject<UGGYGOBossDefinition>(GetTransientPackage())};
		FEncounterTestConfig()
		{
			PawnData->PawnClass = AGGYGOEncounterLifecycleTestPawn::StaticClass();
			Definition->InitialFormTag = GGYGOGameplayTags::State_Boss_Form_Default;
			Definition->InitialPhaseTag = GGYGOGameplayTags::State_Boss_Phase_One;
			FGGYGOBossFormDefinition Form;
			Form.FormTag = Definition->InitialFormTag;
			Form.AvatarPawnData = PawnData.Get();
			Definition->Forms.Add(Form);
			FGGYGOBossPhaseDefinition Phase;
			Phase.PhaseTag = Definition->InitialPhaseTag;
			Definition->Phases.Add(Phase);
		}
	};

	bool ReferencesEmpty(const AGGYGOEncounterLifecycleTestEncounter* Encounter)
	{
		return !Encounter->GetBossState() && !Encounter->GetBossController() && !Encounter->GetBossAvatar();
	}

	template <typename T>
	int32 CountLiveActors(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<T> It(World); It; ++It)
		{
			if (IsValid(*It) && !It->IsActorBeingDestroyed()) { ++Count; }
		}
		return Count;
	}

	void ObserveCleanup(FEncounterTestWorld& Fixture, UGGYGOEncounterLifecycleObserver* Observer,
		AGGYGOEncounterLifecycleTestEncounter* Encounter, AGGYGOEncounterLifecycleTestPawn* CurrentAvatar)
	{
		Observer->Encounter = Encounter;
		Observer->State = Encounter->GetBossState();
		Observer->Controller = Encounter->GetBossController();
		Observer->CreatedAvatar = Encounter->GetBossAvatar();
		Observer->CurrentAvatar = CurrentAvatar;
		Observer->ASC = Observer->State->GetGGYGOAbilitySystemComponent();
		CurrentAvatar->Observer = Observer;
		UGGYGOPawnExtensionComponent* Extension = CurrentAvatar->GetPawnExtensionComponent();
		const FDelegateHandle Handle = Extension->RegisterLocalAbilitySystemNoticeAndCall(
			FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateWeakLambda(Observer,
				[Observer](const FGGYGOPawnASCLocalNotice& Notice)
				{
					if (Notice.Kind == EGGYGOPawnASCLocalNoticeKind::Released) { Observer->HandleUninitialized(); }
				}));
		Fixture.LocalNoticeHandles.Emplace(TWeakObjectPtr<UGGYGOPawnExtensionComponent>(Extension), Handle);
		Observer->CreatedAvatar->OnDestroyed.AddDynamic(Observer, &UGGYGOEncounterLifecycleObserver::HandleDestroyed);
		Observer->Controller->OnDestroyed.AddDynamic(Observer, &UGGYGOEncounterLifecycleObserver::HandleDestroyed);
		Observer->State->OnDestroyed.AddDynamic(Observer, &UGGYGOEncounterLifecycleObserver::HandleDestroyed);
		UGGYGOEncounterLifecycleTestBrain* Brain = NewObject<UGGYGOEncounterLifecycleTestBrain>(Observer->Controller);
		Brain->Observer = Observer;
		Brain->RegisterComponent();
		// Public production AIController interface; no reflection or private-field access.
		Observer->Controller->BrainComponent = Brain;
	}

	void VerifyCleanupOrder(FAutomationTestBase& Test, const UGGYGOEncounterLifecycleObserver* Observer)
	{
		Test.TestEqual(TEXT("StopLogic called once, including reentrant cleanup"), Observer->StopCount, 1);
		Test.TestEqual(TEXT("uninitialization broadcast once"), Observer->UninitializedCount, 1);
		Test.TestTrue(TEXT("public references cleared before StopLogic callback"), Observer->bReferencesEmptyAtStop);
		Test.TestTrue(TEXT("StopLogic precedes UnPossess and Detach"), Observer->bStillBoundAtStop);
		Test.TestTrue(TEXT("StopLogic precedes destruction of every created object"), Observer->bCreatedActorsAliveAtStop);
		Test.TestTrue(TEXT("SpawnBoss rejected inside cleanup callback"), Observer->bSpawnRejectedDuringCleanup);
		Test.TestTrue(TEXT("Controller unpossessed and ASC detached before Destroy"), Observer->bDetachedBeforeDestroy);
		const TArray<FName> Expected = { TEXT("Stop"), TEXT("Unpossess"), TEXT("Detach"),
			TEXT("DestroyAvatar"), TEXT("DestroyController"), TEXT("DestroyState") };
		Test.TestEqual(TEXT("actual lifecycle event count"), Observer->Events.Num(), Expected.Num());
		for (int32 Index = 0; Index < FMath::Min(Observer->Events.Num(), Expected.Num()); ++Index)
		{
			Test.TestEqual(FString::Printf(TEXT("actual lifecycle event %d"), Index), Observer->Events[Index], Expected[Index]);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOEncounterCleanupLifecycleTest,
	"GGYGO.BossAI.Encounter.CleanupLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOEncounterCleanupLifecycleTest::RunTest(const FString& Parameters)
{
	FEncounterTestWorld Fixture;
	if (!TestTrue(TEXT("independent WorldContext fixture"), Fixture.Initialize())) { return false; }
	FEncounterTestConfig Config;
	AGGYGOEncounterLifecycleTestEncounter* Encounter = Fixture.World->SpawnActor<AGGYGOEncounterLifecycleTestEncounter>();
	if (!TestNotNull(TEXT("Encounter"), Encounter)) { return false; }
	Encounter->ConfigureForTest(Config.Definition.Get());
	if (!TestTrue(TEXT("real SpawnBoss succeeds"), Encounter->SpawnBoss())) { return false; }
	AGGYGOBossState* State = Encounter->GetBossState();
	AGGYGOBossAIController* Controller = Encounter->GetBossController();
	AGGYGOEncounterLifecycleTestPawn* Avatar = Cast<AGGYGOEncounterLifecycleTestPawn>(Encounter->GetBossAvatar());
	if (!TestNotNull(TEXT("State"), State) || !TestNotNull(TEXT("Controller"), Controller)
		|| !TestNotNull(TEXT("Avatar"), Avatar)) { return false; }
	TestFalse(TEXT("fixture has not begun play"), Encounter->HasActorBegunPlay());
	TestFalse(TEXT("repeat SpawnBoss rejected"), Encounter->SpawnBoss());
	TestTrue(TEXT("repeat rejection preserves actual assembly"), Encounter->GetBossState() == State
		&& Encounter->GetBossController() == Controller && Encounter->GetBossAvatar() == Avatar);
	TStrongObjectPtr<UGGYGOEncounterLifecycleObserver> Observer(NewObject<UGGYGOEncounterLifecycleObserver>());
	ObserveCleanup(Fixture, Observer.Get(), Encounter, Avatar);
	Observer->bReenterEndPlay = true;
	Encounter->CleanupForTest();
	VerifyCleanupOrder(*this, Observer.Get());
	TestTrue(TEXT("cleaned public references"), ReferencesEmpty(Encounter));
	TestTrue(TEXT("created actors destroyed"), State->IsActorBeingDestroyed()
		&& Controller->IsActorBeingDestroyed() && Avatar->IsActorBeingDestroyed());
	TestNull(TEXT("State Avatar cleared"), State->GetAvatarPawn());
	TestNull(TEXT("Controller Pawn cleared"), Controller->GetPawn());
	Encounter->CleanupForTest();
	Encounter->EndPlayForTest();
	TestEqual(TEXT("repeat cleanup/EndPlay has no extra callback"), Observer->Events.Num(), 6);
	TestFalse(TEXT("EndPlay rejects future SpawnBoss"), Encounter->SpawnBoss());

	// A valid definition reaches State creation, then a wrong Pawn class forces partial rollback.
	FEncounterTestConfig BadConfig;
	BadConfig.PawnData->PawnClass = APawn::StaticClass();
	AGGYGOEncounterLifecycleTestEncounter* Partial = Fixture.World->SpawnActor<AGGYGOEncounterLifecycleTestEncounter>();
	if (!TestNotNull(TEXT("partial failure Encounter"), Partial)) { return false; }
	Partial->ConfigureForTest(BadConfig.Definition.Get());
	int32 CreatedStates = 0;
	const FDelegateHandle SpawnWatch = Fixture.World->AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateLambda([&CreatedStates](AActor* Actor)
		{
			if (Actor->IsA<AGGYGOBossState>()) { ++CreatedStates; }
		}));
	AddExpectedError(TEXT("必须继承 AGGYGOBossCharacter"), EAutomationExpectedErrorFlags::Contains, 1);
	const bool bPartialSpawned = Partial->SpawnBoss();
	Fixture.World->RemoveOnActorSpawnedHandler(SpawnWatch);
	TestFalse(TEXT("bad Pawn class fails after State creation"), bPartialSpawned);
	TestEqual(TEXT("failure actually created a State"), CreatedStates, 1);
	TestTrue(TEXT("partial rollback leaves no public references"), ReferencesEmpty(Partial));
	TestEqual(TEXT("partial rollback leaves no State"), CountLiveActors<AGGYGOBossState>(Fixture.World), 0);
	TestEqual(TEXT("partial rollback leaves no Controller"), CountLiveActors<AGGYGOBossAIController>(Fixture.World), 0);
	TestEqual(TEXT("partial rollback leaves no Avatar"), CountLiveActors<AGGYGOBossCharacter>(Fixture.World), 0);
	Partial->ConfigureForTest(Config.Definition.Get());
	if (!TestTrue(TEXT("failed assembly can retry real SpawnBoss"), Partial->SpawnBoss())) { return false; }
	Partial->CleanupForTest();

	// Construction completion invokes real cleanup while SpawnBoss is still on the stack.
	AGGYGOEncounterLifecycleTestEncounter* Reentrant = Fixture.World->SpawnActor<AGGYGOEncounterLifecycleTestEncounter>();
	if (!TestNotNull(TEXT("initialization reentry Encounter"), Reentrant)) { return false; }
	Reentrant->ConfigureForTest(Config.Definition.Get());
	Reentrant->bCleanupDuringAvatarInitialization = true;
	TestFalse(TEXT("callback cleanup prevents stale SpawnBoss success"), Reentrant->SpawnBoss());
	TestTrue(TEXT("real Avatar PostInitializeComponents callback ran"), Reentrant->bInitializationCallbackRan);
	TestTrue(TEXT("Spawn blocked while original SpawnBoss is still active"), Reentrant->bInitializationSpawnRejected);
	TestTrue(TEXT("construction rollback clears references"), ReferencesEmpty(Reentrant));
	TestEqual(TEXT("construction rollback leaves no State"), CountLiveActors<AGGYGOBossState>(Fixture.World), 0);
	TestEqual(TEXT("construction rollback leaves no Controller"), CountLiveActors<AGGYGOBossAIController>(Fixture.World), 0);
	TestEqual(TEXT("construction rollback leaves no Avatar"), CountLiveActors<AGGYGOBossCharacter>(Fixture.World), 0);

	Reentrant->bCleanupDuringAvatarInitialization = false;
	if (!TestTrue(TEXT("can retry after construction rollback"), Reentrant->SpawnBoss())) { return false; }
	AGGYGOBossState* EarlyState = Reentrant->GetBossState();
	AGGYGOBossAIController* EarlyController = Reentrant->GetBossController();
	AGGYGOBossCharacter* EarlyAvatar = Reentrant->GetBossAvatar();
	TestTrue(TEXT("destroy Avatar before Encounter cleanup"), EarlyAvatar->Destroy());
	TestFalse(TEXT("Avatar became invalid before cleanup"), IsValid(EarlyAvatar));
	Reentrant->CleanupForTest();
	Reentrant->CleanupForTest();
	TestTrue(TEXT("early Avatar loss still releases remaining objects"), EarlyState->IsActorBeingDestroyed()
		&& EarlyController->IsActorBeingDestroyed() && ReferencesEmpty(Reentrant));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOEncounterCreationOwnershipTest,
	"GGYGO.BossAI.Encounter.ExplicitCreationOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOEncounterCreationOwnershipTest::RunTest(const FString& Parameters)
{
	FEncounterTestWorld Fixture;
	if (!TestTrue(TEXT("independent WorldContext fixture"), Fixture.Initialize())) { return false; }
	FEncounterTestConfig Config;
	AGGYGOEncounterLifecycleTestEncounter* Encounter = Fixture.World->SpawnActor<AGGYGOEncounterLifecycleTestEncounter>();
	if (!TestNotNull(TEXT("Encounter"), Encounter)) { return false; }
	Encounter->ConfigureForTest(Config.Definition.Get());
	if (!TestTrue(TEXT("real SpawnBoss succeeds"), Encounter->SpawnBoss())) { return false; }
	AGGYGOBossState* State = Encounter->GetBossState();
	AGGYGOBossAIController* Controller = Encounter->GetBossController();
	AGGYGOBossCharacter* CreatedAvatar = Encounter->GetBossAvatar();
	if (!TestNotNull(TEXT("State"), State) || !TestNotNull(TEXT("Controller"), Controller)
		|| !TestNotNull(TEXT("created Avatar"), CreatedAvatar)) { return false; }
	FActorSpawnParameters ExternalSpawn;
	ExternalSpawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AGGYGOEncounterLifecycleTestPawn* External = Fixture.World->SpawnActor<AGGYGOEncounterLifecycleTestPawn>(ExternalSpawn);
	AActor* DifferentOwner = Fixture.World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("external Avatar"), External) || !TestNotNull(TEXT("different Owner"), DifferentOwner)) { return false; }
	Controller->UnPossess();
	State->AttachAvatar(External);
	Controller->Possess(External);
	TestTrue(TEXT("external Avatar is current through production AttachAvatar"), State->GetAvatarPawn() == External
		&& State->GetGGYGOAbilitySystemComponent()->GetAvatarActor() == External && Controller->GetPawn() == External);
	CreatedAvatar->SetOwner(DifferentOwner);
	External->SetOwner(Encounter);
	TestEqual(TEXT("created Avatar Owner changed"), CreatedAvatar->GetOwner(), DifferentOwner);
	TestEqual(TEXT("external Avatar Owner points at Encounter"), External->GetOwner(), static_cast<AActor*>(Encounter));
	TStrongObjectPtr<UGGYGOEncounterLifecycleObserver> Observer(NewObject<UGGYGOEncounterLifecycleObserver>());
	ObserveCleanup(Fixture, Observer.Get(), Encounter, External);
	Encounter->EndPlayForTest();
	VerifyCleanupOrder(*this, Observer.Get());
	TestTrue(TEXT("initial Avatar reclaimed despite different Owner"), CreatedAvatar->IsActorBeingDestroyed());
	TestTrue(TEXT("external Avatar remains valid despite Encounter Owner"), IsValid(External) && !External->IsActorBeingDestroyed());
	TestNull(TEXT("external Avatar unpossessed"), External->GetController());
	TestNull(TEXT("external Avatar PawnExtension detached"), External->GetPawnExtensionComponent()->GetGGYGOAbilitySystemComponent());
	TestNull(TEXT("host ASC detached"), Observer->ASC->GetAvatarActor());
	TestTrue(TEXT("Encounter State/Controller reclaimed"), State->IsActorBeingDestroyed() && Controller->IsActorBeingDestroyed());
	TestTrue(TEXT("Encounter references empty"), ReferencesEmpty(Encounter));
	Encounter->CleanupForTest();
	TestEqual(TEXT("repeat cleanup preserves event count"), Observer->Events.Num(), 6);
	TestTrue(TEXT("repeat cleanup preserves external Avatar"), IsValid(External) && !External->IsActorBeingDestroyed());
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
