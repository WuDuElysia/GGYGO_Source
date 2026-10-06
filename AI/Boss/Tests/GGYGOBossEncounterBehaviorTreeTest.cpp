#include "AI/Boss/Tests/GGYGOBossEncounterBehaviorTreeTestTypes.h"

#include "AI/Boss/GGYGOBossAIController.h"
#include "AI/Boss/GGYGOBossDefinition.h"
#include "AI/Boss/GGYGOBossState.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossEncounterBehaviorTreeTestTypes)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOEncounterBTTest, Log, All);

namespace
{
	const FName EncounterBTMessage(TEXT("GGYGO.EncounterBT.TerminationProbe"));

	UGGYGOEncounterBTProbe* ResolveProbe(UBehaviorTreeComponent& Owner)
	{
		const AAIController* Controller = Owner.GetAIOwner();
		const AGGYGOEncounterBTTestPawn* Pawn = Controller
			? Cast<AGGYGOEncounterBTTestPawn>(Controller->GetPawn()) : nullptr;
		return Pawn ? Pawn->GetProbeForTest() : nullptr;
	}

	bool IsStopped(const UBehaviorTreeComponent& Brain, const UBTTaskNode* Task)
	{
		return !Brain.IsRunning() && !Brain.TreeHasBeenStarted() && !Brain.IsAbortPending()
			&& !Brain.GetRootTree() && !Brain.GetActiveNode() && Task
			&& Brain.GetTaskStatus(Task) == EBTTaskStatus::Inactive;
	}
}

void AGGYGOEncounterBTTestEncounter::ConfigureForTest(
	const UGGYGOBossDefinition* Definition, UGGYGOEncounterBTProbe* InProbe)
{
	BossDefinition = Definition;
	bSpawnOnBeginPlay = false;
	Probe = InProbe;
}

UGGYGOEncounterBTProbe* AGGYGOEncounterBTTestEncounter::GetProbeForTest() const { return Probe; }
UGGYGOEncounterBTProbe* AGGYGOEncounterBTTestPawn::GetProbeForTest() const { return Probe; }

void AGGYGOEncounterBTTestPawn::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	if (const AGGYGOEncounterBTTestEncounter* Encounter = Cast<AGGYGOEncounterBTTestEncounter>(GetOwner()))
	{
		Probe = Encounter->GetProbeForTest();
	}
}

void AGGYGOEncounterBTTestPawn::UnPossessed()
{
	Super::UnPossessed();
	if (Probe) { Probe->HandleUnpossessed(); }
}

void UGGYGOEncounterBTProbe::HandleWitnessMessage(UBrainComponent* Owner, const FAIMessage& Message)
{
	if (bRecording && Owner == Brain.Get() && Message.MessageName == EncounterBTMessage)
	{
		++WitnessMessageCount;
	}
}

void UGGYGOEncounterBTProbe::CheckMessagesDuringAbort(UBehaviorTreeComponent& Owner)
{
	if (!bRecording) { return; }
	const int32 BeforeTask = TaskMessageCount;
	const int32 BeforeWitness = WitnessMessageCount;
	const UBTTaskNode* Task = RuntimeTask.Get();
	const bool bLiveAbortingInstance = IsValid(Task) && Owner.GetRootTree() == Tree.Get()
		&& Owner.GetTaskStatus(Task) == EBTTaskStatus::Aborting;
	FAIMessage::Send(&Owner, FAIMessage(EncounterBTMessage, this));
	// Dispatch native Brain messages while the BT instance still exists, without ticking
	// a task recursively or requiring access to the protected observer map.
	Owner.UBrainComponent::TickComponent(0.0f, LEVELTICK_All, nullptr);
	bMessagesRemovedDuringLiveAbort = bLiveAbortingInstance
		&& TaskMessageCount == BeforeTask && WitnessMessageCount == BeforeWitness + 1;
}

void UGGYGOEncounterBTProbe::HandleUnpossessed()
{
	if (!bRecording) { return; }
	++UnpossessCount;
	Events.Add(TEXT("Unpossess"));
	if (const UBehaviorTreeComponent* BT = Brain.Get())
	{
		bPendingAbortAtUnpossess = BT->IsAbortPending();
	}
}

void UGGYGOEncounterBTProbe::HandleUninitialized()
{
	if (!bRecording) { return; }
	++DetachCount;
	Events.Add(TEXT("Detach"));
}

void UGGYGOEncounterBTProbe::HandleInitialPawnChanged(APawn* NewPawn)
{
	if (!bRecording || !NewPawn || NewPawn != Avatar.Get()) { return; }
	++InitialPossessCount;
	const AGGYGOBossState* Host = State.Get();
	const AGGYGOBossAIController* AI = Controller.Get();
	const AGGYGOEncounterBTTestPawn* Pawn = Avatar.Get();
	const UGGYGOAbilitySystemComponent* HostASC = ASC.Get();
	const UGGYGOPawnExtensionComponent* Extension = Pawn ? Pawn->GetPawnExtensionComponent() : nullptr;
	const UGGYGOBossDefinition* Definition = Host ? Host->GetBossDefinition() : nullptr;
	bInitialAssemblyBound = IsValid(Host) && IsValid(AI) && IsValid(Pawn) && IsValid(HostASC)
		&& IsValid(Extension) && Definition && Definition->BehaviorTree == Tree.Get()
		&& AI->GetBossState() == Host && AI->GetPawn() == Pawn && Pawn->GetController() == AI
		&& Host->GetAvatarPawn() == Pawn && HostASC->GetOwnerActor() == Host
		&& HostASC->GetAvatarActor() == Pawn && Extension->GetGGYGOAbilitySystemComponent() == HostASC;
	// Native Possess broadcasts after OnPossess/RunBehaviorTree returns, before the
	// production initial wrapper returns to SpawnBoss and its failure cleanup begins.
	UBehaviorTreeComponent* BT = AI ? Cast<UBehaviorTreeComponent>(AI->GetBrainComponent()) : nullptr;
	Brain = BT;
	bInitialBrainWithoutStartedTree = IsValid(BT) && BT->GetOwner() == AI && BT->GetAIOwner() == AI
		&& BT->IsRegistered() && BT->HasBeenInitialized() && !BT->GetRootTree()
		&& !BT->TreeHasBeenStarted() && !BT->GetActiveNode();
}

void UGGYGOEncounterBTProbe::HandleDestroyed(AActor* Actor)
{
	if (!bRecording) { return; }
	if (Actor == Avatar.Get())
	{
		++AvatarDestroyedCount;
		Events.Add(TEXT("DestroyAvatar"));
		const AGGYGOBossState* Host = State.Get();
		const AGGYGOBossAIController* AI = Controller.Get();
		const UGGYGOAbilitySystemComponent* HostASC = ASC.Get();
		const AGGYGOEncounterBTTestPawn* Pawn = Avatar.Get();
		const UGGYGOPawnExtensionComponent* Extension = Pawn ? Pawn->GetPawnExtensionComponent() : nullptr;
		bDetachedBeforeAvatarDestroy = IsValid(Host) && IsValid(AI) && IsValid(HostASC) && Pawn && IsValid(Extension)
			&& !Host->IsActorBeingDestroyed() && !AI->IsActorBeingDestroyed()
			&& !Host->GetAvatarPawn() && !AI->GetPawn() && !HostASC->GetAvatarActor()
			&& !Pawn->GetController() && !Extension->GetGGYGOAbilitySystemComponent();
	}
	else if (Actor == Controller.Get())
	{
		++ControllerDestroyedCount;
		Events.Add(TEXT("DestroyController"));
		// Actor::Destroyed routes component Uninitialize before this broadcast, and
		// MarkComponentsAsGarbage occurs afterwards. Capture now, never tick after Destroy.
		const UBehaviorTreeComponent* BT = Brain.Get();
		const UBTTaskNode* Task = RuntimeTask.Get();
		bFinalStateObserved = IsValid(BT) && IsValid(Task);
		bFinalStateStopped = bFinalStateObserved && IsStopped(*BT, Task);
		bInstanceAndMemoryReleasedAtControllerDestroy = InstanceDestroyedCount == 1 && MemoryDestroyedCount == 1;
		bBrainStoppedWithoutInstanceAtControllerDestroy = IsValid(BT)
			&& !BT->IsRunning() && !BT->TreeHasBeenStarted() && !BT->IsAbortPending()
			&& !BT->GetRootTree() && !BT->GetActiveNode();
	}
	else if (Actor == State.Get())
	{
		++StateDestroyedCount;
		Events.Add(TEXT("DestroyState"));
	}
}

UGGYGOEncounterBTLatentTask::UGGYGOEncounterBTLatentTask(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NodeName = TEXT("Encounter termination latent probe");
	bCreateNodeInstance = true;
	bNotifyTick = true;
	bNotifyTaskFinished = true;
}

void UGGYGOEncounterBTLatentTask::OnInstanceCreated(UBehaviorTreeComponent& Owner)
{
	Super::OnInstanceCreated(Owner);
	Probe = ResolveProbe(Owner);
	if (UGGYGOEncounterBTProbe* Observation = Probe.Get(); Observation && Observation->bRecording)
	{
		++Observation->InstanceCreatedCount;
		Observation->RuntimeTask = this;
	}
}

EBTNodeResult::Type UGGYGOEncounterBTLatentTask::ExecuteTask(UBehaviorTreeComponent& Owner, uint8* NodeMemory)
{
	Probe = ResolveProbe(Owner);
	UGGYGOEncounterBTProbe* Observation = Probe.Get();
	if (!Observation)
	{
		UE_LOG(LogGGYGOEncounterBTTest, Error,
			TEXT("BossAI EncounterBT test task [%s], tree [%s]: possessed test Pawn has no configured observation probe."),
			*GetNameSafe(this), *GetNameSafe(Owner.GetRootTree()));
		return EBTNodeResult::Failed;
	}
	bAwaitingAbort = false;
	AbortTicksRemaining = 0;
	Observation->RuntimeTask = this;
	if (Observation->bRecording)
	{
		++Observation->ExecuteCount;
		Observation->Events.Add(TEXT("Execute"));
	}
	WaitForMessage(Owner, EncounterBTMessage);
	return EBTNodeResult::InProgress;
}

EBTNodeResult::Type UGGYGOEncounterBTLatentTask::AbortTask(UBehaviorTreeComponent& Owner, uint8* NodeMemory)
{
	if (UGGYGOEncounterBTProbe* Observation = Probe.Get(); Observation && Observation->bRecording)
	{
		++Observation->AbortCount;
		Observation->Events.Add(TEXT("Abort"));
		const AGGYGOEncounterBTTestEncounter* Encounter = Observation->Encounter.Get();
		const AGGYGOBossState* Host = Observation->State.Get();
		const AGGYGOBossAIController* AI = Observation->Controller.Get();
		const AGGYGOEncounterBTTestPawn* Pawn = Observation->Avatar.Get();
		const UGGYGOAbilitySystemComponent* HostASC = Observation->ASC.Get();
		const UGGYGOPawnExtensionComponent* Extension = Pawn ? Pawn->GetPawnExtensionComponent() : nullptr;
		Observation->bReferencesClearedAtAbort = Encounter && !Encounter->GetBossState()
			&& !Encounter->GetBossController() && !Encounter->GetBossAvatar();
		Observation->bAssemblyBoundAtAbort = Host && AI && Pawn && HostASC && IsValid(Extension)
			&& AI->GetPawn() == Pawn && Host->GetAvatarPawn() == Pawn && HostASC->GetAvatarActor() == Pawn
			&& Extension->GetGGYGOAbilitySystemComponent() == HostASC;
		Observation->bCreatedActorsAliveAtAbort = IsValid(Host) && IsValid(AI) && IsValid(Pawn)
			&& !Host->IsActorBeingDestroyed() && !AI->IsActorBeingDestroyed() && !Pawn->IsActorBeingDestroyed();
		Observation->CheckMessagesDuringAbort(Owner);
	}
	bAwaitingAbort = bLatentAbort;
	AbortTicksRemaining = bLatentAbort ? 2 : 0;
	return bLatentAbort ? EBTNodeResult::InProgress : EBTNodeResult::Aborted;
}

void UGGYGOEncounterBTLatentTask::TickTask(UBehaviorTreeComponent& Owner, uint8* NodeMemory, float DeltaSeconds)
{
	UGGYGOEncounterBTProbe* Observation = Probe.Get();
	if (Observation && Observation->bRecording) { ++Observation->TickCount; }
	if (!bAwaitingAbort) { return; }
	if (Observation && Observation->bRecording) { ++Observation->AbortTickCount; }
	if (--AbortTicksRemaining == 0)
	{
		bAwaitingAbort = false;
		if (Observation && Observation->bRecording)
		{
			++Observation->FinishLatentAbortCount;
			Observation->Events.Add(TEXT("FinishLatentAbort"));
		}
		FinishLatentAbort(Owner);
	}
}

void UGGYGOEncounterBTLatentTask::OnMessage(UBehaviorTreeComponent& Owner, uint8* NodeMemory,
	FName Message, int32 RequestID, bool bSuccess)
{
	if (UGGYGOEncounterBTProbe* Observation = Probe.Get(); Observation && Observation->bRecording
		&& Message == EncounterBTMessage)
	{
		++Observation->TaskMessageCount;
	}
	// Deliberately do not call the base completion handler: this message observes only.
}

void UGGYGOEncounterBTLatentTask::OnTaskFinished(UBehaviorTreeComponent& Owner, uint8* NodeMemory,
	EBTNodeResult::Type Result)
{
	if (UGGYGOEncounterBTProbe* Observation = Probe.Get(); Observation && Observation->bRecording
		&& Result == EBTNodeResult::Aborted)
	{
		++Observation->AbortedNotificationCount;
		Observation->Events.Add(TEXT("TaskAborted"));
	}
	Super::OnTaskFinished(Owner, NodeMemory, Result);
}

void UGGYGOEncounterBTLatentTask::OnInstanceDestroyed(UBehaviorTreeComponent& Owner)
{
	bAwaitingAbort = false;
	AbortTicksRemaining = 0;
	if (UGGYGOEncounterBTProbe* Observation = Probe.Get(); Observation && Observation->bRecording)
	{
		++Observation->InstanceDestroyedCount;
		Observation->Events.Add(TEXT("InstanceDestroyed"));
	}
	Super::OnInstanceDestroyed(Owner);
}

void UGGYGOEncounterBTLatentTask::CleanupMemory(UBehaviorTreeComponent& Owner, uint8* NodeMemory,
	EBTMemoryClear::Type CleanupType) const
{
	if (UGGYGOEncounterBTProbe* Observation = Probe.Get(); Observation && Observation->bRecording
		&& CleanupType == EBTMemoryClear::Destroy)
	{
		++Observation->MemoryDestroyedCount;
		Observation->Events.Add(TEXT("MemoryDestroyed"));
	}
	Super::CleanupMemory(Owner, NodeMemory, CleanupType);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "AISystem.h"
#include "BehaviorTree/BehaviorTreeManager.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	enum class EEncounterBTScenario { SafeLatent, CleanupSync, CleanupLatent };
	constexpr int32 MaxBTTicks = 32;
	constexpr float BTTickDelta = 1.0f / 60.0f;

	struct FEncounterBTWorld
	{
		UWorld* World = nullptr;
		UEngine* Engine = nullptr;
		bool bHasContext = false;
		TStrongObjectPtr<UGGYGOEncounterBTProbe> Probe{NewObject<UGGYGOEncounterBTProbe>()};

		bool Initialize()
		{
			Engine = GEngine;
			if (!Engine) { return false; }
			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
				.CreateNavigation(false).CreateAISystem(true).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
			if (!World) { return false; }
			Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			bHasContext = true;
			World->InitializeActorsForPlay(FURL());
			return Engine->GetWorldContextFromWorld(World) && World->IsGameWorld() && World->GetPhysicsScene()
				&& World->AreActorsInitialized() && !World->HasBegunPlay()
				&& UAISystem::GetCurrentSafe(World) && UBehaviorTreeManager::GetCurrent(World);
		}

		~FEncounterBTWorld()
		{
			// Assertions finish before this guard. Teardown can never supply DUT evidence.
			Probe->bRecording = false;
			Probe->MessageWitness.Reset();
			if (World)
			{
				for (TActorIterator<AGGYGOEncounterBTTestEncounter> It(World); It; ++It)
				{
					It->CleanupForTest();
				}
				World->DestroyWorld(false);
				if (bHasContext) { Engine->DestroyWorldContext(World); }
				if (UPackage* Package = World->GetPackage()) { Package->SetDirtyFlag(false); }
			}
		}
	};

	struct FEncounterBTConfig
	{
		TStrongObjectPtr<UGGYGOPawnData> PawnData{NewObject<UGGYGOPawnData>(GetTransientPackage())};
		TStrongObjectPtr<UGGYGOBossDefinition> Definition{NewObject<UGGYGOBossDefinition>(GetTransientPackage())};
		TStrongObjectPtr<UBehaviorTree> Tree{NewObject<UBehaviorTree>(GetTransientPackage())};
		UGGYGOEncounterBTLatentTask* AssetTask = nullptr;

		explicit FEncounterBTConfig(bool bLatentAbort)
		{
			UBTComposite_Sequence* Root = NewObject<UBTComposite_Sequence>(Tree.Get());
			AssetTask = NewObject<UGGYGOEncounterBTLatentTask>(Tree.Get());
			AssetTask->bLatentAbort = bLatentAbort;
			FBTCompositeChild Child;
			Child.ChildTask = AssetTask;
			Root->Children.Add(Child);
			Tree->RootNode = Root;
			PawnData->PawnClass = AGGYGOEncounterBTTestPawn::StaticClass();
			Definition->InitialFormTag = GGYGOGameplayTags::State_Boss_Form_Default;
			Definition->InitialPhaseTag = GGYGOGameplayTags::State_Boss_Phase_One;
			Definition->BehaviorTree = Tree.Get();
			FGGYGOBossFormDefinition Form;
			Form.FormTag = Definition->InitialFormTag;
			Form.AvatarPawnData = PawnData.Get();
			Definition->Forms.Add(Form);
			FGGYGOBossPhaseDefinition Phase;
			Phase.PhaseTag = Definition->InitialPhaseTag;
			Definition->Phases.Add(Phase);
		}
	};

	bool EventBefore(const UGGYGOEncounterBTProbe& Probe, FName First, FName Second)
	{
		const int32 A = Probe.Events.Find(First);
		const int32 B = Probe.Events.Find(Second);
		return A != INDEX_NONE && B != INDEX_NONE && A < B;
	}

	bool RunInvalidRequiredTreeScenario(FAutomationTestBase& Test)
	{
		FEncounterBTWorld Fixture;
		if (!Test.TestTrue(TEXT("real initialized Game World/Context/Physics/AISystem/Manager"), Fixture.Initialize()))
		{
			return false;
		}
		FEncounterBTConfig Config(false);
		Config.Tree->RootNode = nullptr;
		if (!Test.TestTrue(TEXT("non-null required tree has an invalid missing root, not AssemblyOnly mode"),
			Config.Definition->BehaviorTree == Config.Tree.Get() && !Config.Tree->RootNode)) { return false; }
		UGGYGOEncounterBTProbe* Probe = Fixture.Probe.Get();
		AGGYGOEncounterBTTestEncounter* Encounter = Fixture.World->SpawnActor<AGGYGOEncounterBTTestEncounter>();
		if (!Test.TestNotNull(TEXT("Encounter"), Encounter)) { return false; }
		Encounter->ConfigureForTest(Config.Definition.Get(), Probe);
		Probe->Encounter = Encounter;
		Probe->Tree = Config.Tree.Get();
		FActorSpawnParameters ExternalParams;
		ExternalParams.Owner = Encounter;
		AActor* External = Fixture.World->SpawnActor<AActor>(ExternalParams);
		if (!Test.TestNotNull(TEXT("external ownership witness"), External)) { return false; }
		const TWeakObjectPtr<AActor> ExternalWeak(External);
		int32 CreatedStates = 0;
		int32 CreatedControllers = 0;
		int32 CreatedAvatars = 0;
		FDelegateHandle PawnWatch;
		// Observe actual production-created identities before failure clears unpublished
		// creation records. No test code performs the rejected assembly's cleanup.
		const FDelegateHandle SpawnWatch = Fixture.World->AddOnActorSpawnedHandler(
			FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor)
			{
				if (Actor->GetOwner() != Encounter) { return; }
				if (AGGYGOBossState* State = Cast<AGGYGOBossState>(Actor))
				{
					++CreatedStates;
					Probe->State = State;
					Probe->ASC = State->GetGGYGOAbilitySystemComponent();
					State->OnDestroyed.AddDynamic(Probe, &UGGYGOEncounterBTProbe::HandleDestroyed);
				}
				else if (AGGYGOBossAIController* Controller = Cast<AGGYGOBossAIController>(Actor))
				{
					++CreatedControllers;
					Probe->Controller = Controller;
					Controller->OnDestroyed.AddDynamic(Probe, &UGGYGOEncounterBTProbe::HandleDestroyed);
					PawnWatch = Controller->GetOnNewPawnNotifier().AddUObject(
						Probe, &UGGYGOEncounterBTProbe::HandleInitialPawnChanged);
				}
				else if (AGGYGOEncounterBTTestPawn* Avatar = Cast<AGGYGOEncounterBTTestPawn>(Actor))
				{
					++CreatedAvatars;
					Probe->Avatar = Avatar;
					Avatar->OnDestroyed.AddDynamic(Probe, &UGGYGOEncounterBTProbe::HandleDestroyed);
					if (UGGYGOPawnExtensionComponent* Extension = Avatar->GetPawnExtensionComponent())
					{
						Extension->OnAbilitySystemUninitialized_Register(FSimpleMulticastDelegate::FDelegate::CreateUObject(
							Probe, &UGGYGOEncounterBTProbe::HandleUninitialized));
					}
				}
			}));
		// Keep real startup Errors visible. Behavior assertions and automation log failures
		// are separate evidence; do not turn invalid required configuration into a green run.
		const bool bSpawned = Encounter->SpawnBoss();
		Fixture.World->RemoveOnActorSpawnedHandler(SpawnWatch);
		if (AGGYGOBossAIController* Controller = Probe->Controller.Get())
		{
			Controller->GetOnNewPawnNotifier().Remove(PawnWatch);
		}
		// All DUT observations/assertions precede explicit or RAII fixture cleanup.
		bool bPassed = Test.TestFalse(TEXT("production SpawnBoss rejects invalid required tree"), bSpawned);
		bPassed &= Test.TestEqual(TEXT("production created the original State"), CreatedStates, 1);
		bPassed &= Test.TestEqual(TEXT("production created the original Controller"), CreatedControllers, 1);
		bPassed &= Test.TestEqual(TEXT("production created the original Avatar"), CreatedAvatars, 1);
		bPassed &= Test.TestEqual(TEXT("original initial Possess returned through native Pawn notification"), Probe->InitialPossessCount, 1);
		bPassed &= Test.TestTrue(TEXT("original State/ASC/Pawn/Controller assembly bound before failure cleanup"), Probe->bInitialAssemblyBound);
		bPassed &= Test.TestTrue(TEXT("native RunBehaviorTree created initialized Brain but no root/Started instance"), Probe->bInitialBrainWithoutStartedTree);
		bPassed &= Test.TestTrue(TEXT("failure cleanup stopped the actual native Brain before Controller destruction"),
			Probe->bBrainStoppedWithoutInstanceAtControllerDestroy);
		bPassed &= Test.TestTrue(TEXT("failed original assembly detached before Avatar destruction"), Probe->bDetachedBeforeAvatarDestroy);
		bPassed &= Test.TestTrue(TEXT("failed SpawnBoss publishes no assembly"),
			!Encounter->GetBossState() && !Encounter->GetBossController() && !Encounter->GetBossAvatar());
		bPassed &= Test.TestTrue(TEXT("original created actors and native Brain invalid before fixture cleanup"),
			!Probe->State.IsValid() && !Probe->Controller.IsValid() && !Probe->Avatar.IsValid() && !Probe->Brain.IsValid());
		bPassed &= Test.TestEqual(TEXT("failed assembly UnPossess once"), Probe->UnpossessCount, 1);
		bPassed &= Test.TestEqual(TEXT("failed assembly host uninitialization once"), Probe->DetachCount, 1);
		bPassed &= Test.TestEqual(TEXT("failed created Avatar destroyed once"), Probe->AvatarDestroyedCount, 1);
		bPassed &= Test.TestEqual(TEXT("failed created Controller destroyed once"), Probe->ControllerDestroyedCount, 1);
		bPassed &= Test.TestEqual(TEXT("failed created State destroyed once"), Probe->StateDestroyedCount, 1);
		bPassed &= Test.TestTrue(TEXT("failed required tree never created or executed a task instance"),
			Probe->InstanceCreatedCount == 0 && Probe->ExecuteCount == 0 && Probe->TickCount == 0);
		bPassed &= Test.TestTrue(TEXT("failure cleanup preserves the external Actor with the same Encounter Owner"), ExternalWeak.IsValid());
		Test.AddInfo(FString::Printf(TEXT("RequiredTreeRejectsInvalidRoot behavior assertions: %s; production startup Errors remain unsuppressed."),
			bPassed ? TEXT("PASS") : TEXT("FAIL")));
		return bPassed;
	}

	bool RunEncounterBTScenario(FAutomationTestBase& Test, EEncounterBTScenario Scenario)
	{
		FEncounterBTWorld Fixture;
		if (!Test.TestTrue(TEXT("real Game World/Context/Physics/AISystem/Manager, initialized without BeginPlay"), Fixture.Initialize()))
		{
			return false;
		}
		FEncounterBTConfig Config(Scenario != EEncounterBTScenario::CleanupSync);
		UGGYGOEncounterBTProbe* Probe = Fixture.Probe.Get();
		AGGYGOEncounterBTTestEncounter* Encounter = Fixture.World->SpawnActor<AGGYGOEncounterBTTestEncounter>();
		if (!Test.TestNotNull(TEXT("Encounter"), Encounter)) { return false; }
		Encounter->ConfigureForTest(Config.Definition.Get(), Probe);
		Probe->Encounter = Encounter;
		Probe->Tree = Config.Tree.Get();
		if (!Test.TestTrue(TEXT("production SpawnBoss succeeds"), Encounter->SpawnBoss())) { return false; }
		AGGYGOBossState* State = Encounter->GetBossState();
		AGGYGOBossAIController* Controller = Encounter->GetBossController();
		AGGYGOEncounterBTTestPawn* Avatar = Cast<AGGYGOEncounterBTTestPawn>(Encounter->GetBossAvatar());
		if (!Test.TestNotNull(TEXT("created State"), State) || !Test.TestNotNull(TEXT("created Controller"), Controller)
			|| !Test.TestNotNull(TEXT("created Avatar"), Avatar)) { return false; }
		UBehaviorTreeComponent* Brain = Cast<UBehaviorTreeComponent>(Controller->GetBrainComponent());
		UGGYGOAbilitySystemComponent* ASC = State->GetGGYGOAbilitySystemComponent();
		if (!Test.TestNotNull(TEXT("native BehaviorTreeComponent from RunBehaviorTree"), Brain)
			|| !Test.TestNotNull(TEXT("host ASC"), ASC)) { return false; }
		Probe->State = State;
		Probe->Controller = Controller;
		Probe->Avatar = Avatar;
		Probe->ASC = ASC;
		Probe->Brain = Brain;
		if (!Test.TestTrue(TEXT("registered initialized BT, correct AI owner and actual root tree"),
			Brain->IsRegistered() && Brain->HasBeenInitialized() && Brain->GetAIOwner() == Controller
			&& Brain->GetRootTree() == Config.Tree.Get() && Brain->TreeHasBeenStarted() && Brain->IsRunning())
			|| !Test.TestTrue(TEXT("production Possess/host/extension assembly"), Controller->GetPawn() == Avatar
				&& Avatar->GetController() == Controller && State->GetAvatarPawn() == Avatar && ASC->GetAvatarActor() == Avatar
				&& Avatar->GetProbeForTest() == Probe && Avatar->GetPawnExtensionComponent()->GetGGYGOAbilitySystemComponent() == ASC))
		{
			return false;
		}
		Probe->MessageWitness = FAIMessageObserver::Create(Brain, EncounterBTMessage,
			FOnAIMessage::CreateUObject(Probe, &UGGYGOEncounterBTProbe::HandleWitnessMessage));
		if (!Test.TestTrue(TEXT("independent native message witness registered"), Probe->MessageWitness.IsValid())) { return false; }
		for (int32 Tick = 0; Tick < MaxBTTicks && (Probe->ExecuteCount == 0 || Probe->TickCount == 0); ++Tick)
		{
			Brain->TickComponent(BTTickDelta, LEVELTICK_All, nullptr);
		}
		UGGYGOEncounterBTLatentTask* RuntimeTask = Probe->RuntimeTask.Get();
		if (!Test.TestNotNull(TEXT("real task instance"), RuntimeTask)
			|| !Test.TestTrue(TEXT("manager created distinct active task and executed/ticked it"), RuntimeTask != Config.AssetTask
				&& RuntimeTask->IsInstanced() && Probe->InstanceCreatedCount == 1 && Probe->ExecuteCount == 1
				&& Probe->TickCount > 0 && Brain->GetTaskStatus(RuntimeTask) == EBTTaskStatus::Active)) { return false; }
		FAIMessage::Send(Brain, FAIMessage(EncounterBTMessage, Probe));
		Brain->TickComponent(BTTickDelta, LEVELTICK_All, nullptr);
		if (!Test.TestEqual(TEXT("active task actually received probe"), Probe->TaskMessageCount, 1)
			|| !Test.TestEqual(TEXT("independent witness actually received probe"), Probe->WitnessMessageCount, 1)) { return false; }
		Avatar->GetPawnExtensionComponent()->OnAbilitySystemUninitialized_Register(
			FSimpleMulticastDelegate::FDelegate::CreateUObject(Probe, &UGGYGOEncounterBTProbe::HandleUninitialized));
		Avatar->OnDestroyed.AddDynamic(Probe, &UGGYGOEncounterBTProbe::HandleDestroyed);
		Controller->OnDestroyed.AddDynamic(Probe, &UGGYGOEncounterBTProbe::HandleDestroyed);
		State->OnDestroyed.AddDynamic(Probe, &UGGYGOEncounterBTProbe::HandleDestroyed);
		FActorSpawnParameters ExternalParams;
		ExternalParams.Owner = Encounter;
		AActor* External = Fixture.World->SpawnActor<AActor>(ExternalParams);
		if (!Test.TestNotNull(TEXT("external ownership witness"), External)) { return false; }
		const TWeakObjectPtr<AActor> ExternalWeak(External);
		bool bPassed = true;

		if (Scenario == EEncounterBTScenario::SafeLatent)
		{
			Brain->StopLogic(TEXT("E9-T1 Safe latent control"));
			if (!Test.TestTrue(TEXT("SafeStop retains a real aborting instance"), Brain->IsAbortPending()
				&& Brain->GetTaskStatus(RuntimeTask) == EBTTaskStatus::Aborting && Probe->FinishLatentAbortCount == 0)) { return false; }
			for (int32 Tick = 0; Tick < MaxBTTicks && Brain->TreeHasBeenStarted(); ++Tick)
			{
				Brain->TickComponent(BTTickDelta, LEVELTICK_All, nullptr);
			}
			if (!Test.TestTrue(TEXT("Safe control completed via actual task Tick/FinishLatentAbort"),
				Probe->AbortTickCount == 2 && Probe->FinishLatentAbortCount == 1
				&& Probe->AbortedNotificationCount == 1 && IsStopped(*Brain, RuntimeTask))) { return false; }
			bPassed &= Test.TestFalse(TEXT("Safe control does not clear Encounter assembly"), Probe->bReferencesClearedAtAbort);
			// A stopped valid component can drain a new message without ticking a stopped BT.
			FAIMessage::Send(Brain, FAIMessage(EncounterBTMessage, Probe));
			Brain->UBrainComponent::TickComponent(0.0f, LEVELTICK_All, nullptr);
			bPassed &= Test.TestEqual(TEXT("stopped control witness received message"), Probe->WitnessMessageCount, 3);
			bPassed &= Test.TestEqual(TEXT("stopped control task did not receive message"), Probe->TaskMessageCount, 1);
		}

		const TWeakObjectPtr<AGGYGOBossState> StateWeak(State);
		const TWeakObjectPtr<AGGYGOBossAIController> ControllerWeak(Controller);
		const TWeakObjectPtr<AGGYGOEncounterBTTestPawn> AvatarWeak(Avatar);
		const TWeakObjectPtr<UBehaviorTreeComponent> BrainWeak(Brain);
		Encounter->CleanupForTest();
		// Every assertion below precedes RAII teardown. Never use a raw destroyed actor/component again.
		bPassed &= Test.TestTrue(TEXT("Abort saw bound, live original assembly"), Probe->bAssemblyBoundAtAbort && Probe->bCreatedActorsAliveAtAbort);
		bPassed &= Test.TestTrue(TEXT("Abort removes task message while its real instance is still present"), Probe->bMessagesRemovedDuringLiveAbort);
		bPassed &= Test.TestEqual(TEXT("one Abort request"), Probe->AbortCount, 1);
		bPassed &= Test.TestEqual(TEXT("instance destruction callback"), Probe->InstanceDestroyedCount, 1);
		bPassed &= Test.TestEqual(TEXT("instance memory destroyed"), Probe->MemoryDestroyedCount, 1);
		bPassed &= Test.TestTrue(TEXT("final public state captured inside Controller OnDestroyed"),
			Probe->bFinalStateObserved && Probe->bFinalStateStopped && Probe->bInstanceAndMemoryReleasedAtControllerDestroy);
		bPassed &= Test.TestTrue(TEXT("host/PawnExtension detached before first created Actor destruction"), Probe->bDetachedBeforeAvatarDestroy);
		bPassed &= Test.TestTrue(TEXT("public Encounter references empty"), !Encounter->GetBossState() && !Encounter->GetBossController() && !Encounter->GetBossAvatar());
		bPassed &= Test.TestTrue(TEXT("original created resources and BT component invalid after Destroy"),
			!StateWeak.IsValid() && !ControllerWeak.IsValid() && !AvatarWeak.IsValid() && !BrainWeak.IsValid());
		bPassed &= Test.TestTrue(TEXT("external Actor retained even with Encounter Owner"), ExternalWeak.IsValid());
		bPassed &= Test.TestEqual(TEXT("UnPossess once"), Probe->UnpossessCount, 1);
		bPassed &= Test.TestEqual(TEXT("host uninitialization once"), Probe->DetachCount, 1);
		bPassed &= Test.TestEqual(TEXT("only created Avatar destroyed once"), Probe->AvatarDestroyedCount, 1);
		bPassed &= Test.TestEqual(TEXT("only created Controller destroyed once"), Probe->ControllerDestroyedCount, 1);
		bPassed &= Test.TestEqual(TEXT("only created State destroyed once"), Probe->StateDestroyedCount, 1);
		bPassed &= Test.TestTrue(TEXT("actual lifecycle order"), EventBefore(*Probe, TEXT("Abort"), TEXT("Unpossess"))
			&& EventBefore(*Probe, TEXT("Unpossess"), TEXT("Detach")) && EventBefore(*Probe, TEXT("Detach"), TEXT("DestroyAvatar"))
			&& EventBefore(*Probe, TEXT("DestroyAvatar"), TEXT("DestroyController")) && EventBefore(*Probe, TEXT("DestroyController"), TEXT("DestroyState")));
		if (Scenario != EEncounterBTScenario::SafeLatent)
		{
			bPassed &= Test.TestTrue(TEXT("Encounter cleared public references before real BT Abort"), Probe->bReferencesClearedAtAbort);
			bPassed &= Test.TestEqual(TEXT("DUT did not wait for graceful Tick completion"), Probe->FinishLatentAbortCount, 0);
			bPassed &= Test.TestEqual(TEXT("DUT did not tick a latent abort after StopLogic"), Probe->AbortTickCount, 0);
			bPassed &= Test.TestEqual(TEXT("DUT message witness received live-abort probe"), Probe->WitnessMessageCount, 2);
		}
		if (Scenario == EEncounterBTScenario::CleanupSync)
		{
			bPassed &= Test.TestEqual(TEXT("synchronous Aborted notification"), Probe->AbortedNotificationCount, 1);
			bPassed &= Test.TestFalse(TEXT("synchronous Abort not pending at UnPossess"), Probe->bPendingAbortAtUnpossess);
			bPassed &= Test.TestTrue(TEXT("synchronous BT instances gone before UnPossess"), EventBefore(*Probe, TEXT("MemoryDestroyed"), TEXT("Unpossess")));
		}
		else if (Scenario == EEncounterBTScenario::CleanupLatent)
		{
			bPassed &= Test.TestTrue(TEXT("latent Abort pending when UnPossess begins"), Probe->bPendingAbortAtUnpossess);
			bPassed &= Test.TestTrue(TEXT("native destruction releases latent instance after Detach, before Controller OnDestroyed"),
				EventBefore(*Probe, TEXT("Detach"), TEXT("InstanceDestroyed"))
				&& EventBefore(*Probe, TEXT("MemoryDestroyed"), TEXT("DestroyController")));
			// UE's native Forced-stop warning is UE_VLOG, not UE_LOG. Do not suppress
			// unrelated warnings or register a phantom expected automation log.
		}
		const int32 EventsBefore = Probe->Events.Num();
		const int32 ExecutesBefore = Probe->ExecuteCount;
		const int32 TicksBefore = Probe->TickCount;
		Encounter->CleanupForTest();
		for (int32 Tick = 0; Tick < 3; ++Tick) { Fixture.World->Tick(LEVELTICK_All, BTTickDelta); }
		bPassed &= Test.TestEqual(TEXT("repeat cleanup/post-destruction World ticks have no task/lifecycle callbacks"), Probe->Events.Num(), EventsBefore);
		bPassed &= Test.TestEqual(TEXT("no subsequent decision/task execution"), Probe->ExecuteCount, ExecutesBefore);
		bPassed &= Test.TestEqual(TEXT("no subsequent original task Tick"), Probe->TickCount, TicksBefore);
		bPassed &= Test.TestTrue(TEXT("external witness still retained"), ExternalWeak.IsValid());
		return bPassed;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOEncounterBTSafeLatentTest,
	"GGYGO.BossAI.Encounter.BehaviorTree.SafeLatentAbort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOEncounterBTSafeLatentTest::RunTest(const FString& Parameters)
{
	return RunEncounterBTScenario(*this, EEncounterBTScenario::SafeLatent);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOEncounterBTSyncCleanupTest,
	"GGYGO.BossAI.Encounter.BehaviorTree.CleanupSyncAbort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOEncounterBTSyncCleanupTest::RunTest(const FString& Parameters)
{
	return RunEncounterBTScenario(*this, EEncounterBTScenario::CleanupSync);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOEncounterBTLatentCleanupTest,
	"GGYGO.BossAI.Encounter.BehaviorTree.CleanupLatentAbortForced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOEncounterBTLatentCleanupTest::RunTest(const FString& Parameters)
{
	return RunEncounterBTScenario(*this, EEncounterBTScenario::CleanupLatent);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOEncounterBTInvalidRequiredTreeTest,
	"GGYGO.BossAI.Encounter.BehaviorTree.RequiredTreeRejectsInvalidRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOEncounterBTInvalidRequiredTreeTest::RunTest(const FString& Parameters)
{
	return RunInvalidRequiredTreeScenario(*this);
}
#endif // WITH_DEV_AUTOMATION_TESTS
