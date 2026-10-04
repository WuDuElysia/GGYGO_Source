#include "Input/Tests/GGYGOInputTestTypes.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Combatants/GGYGOCombatantState.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "GameFramework/InputSettings.h"
#include "HAL/PlatformTime.h"
#include "Input/GGYGOInputComponent.h"
#include "Input/GGYGOInputConfig.h"
#include "Input/GGYGOMovementInputOriginResource.h"
#include "Input/GGYGOPlayerInput.h"
#include "InputAction.h"
#include "Misc/AutomationTest.h"
#include "Player/GGYGOLocalPlayer.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Framework/Application/SlateApplication.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#endif
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOInputTestTypes)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGONativeBirthDiagnostics, Log, All);

void UGGYGOInputTestGameInstance::Init()
{
	// InitializeStandalone already establishes this instance's real WorldContext.
	// The source gate needs LocalPlayer subsystems, not GI services/online delegates.
	// Intentionally do not call Super: installing global network handlers is unrelated.
}

void UGGYGOInputTestGameInstance::Shutdown()
{
	const TArray<ULocalPlayer*> Players = GetLocalPlayers();
	for (ULocalPlayer* LocalPlayer : Players)
	{
		RemoveLocalPlayer(LocalPlayer);
	}
	// Init did not start GI subsystems or install delegates. Super::Shutdown would
	// unconditionally clear other instances' global encryption handlers; avoid it.
	WorldContext = nullptr;
}

void AGGYGOInputTestController::InitInputSystem()
{
	Super::InitInputSystem();
}

bool AGGYGOInputTestController::EnableNativeInputInitializationForTest()
{
	if (PlayerInput || bUseNativeInputInitialization)
	{
		return false;
	}
	bUseNativeInputInitialization = true;
	return true;
}

void AGGYGOInputTestController::ConsumeInputForTest(float DeltaTime)
{
	PostProcessInput(DeltaTime, false);
}

void UGGYGOInputTestHeroComponent::ConfigureMappingForTest(const UInputMappingContext* Mapping, int32 Priority)
{
	DefaultInputMappings.Reset();
	DefaultInputMappings.Add(Mapping);
	InputMappingPriority = Priority;
}

bool UGGYGOInputTestHeroComponent::ObserveLocalAbilitySystemForTest(
	UGGYGOPawnExtensionComponent* Extension, FString& OutError)
{
	return PrepareLocalAbilitySystemSubscription(Extension, OutError);
}

void UGGYGOInputTestHeroComponent::StopObservingLocalAbilitySystemForTest()
{
	ReleaseLocalAbilitySystemSubscription();
}

bool UGGYGOInputTestHeroComponent::ObserveNativeMappingRebuildForTest(UEnhancedInputLocalPlayerSubsystem* Subsystem)
{
	if (!IsValid(Subsystem) || !OriginalNativeMappingSubsystem.IsExplicitlyNull())
	{
		return false;
	}
	OriginalNativeMappingSubsystem = Subsystem;
	NativeMappingRebuildCount = 0;
	FirstNativeMappingRebuildTime = 0.0;
	Subsystem->ControlMappingsRebuiltDelegate.AddDynamic(this, &ThisClass::OnNativeMappingRebuiltForTest);
	return true;
}

void UGGYGOInputTestHeroComponent::StopObservingNativeMappingRebuildForTest()
{
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = OriginalNativeMappingSubsystem.Get())
	{
		Subsystem->ControlMappingsRebuiltDelegate.RemoveDynamic(this, &ThisClass::OnNativeMappingRebuiltForTest);
	}
	OriginalNativeMappingSubsystem.Reset();
}

void UGGYGOInputTestHeroComponent::OnNativeMappingRebuiltForTest()
{
	if (!OriginalNativeMappingSubsystem.IsValid()) { return; }
	if (NativeMappingRebuildCount == 0)
	{
		FirstNativeMappingRebuildTime = FPlatformTime::Seconds();
	}
	++NativeMappingRebuildCount;
}

UGGYGOInputTestMappingContext::UGGYGOInputTestMappingContext(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	RegistrationTrackingMode = EMappingContextRegistrationTrackingMode::CountRegistrations;
}

UGGYGOInputTestAbility::UGGYGOInputTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	ActivationPolicy = EGGYGOAbilityActivationPolicy::OnInputTriggered;
	GroupTag = FGameplayTag();
}

void UGGYGOInputTestAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbilityBody(Original, Handle, ActorInfo, ActivationInfo, TriggerEventData);
	if (IsActive())
	{
		++ActivationCount;
	}
}

AGGYGOInputTestPawn::AGGYGOInputTestPawn(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UGGYGOCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	PawnExtension = CreateDefaultSubobject<UGGYGOPawnExtensionComponent>(TEXT("PawnExtension"));
	Hero = CreateDefaultSubobject<UGGYGOInputTestHeroComponent>(TEXT("Hero"));
}

void AGGYGOInputTestPawn::SetAbilitySystemHostForTest(AGGYGOCombatantState* Host)
{
	check(IsValid(Host) && AbilitySystemHost.IsExplicitlyNull());
	AbilitySystemHost = Host;
}

UGGYGOAbilitySystemComponent* AGGYGOInputTestPawn::GetASCForTest() const
{
	const AGGYGOCombatantState* Host = AbilitySystemHost.Get();
	return Host ? Host->GetGGYGOAbilitySystemComponent() : nullptr;
}

UGGYGOInputComponent* AGGYGOInputTestPawn::GetInputComponentForTest() const
{
	return Cast<UGGYGOInputComponent>(InputComponent);
}

UInputComponent* AGGYGOInputTestPawn::CreatePlayerInputComponent()
{
	return NewObject<UGGYGOInputComponent>(this, NAME_None, RF_Transient);
}

void AGGYGOInputTestPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	Hero->InitializePlayerInput(PlayerInputComponent);
}

#if WITH_DEV_AUTOMATION_TESTS
struct FGGYGOInputTestFixture::FState
{
	bool bNativeMovementOrigin = false;
	UEngine* Engine = nullptr;
	UWorld* World = nullptr;
	TStrongObjectPtr<UGGYGOInputTestGameInstance> GameInstance;
	TStrongObjectPtr<UGGYGOInputTestViewport> Viewport;
	TSharedPtr<SWindow> NativeWindow;
	TSharedPtr<SViewport> NativeViewportWidget;
	TSharedPtr<FSceneViewport> NativeSceneViewport;
	TStrongObjectPtr<ULocalPlayer> LocalPlayer;
	TStrongObjectPtr<UGGYGOInputConfig> InputConfig;
	TStrongObjectPtr<UGGYGOPawnData> PawnData;
	TStrongObjectPtr<UGGYGOInputTestMappingContext> Mapping;
	TStrongObjectPtr<UInputAction> MoveAction;
	TStrongObjectPtr<UInputAction> LookAction;
	TStrongObjectPtr<UInputAction> AbilityAction;
	AGGYGOInputTestController* Controller = nullptr;
	AGGYGOInputTestPawn* Pawn = nullptr;
	AGGYGOCombatantState* AbilitySystemHost = nullptr;
};

FGGYGOInputTestFixture::FGGYGOInputTestFixture() : State(MakeUnique<FState>()) {}
FGGYGOInputTestFixture::~FGGYGOInputTestFixture() { Shutdown(); }

bool FGGYGOInputTestFixture::InitializeNativeMovementOrigin(FAutomationTestBase& Test)
{
	if (State->GameInstance.IsValid() || State->bNativeMovementOrigin)
	{
		Test.AddError(TEXT("Native movement fixture must select its construction path before initialization."));
		return false;
	}
	State->bNativeMovementOrigin = true;
	return Initialize(Test);
}

bool FGGYGOInputTestFixture::RequireNativeMovementCold(FAutomationTestBase& Test, const TCHAR* Stage) const
{
	const UGGYGOLocalPlayer* Host = Cast<UGGYGOLocalPlayer>(State->LocalPlayer.Get());
	const UGGYGOPlayerInput* Producer = Cast<UGGYGOPlayerInput>(GetPlayerInput());
	const UGGYGOMovementInputOriginResource* Resource = Host ? Host->GetMovementInputOriginResource() : nullptr;
	if (!IsValid(Producer) || !IsValid(Resource)
		|| Resource->GetQualification(Producer) != EGGYGOMovementInputOriginQualification::Cold)
	{
		Test.AddError(FString::Printf(TEXT("[Input.NativeBirthTest] Stage=%s Player=%s Controller=%s Producer=%s Resource=%s Qualification=%u: original Cold unavailable; stop without release, requalification or legacy fallback."),
			Stage, *GetPathNameSafe(Host), *GetPathNameSafe(State->Controller), *GetPathNameSafe(Producer),
			*GetPathNameSafe(Resource), Resource ? static_cast<uint32>(Resource->GetQualification(Producer)) : 0));
		return false;
	}
	return true;
}

bool FGGYGOInputTestFixture::Initialize(FAutomationTestBase& Test)
{
	if (State->GameInstance.IsValid())
	{
		Test.AddError(TEXT("Input fixture may only be initialized once before Shutdown."));
		return false;
	}
	State->Engine = GEngine;
	if (!Test.TestNotNull(TEXT("GEngine"), State->Engine)) { return false; }

	State->GameInstance.Reset(NewObject<UGGYGOInputTestGameInstance>(State->Engine, NAME_None, RF_Transient));
	State->GameInstance->InitializeStandalone();
	State->World = State->GameInstance->GetWorld();
	if (!Test.TestNotNull(TEXT("private standalone world"), State->World)) { return false; }
	FWorldContext* Context = State->GameInstance->GetWorldContext();
	if (!Test.TestNotNull(TEXT("private world context"), Context)) { return false; }
	if (!Test.TestEqual(TEXT("context owns this GI"), Context->OwningGameInstance.Get(),
		static_cast<UGameInstance*>(State->GameInstance.Get()))) { return false; }

	// Spawn must route PostInitializeComponents so the native World controller list
	// can resolve LocalPlayer input during SetPlayer's subsystem notification.
	// Initialize actors only; this fixture never starts World gameplay.
	State->World->InitializeActorsForPlay(FURL());
	if (!Test.TestTrue(TEXT("private world actors initialized"), State->World->AreActorsInitialized()))
	{
		return false;
	}
	if (!Test.TestFalse(TEXT("private world has not begun play"), State->World->HasBegunPlay()))
	{
		return false;
	}

	// Viewport and LocalPlayer both require an Engine Outer; the private context
	// and strong owner establish their lifetime without replacing the engine viewport.
	State->Viewport.Reset(NewObject<UGGYGOInputTestViewport>(State->Engine, NAME_None, RF_Transient));
	Context->GameViewport = State->Viewport.Get();
	State->Viewport->Init(*Context, State->GameInstance.Get(), false);
	// Ordinary mode has no window; neither fixture mode assigns GEngine->GameViewport or GWorld.
	if (State->bNativeMovementOrigin)
	{
		if (!Test.TestTrue(TEXT("[Input.NativeBirthTest] native window requires an initialized Slate application"),
			FSlateApplication::IsInitialized())) { return false; }
		if (!Test.TestNotNull(TEXT("[Input.NativeBirthTest] native window requires the actual Slate renderer"),
			FSlateApplication::Get().GetRenderer())) { return false; }

		State->NativeViewportWidget = SNew(SViewport)
			.RenderDirectlyToWindow(false)
			.EnableGammaCorrection(false)
			.EnableStereoRendering(false);
		State->NativeWindow = SNew(SWindow)
			.Title(FText::FromString(TEXT("GGYGO Native Input Fixture")))
			.ClientSize(FVector2D(64.0, 64.0))
			.SizingRule(ESizingRule::FixedSize)
			.FocusWhenFirstShown(false)
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			[
				State->NativeViewportWidget.ToSharedRef()
			];
		FSlateApplication::Get().AddWindow(State->NativeWindow.ToSharedRef(), false);
		if (!Test.TestTrue(TEXT("[Input.NativeBirthTest] hidden window has an actual native OS window"),
			State->NativeWindow->GetNativeWindow().IsValid()
			&& State->NativeWindow->GetNativeWindow()->GetOSWindowHandle() != nullptr)
			|| !Test.TestFalse(TEXT("[Input.NativeBirthTest] fixture window remains hidden"),
				State->NativeWindow->IsVisible())) { return false; }

		// CreateViewport installs the real Slate interface and client association.
		State->NativeSceneViewport = State->Viewport->CreateViewport(State->NativeViewportWidget);
		State->NativeSceneViewport->SetViewportSize(64, 64);
		if (!Test.TestTrue(TEXT("[Input.NativeBirthTest] client owns this actual scene viewport/widget/window"),
			State->Viewport->GetGameViewport() == State->NativeSceneViewport.Get()
			&& State->Viewport->GetGameViewportWidget() == State->NativeViewportWidget
			&& State->Viewport->GetWindow() == State->NativeWindow)
			|| !Test.TestEqual(TEXT("[Input.NativeBirthTest] actual native scene viewport has the configured nonzero size"),
				State->NativeSceneViewport->GetSizeXY(), FIntPoint(64, 64))) { return false; }
		State->Engine->RegisterViewport(State->NativeSceneViewport.ToSharedRef());
	}
	if (State->bNativeMovementOrigin)
	{
		UClass* ConfiguredPlayerClass = State->Engine->LocalPlayerClass.Get();
		if (!Test.TestTrue(TEXT("configured LocalPlayer class is the project origin host"),
			IsValid(ConfiguredPlayerClass) && ConfiguredPlayerClass->IsChildOf(UGGYGOLocalPlayer::StaticClass()))) { return false; }
		FString Error;
		State->LocalPlayer.Reset(State->GameInstance->CreateLocalPlayer(FPlatformUserId::CreateFromInternalId(0), Error, false));
		if (!Test.TestNotNull(TEXT("native CreateLocalPlayer returned the configured project player"), State->LocalPlayer.Get()))
		{
			Test.AddError(TEXT("[Input.NativeBirthTest] CreateLocalPlayer failed: ") + Error);
			return false;
		}
		const UGGYGOLocalPlayer* Host = Cast<UGGYGOLocalPlayer>(State->LocalPlayer.Get());
		const UGGYGOMovementInputOriginResource* Resource = Host ? Host->GetMovementInputOriginResource() : nullptr;
		if (!Test.TestTrue(TEXT("real PlayerAdded retained the original resource before any Controller"),
			Host && !Host->PlayerController && IsValid(Resource) && Resource->GetOuter() == Host
			&& Host->GetClass() == ConfiguredPlayerClass)) { return false; }
	}
	else
	{
		UClass* ConfiguredPlayerClass = State->Engine->LocalPlayerClass.Get();
		if (!IsValid(ConfiguredPlayerClass) || !ConfiguredPlayerClass->IsChildOf(UGGYGOLocalPlayer::StaticClass()))
		{
			Test.AddError(FString::Printf(TEXT("[Input.Fixture] LocalPlayerClass=%s Reason=RequiredProjectLocalPlayerClassInvalid."),
				*GetPathNameSafe(ConfiguredPlayerClass)));
			return false;
		}
		FString Error;
		State->LocalPlayer.Reset(State->GameInstance->CreateLocalPlayer(FPlatformUserId::CreateFromInternalId(0), Error, false));
		if (!State->LocalPlayer.IsValid() || State->LocalPlayer->GetClass() != ConfiguredPlayerClass)
		{
			Test.AddError(FString::Printf(TEXT("[Input.Fixture] CreateLocalPlayer Class=%s Player=%s Error=%s Reason=ConfiguredPlayerCreationFailed."),
				*GetPathNameSafe(ConfiguredPlayerClass), *GetPathNameSafe(State->LocalPlayer.Get()), *Error));
			return false;
		}
	}
	if (!Test.TestNotNull(TEXT("real EnhancedInput LocalPlayer subsystem"), GetInputSubsystem())) { return false; }

	State->Controller = State->World->SpawnActor<AGGYGOInputTestController>();
	if (!Test.TestNotNull(TEXT("private controller"), State->Controller)) { return false; }
	if (!Test.TestTrue(TEXT("private controller actor initialized"), State->Controller->IsActorInitialized()))
	{
		return false;
	}
	if (State->bNativeMovementOrigin)
	{
		if (!Test.TestNull(TEXT("native birth starts with the actual empty Controller input slot"), State->Controller->PlayerInput.Get())
			|| !Test.TestNull(TEXT("native birth uses the configured default, with no Controller override"), State->Controller->GetOverridePlayerInputClass().Get())
			|| !Test.TestTrue(TEXT("native construction choice installed before SetPlayer"), State->Controller->EnableNativeInputInitializationForTest())) { return false; }
	}
	else if (!State->Controller->EnableNativeInputInitializationForTest())
	{
		Test.AddError(TEXT("[Input.Fixture] Reason=NativeInputChoiceRejectedBeforeSetPlayer."));
		return false;
	}
	State->Controller->SetPlayer(State->LocalPlayer.Get());
	if (State->bNativeMovementOrigin)
	{
		if (!RequireNativeMovementCold(Test, TEXT("SetPlayer/native Init/PostInit/Complete/Close"))) { return false; }
		if (!Test.TestTrue(TEXT("native input is the exact configured project default class"),
			GetPlayerInput()->GetClass() == UGGYGOPlayerInput::StaticClass()
			&& UInputSettings::GetDefaultPlayerInputClass() == UGGYGOPlayerInput::StaticClass())) { return false; }
	}
	if (!Test.TestNotNull(TEXT("controller-owned EnhancedPlayerInput"), GetPlayerInput())) { return false; }
	if (!Test.TestEqual(TEXT("LocalPlayer world lookup returns private controller"),
		State->LocalPlayer->GetPlayerController(State->World),
		static_cast<APlayerController*>(State->Controller))) { return false; }
	if (!Test.TestEqual(TEXT("subsystem returns controller input"), GetInputSubsystem()->GetPlayerInput(),
		GetPlayerInput())) { return false; }

	State->InputConfig.Reset(NewObject<UGGYGOInputConfig>(State->GameInstance.Get(), NAME_None, RF_Transient));
	State->PawnData.Reset(NewObject<UGGYGOPawnData>(State->GameInstance.Get(), NAME_None, RF_Transient));
	State->Mapping.Reset(NewObject<UGGYGOInputTestMappingContext>(State->GameInstance.Get(), NAME_None, RF_Transient));
	State->MoveAction.Reset(NewObject<UInputAction>(State->InputConfig.Get(), NAME_None, RF_Transient));
	State->LookAction.Reset(NewObject<UInputAction>(State->InputConfig.Get(), NAME_None, RF_Transient));
	State->AbilityAction.Reset(NewObject<UInputAction>(State->InputConfig.Get(), NAME_None, RF_Transient));
	State->MoveAction->ValueType = EInputActionValueType::Axis2D;
	State->LookAction->ValueType = EInputActionValueType::Axis2D;
	State->AbilityAction->ValueType = EInputActionValueType::Boolean;
	if (State->bNativeMovementOrigin)
	{
		// Define the fixture asset before setup; installation remains Hero's normal default Add.
		for (const FKey& Key : { EKeys::W, EKeys::A, EKeys::S, EKeys::D })
		{
			State->Mapping->MapKey(State->MoveAction.Get(), Key);
		}
	}
	else
	{
		// The ordinary fixture also provides valid movement configuration for Hero's native rebuild.
		for (const FKey& Key : { EKeys::W, EKeys::A, EKeys::S, EKeys::D })
		{
			State->Mapping->MapKey(State->MoveAction.Get(), Key);
		}
	}
	FGGYGOInputAction MoveBinding;
	MoveBinding.InputAction = State->MoveAction.Get();
	MoveBinding.InputTag = GGYGOGameplayTags::InputTag_Move;
	State->InputConfig->NativeInputActions.Add(MoveBinding);
	FGGYGOInputAction LookBinding;
	LookBinding.InputAction = State->LookAction.Get();
	LookBinding.InputTag = GGYGOGameplayTags::InputTag_Look_Mouse;
	State->InputConfig->NativeInputActions.Add(LookBinding);
	FGGYGOInputAction AbilityBinding;
	AbilityBinding.InputAction = State->AbilityAction.Get();
	AbilityBinding.InputTag = GGYGOGameplayTags::InputTag_Attack_Light;
	State->InputConfig->AbilityInputActions.Add(AbilityBinding);
	State->PawnData->InputConfig = State->InputConfig.Get();

	State->Pawn = State->World->SpawnActor<AGGYGOInputTestPawn>();
	if (!Test.TestNotNull(TEXT("private Pawn"), State->Pawn)) { return false; }
	FActorSpawnParameters HostSpawnParameters;
	HostSpawnParameters.Owner = State->Controller;
	State->AbilitySystemHost = State->World->SpawnActor<AGGYGOInputTestAbilitySystemHost>(HostSpawnParameters);
	if (!IsValid(State->AbilitySystemHost) || !IsValid(State->AbilitySystemHost->GetGGYGOAbilitySystemComponent()))
	{
		Test.AddError(FString::Printf(TEXT("[Input.Fixture] Host=%s Pawn=%s Reason=RequiredAbilitySystemHostCreationFailed."),
			*GetPathNameSafe(State->AbilitySystemHost), *GetPathNameSafe(State->Pawn)));
		return false;
	}
	State->Pawn->SetAbilitySystemHostForTest(State->AbilitySystemHost);
	if (!State->bNativeMovementOrigin)
	{
		FString SubscriptionError;
		if (!State->Pawn->GetHeroForTest()->ObserveLocalAbilitySystemForTest(
			State->Pawn->GetPawnExtensionForTest(), SubscriptionError))
		{
			Test.AddError(SubscriptionError);
			return false;
		}
	}
	State->Pawn->GetHeroForTest()->ConfigureMappingForTest(State->Mapping.Get(), 0);
	if (State->bNativeMovementOrigin
		&& !Test.TestTrue(TEXT("observe only the original subsystem's actual rebuild notification"),
			State->Pawn->GetHeroForTest()->ObserveNativeMappingRebuildForTest(GetInputSubsystem()))) { return false; }
	State->Pawn->GetPawnExtensionForTest()->SetPawnData(State->PawnData.Get());
	State->Controller->Possess(State->Pawn);
	if (State->bNativeMovementOrigin && !RequireNativeMovementCold(Test, TEXT("Possess/first SetupPlayerInputComponent"))) { return false; }
	// A synthetic world never starts gameplay. Use the public restart entry to ensure
	// local Pawn input exists even when possession has not dispatched a client restart.
	if (!State->bNativeMovementOrigin || !State->Pawn->GetInputComponentForTest())
	{
		State->Pawn->PawnClientRestart();
	}
	if (State->bNativeMovementOrigin && !RequireNativeMovementCold(Test, TEXT("PawnClientRestart/first setup if required"))) { return false; }
	if (!Test.TestNotNull(TEXT("Pawn-owned project input component"), State->Pawn->GetInputComponentForTest()))
	{
		return false;
	}
	// The real Host alone initializes, installs and publishes this Avatar's ASC resource.
	State->AbilitySystemHost->AttachAvatar(State->Pawn);
	if (State->bNativeMovementOrigin)
	{
		// The source gate needs first normal setup, not the legacy fixture's second Hero init.
		return RequireNativeMovementCold(Test, TEXT("fixture initialized/pending native rebuild"))
			&& Test.TestTrue(TEXT("native source route ready, waiting for the actual mapping rebuild"),
				State->Pawn->IsLocallyControlled() && GetInputSubsystem()->GetPlayerInput() == GetPlayerInput()
				&& GetInputSubsystem()->HasMappingContext(State->Mapping.Get()));
	}
	// Actual Hero initialization binds its original ASC after the real coordinator
	// initialized it. No fabricated notification or private session-validity override.
	State->Pawn->GetHeroForTest()->InitializePlayerInput(State->Pawn->GetInputComponentForTest());
	return Test.TestTrue(TEXT("real local Hero source gate established"), State->Pawn->IsLocallyControlled()
		&& State->Pawn->GetASCForTest()->GetAvatarActor() == State->Pawn
		&& State->Pawn->GetASCForTest()->OnAbilityInputRetryable.IsBoundToObject(State->Pawn->GetHeroForTest())
		&& State->Pawn->GetASCForTest()->OnAbilityGroupFreed.IsBoundToObject(State->Pawn->GetHeroForTest())
		&& GetInputSubsystem()->GetPlayerInput() == GetPlayerInput()
		&& GetInputSubsystem()->HasMappingContext(State->Mapping.Get()));
}

void FGGYGOInputTestFixture::Shutdown()
{
	if (!State) { return; }
	// DestroyWorld removes its root; keep it alive through context/viewport callbacks.
	TStrongObjectPtr<UWorld> WorldLifetime(State->World);
	if (IsValid(State->Pawn))
	{
		State->Pawn->GetHeroForTest()->StopObservingNativeMappingRebuildForTest();
		State->Pawn->GetHeroForTest()->ReleasePlayerInput();
		State->Pawn->GetPawnExtensionForTest()->UninitializeAbilitySystem(State->Pawn->GetASCForTest());
		State->Pawn->GetHeroForTest()->StopObservingLocalAbilitySystemForTest();
	}
	if (IsValid(State->AbilitySystemHost)) { State->AbilitySystemHost->DetachAvatar(State->Pawn); }
	if (IsValid(State->AbilitySystemHost)) { State->AbilitySystemHost->Destroy(); }
	State->AbilitySystemHost = nullptr;
	if (IsValid(State->Pawn)) { State->Pawn->Destroy(); }
	State->Pawn = nullptr;
	if (State->GameInstance.IsValid())
	{
		// Removes this instance's LocalPlayers (PlayerRemoved deinitializes their subsystems)
		// and clears only this GI's context pointer; never clears global network delegates.
		State->GameInstance->Shutdown();
	}
	if (IsValid(State->Controller)) { State->Controller->Destroy(); }
	State->Controller = nullptr;
	State->LocalPlayer.Reset();
	// Remove native rendering/association/window resources while the client and World are still alive.
	// Unregister is also safe when initialization failed before registration.
	if (State->NativeSceneViewport.IsValid())
	{
		State->Engine->UnregisterViewport(State->NativeSceneViewport.ToSharedRef());
		State->NativeSceneViewport.Reset(); // Destructor runs Destroy/RemoveAssociation and releases rendering resources.
	}
	if (State->NativeWindow.IsValid())
	{
		FSlateApplication::Get().DestroyWindowImmediately(State->NativeWindow.ToSharedRef());
		State->NativeWindow.Reset();
	}
	State->NativeViewportWidget.Reset();
	if (State->World)
	{
		State->World->SetGameInstance(nullptr);
		State->World->DestroyWorld(false);
		// Lookup still requires the current World. DestroyWorldContext itself clears
		// its external references, including the viewport's World installed by Init.
		State->Engine->DestroyWorldContext(State->World);
		if (State->Viewport.IsValid())
		{
			State->Viewport->DetachViewportClient();
			// Public UObject lifecycle entry immediately returns Init's owned audio handle.
			State->Viewport->ConditionalBeginDestroy();
		}
		State->World->GetPackage()->SetDirtyFlag(false);
		State->World = nullptr;
	}
	State->Viewport.Reset();
	State->InputConfig.Reset();
	State->PawnData.Reset();
	State->Mapping.Reset();
	State->MoveAction.Reset();
	State->LookAction.Reset();
	State->AbilityAction.Reset();
	State->GameInstance.Reset();
	State->bNativeMovementOrigin = false;
}

UWorld* FGGYGOInputTestFixture::GetWorld() const { return State->World; }
UGGYGOInputTestGameInstance* FGGYGOInputTestFixture::GetGameInstance() const { return State->GameInstance.Get(); }
UGameViewportClient* FGGYGOInputTestFixture::GetViewport() const { return State->Viewport.Get(); }
ULocalPlayer* FGGYGOInputTestFixture::GetLocalPlayer() const { return State->LocalPlayer.Get(); }
UEnhancedInputLocalPlayerSubsystem* FGGYGOInputTestFixture::GetInputSubsystem() const
{
	return State->LocalPlayer.IsValid() ? State->LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
}
UEnhancedPlayerInput* FGGYGOInputTestFixture::GetPlayerInput() const
{
	return IsValid(State->Controller) ? Cast<UEnhancedPlayerInput>(State->Controller->PlayerInput) : nullptr;
}
AGGYGOInputTestController* FGGYGOInputTestFixture::GetController() const { return State->Controller; }
AGGYGOInputTestPawn* FGGYGOInputTestFixture::GetPawn() const { return State->Pawn; }
const UInputAction* FGGYGOInputTestFixture::GetAbilityAction() const { return State->AbilityAction.Get(); }
const UInputAction* FGGYGOInputTestFixture::GetMoveAction() const { return State->MoveAction.Get(); }
const UInputMappingContext* FGGYGOInputTestFixture::GetMappingContext() const { return State->Mapping.Get(); }

int32 FGGYGOInputTestFixture::ExecuteAbilityBinding(ETriggerEvent Event)
{
	UGGYGOInputComponent* Input = IsValid(State->Pawn) ? State->Pawn->GetInputComponentForTest() : nullptr;
	if (!Input || !State->AbilityAction.IsValid()) { return 0; }
	TArray<TUniquePtr<FEnhancedInputActionEventBinding>> Snapshot;
	for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : Input->GetActionEventBindings())
	{
		if (Binding->GetAction() == State->AbilityAction.Get() && Binding->GetTriggerEvent() == Event)
		{
			Snapshot.Add(Binding->Clone());
		}
	}
	const FInputActionInstance Instance(State->AbilityAction.Get());
	for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : Snapshot) { Binding->Execute(Instance); }
	return Snapshot.Num();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOInputFixtureLocalSessionReadyTest,
	"GGYGO.Input.Fixture.LocalSessionReady",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOInputFixtureLocalSessionReadyTest::RunTest(const FString& Parameters)
{
	UWorld* OriginalGlobalWorld = GWorld;
	UGameViewportClient* OriginalGlobalViewport = GEngine ? GEngine->GameViewport.Get() : nullptr;
	FGGYGOInputTestFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	AGGYGOInputTestPawn* Pawn = Fixture.GetPawn();
	UGGYGOInputTestHeroComponent* Hero = Pawn->GetHeroForTest();
	UGGYGOAbilitySystemComponent* ASC = Pawn->GetASCForTest();
	UGGYGOInputComponent* Input = Pawn->GetInputComponentForTest();
	TestTrue(TEXT("Pawn is genuinely locally controlled"), Pawn->IsLocallyControlled());
	TestEqual(TEXT("controller owns this LocalPlayer"), Fixture.GetController()->GetLocalPlayer(), Fixture.GetLocalPlayer());
	TestEqual(TEXT("LocalPlayer uses this viewport world"), Fixture.GetLocalPlayer()->GetWorld(), Fixture.GetWorld());
	TestEqual(TEXT("LocalPlayer uses this GI"), Fixture.GetLocalPlayer()->GetGameInstance(),
		static_cast<UGameInstance*>(Fixture.GetGameInstance()));
	TestEqual(TEXT("viewport uses this world"), Fixture.GetViewport()->GetWorld(), Fixture.GetWorld());
	TestEqual(TEXT("world owns this GI"), Fixture.GetWorld()->GetGameInstance(),
		static_cast<UGameInstance*>(Fixture.GetGameInstance()));
	TestEqual(TEXT("GI resolves its own world"), Fixture.GetGameInstance()->GetWorld(), Fixture.GetWorld());
	TestEqual(TEXT("subsystem belongs to this LocalPlayer"), Fixture.GetInputSubsystem()->GetLocalPlayer(), Fixture.GetLocalPlayer());
	TestEqual(TEXT("PlayerInput belongs to this controller"), Fixture.GetPlayerInput()->GetOuter(),
		static_cast<UObject*>(Fixture.GetController()));
	TestEqual(TEXT("PawnExtension exposes the real ASC"), Pawn->GetPawnExtensionForTest()->GetGGYGOAbilitySystemComponent(), ASC);
	TestEqual(TEXT("ASC Avatar matches this Pawn"), ASC->GetAvatarActor(), static_cast<AActor*>(Pawn));
	TestTrue(TEXT("Hero owns a real retry subscription"), ASC->OnAbilityInputRetryable.IsBoundToObject(Hero));
	TestTrue(TEXT("Hero owns a real group subscription"), ASC->OnAbilityGroupFreed.IsBoundToObject(Hero));
	TestTrue(TEXT("fixture IMC is actually registered"), Fixture.GetInputSubsystem()->HasMappingContext(Fixture.GetMappingContext()));

	FGameplayAbilitySpec ProbeSpec(UGGYGOInputTestAbility::StaticClass(), 1);
	ProbeSpec.GetDynamicSpecSourceTags().AddTag(GGYGOGameplayTags::InputTag_Attack_Light);
	const FGameplayAbilitySpecHandle ProbeHandle = ASC->GiveAbility(ProbeSpec);
	if (!TestTrue(TEXT("fixture probe granted"), ProbeHandle.IsValid())) { return false; }
	if (!TestEqual(TEXT("one real Triggered binding executes"), Fixture.ExecuteAbilityBinding(ETriggerEvent::Triggered), 1))
	{
		return false;
	}
	Fixture.GetController()->ConsumeInputForTest();
	FGameplayAbilitySpec* ActivatedSpec = ASC->FindAbilitySpecFromHandle(ProbeHandle);
	UGGYGOInputTestAbility* Probe = ActivatedSpec ? Cast<UGGYGOInputTestAbility>(ActivatedSpec->GetPrimaryInstance()) : nullptr;
	if (!TestNotNull(TEXT("real GAS probe instance"), Probe)) { return false; }
	TestEqual(TEXT("binding passed Hero validity and PC/ASC consumption"), Probe->GetActivationCountForTest(), 1);
	TestEqual(TEXT("one real Completed binding executes"), Fixture.ExecuteAbilityBinding(ETriggerEvent::Completed), 1);
	Fixture.GetController()->ConsumeInputForTest();

	Hero->ReleasePlayerInput();
	TestEqual(TEXT("fixture input bindings returned"), Input->GetActionEventBindings().Num(), 0);
	TestFalse(TEXT("fixture IMC returned"), Fixture.GetInputSubsystem()->HasMappingContext(Fixture.GetMappingContext()));
	TestFalse(TEXT("fixture retry subscription returned"), ASC->OnAbilityInputRetryable.IsBoundToObject(Hero));
	TestFalse(TEXT("fixture group subscription returned"), ASC->OnAbilityGroupFreed.IsBoundToObject(Hero));
	Pawn->GetPawnExtensionForTest()->UninitializeAbilitySystem(ASC);
	TestNull(TEXT("coordinator released its ASC"), Pawn->GetPawnExtensionForTest()->GetGGYGOAbilitySystemComponent());
	TestNull(TEXT("ASC released its Avatar"), ASC->GetAvatarActor());

	// Hold only the objects whose public post-cleanup state is inspected; no GC sweep.
	TStrongObjectPtr<ULocalPlayer> ReleasedPlayer(Fixture.GetLocalPlayer());
	TStrongObjectPtr<UGGYGOInputTestGameInstance> ReleasedGI(Fixture.GetGameInstance());
	UWorld* ReleasedWorld = Fixture.GetWorld();
	Fixture.Shutdown();
	TestEqual(TEXT("own LocalPlayer removed"), ReleasedGI->GetNumLocalPlayers(), 0);
	TestNull(TEXT("LocalPlayer subsystems deinitialized"), ReleasedPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>());
	TestNull(TEXT("own GI detached from context"), ReleasedGI->GetWorldContext());
	TestNull(TEXT("own engine world context removed"), GEngine->GetWorldContextFromWorld(ReleasedWorld));
	TestEqual(TEXT("global editor world preserved"), static_cast<UWorld*>(GWorld), OriginalGlobalWorld);
	TestEqual(TEXT("global engine viewport preserved"), GEngine->GameViewport.Get(), OriginalGlobalViewport);
	return true;
}

namespace
{
	/** Weak observations do not retain or refresh the original native objects. */
	struct FNativeWeakObjectDiagnostic
	{
		TWeakObjectPtr<UObject> Identity;
		FString InitialPath;

		void Capture(UObject* Object)
		{
			Identity = Object;
			InitialPath = GetPathNameSafe(Object);
		}

		FString Describe() const
		{
			// Index/serial resolution permits flags only; live routes use normal weak Get.
			const UObject* AllocatedObject = Identity.GetEvenIfUnreachable();
			return FString::Printf(TEXT("%s{Null=%d Valid=%d Stale=%d FlagsKnown=%d Flags=0x%08x InternalFlags=0x%08x}"),
				*InitialPath, Identity.IsExplicitlyNull(), Identity.IsValid(), Identity.IsStale(), AllocatedObject != nullptr,
				AllocatedObject ? static_cast<uint32>(AllocatedObject->GetFlags()) : 0u,
				AllocatedObject ? static_cast<uint32>(AllocatedObject->GetInternalFlags()) : 0u);
		}
	};

	/** Owns only this probe's resources and immutable observed fact copies. */
	struct FNativeMovementOriginProbe
	{
		explicit FNativeMovementOriginProbe(FAutomationTestBase& InTest)
			: Test(InTest), OriginalGlobalWorld(GWorld), OriginalGlobalViewport(GEngine ? GEngine->GameViewport.Get() : nullptr)
		{}
		~FNativeMovementOriginProbe() { Cleanup(); }

		bool Initialize()
		{
			const bool bReady = Fixture.InitializeNativeMovementOrigin(Test);
			OriginalPlayer.Reset(Fixture.GetLocalPlayer());
			OriginalGI.Reset(Fixture.GetGameInstance());
			OriginalWorld = Fixture.GetWorld();
			OriginalProducer.Reset(Cast<UGGYGOPlayerInput>(Fixture.GetPlayerInput()));
			const UGGYGOLocalPlayer* Host = Cast<UGGYGOLocalPlayer>(OriginalPlayer.Get());
			OriginalResource.Reset(Host ? Host->GetMovementInputOriginResource() : nullptr);
			if (!bReady)
			{
				Cleanup();
				return false;
			}
			PlayerDiagnostic.Capture(OriginalPlayer.Get());
			GIDiagnostic.Capture(OriginalGI.Get());
			ProducerDiagnostic.Capture(OriginalProducer.Get());
			ResourceDiagnostic.Capture(OriginalResource.Get());
			ControllerDiagnostic.Capture(Fixture.GetController());
			PawnDiagnostic.Capture(Fixture.GetPawn());
			SubsystemDiagnostic.Capture(Fixture.GetInputSubsystem());
			WorldDiagnostic.Capture(OriginalWorld);
			const bool bCold = CheckCold(TEXT("after real native fixture initialization"));
			LogNativeSnapshot(TEXT("native fixture initialization complete"));
			return bCold;
		}

		bool CheckCold(const TCHAR* Stage)
		{
			if (!IsValid(OriginalProducer.Get()) || !IsValid(OriginalResource.Get())
				|| OriginalResource->GetQualification(OriginalProducer.Get()) != EGGYGOMovementInputOriginQualification::Cold)
			{
				const FString Error = FString::Printf(TEXT("[Input.NativeBirthTest] Stage=%s Producer=%s Resource=%s: Cold retired/unavailable; stop without release or requalification."),
					Stage, *GetPathNameSafe(OriginalProducer.Get()), *GetPathNameSafe(OriginalResource.Get()));
				LogNativeSnapshot(Stage);
				Test.AddError(Error);
				return false;
			}
			return true;
		}

		void LogNativeSnapshot(const TCHAR* Stage) const
		{
			if (!UE_LOG_ACTIVE(LogGGYGONativeBirthDiagnostics, Verbose)) { return; }
			const uint64 Frame = GFrameCounter;
			const ULocalPlayer* Player = Cast<ULocalPlayer>(PlayerDiagnostic.Identity.Get());
			const UGGYGOInputTestGameInstance* GI = Cast<UGGYGOInputTestGameInstance>(GIDiagnostic.Identity.Get());
			const AGGYGOInputTestController* Controller = Cast<AGGYGOInputTestController>(ControllerDiagnostic.Identity.Get());
			const AGGYGOInputTestPawn* Pawn = Cast<AGGYGOInputTestPawn>(PawnDiagnostic.Identity.Get());
			const UEnhancedInputLocalPlayerSubsystem* Subsystem = Cast<UEnhancedInputLocalPlayerSubsystem>(SubsystemDiagnostic.Identity.Get());
			const UWorld* World = Cast<UWorld>(WorldDiagnostic.Identity.Get());
			const UGGYGOMovementInputOriginResource* Resource = Cast<UGGYGOMovementInputOriginResource>(ResourceDiagnostic.Identity.Get());

			FString Qualification(TEXT("NotQueried.ResourceInvalid"));
			if (Resource)
			{
				switch (Resource->GetQualification(OriginalProducer.Get()))
				{
				case EGGYGOMovementInputOriginQualification::Unavailable: Qualification = TEXT("Unavailable"); break;
				case EGGYGOMovementInputOriginQualification::Cold: Qualification = TEXT("Cold"); break;
				case EGGYGOMovementInputOriginQualification::Rearm: Qualification = TEXT("Rearm"); break;
				}
			}
			const auto DescribeRoute = [](UObject* Current, const FNativeWeakObjectDiagnostic& Expected)
			{
				FNativeWeakObjectDiagnostic Observed;
				Observed.Capture(Current);
				return FString::Printf(TEXT("%s SameOriginal=%d"), *Observed.Describe(),
					Observed.Identity.HasSameIndexAndSerialNumber(Expected.Identity));
			};
			const FString PlayerControllerRoute = Player
				? DescribeRoute(Player->PlayerController, ControllerDiagnostic) : TEXT("NotReadable.PlayerInvalid");
			const FString ControllerPlayerRoute = Controller
				? DescribeRoute(Controller->GetLocalPlayer(), PlayerDiagnostic) : TEXT("NotReadable.ControllerInvalid");
			const FString ControllerInputRoute = Controller
				? DescribeRoute(Controller->PlayerInput, ProducerDiagnostic) : TEXT("NotReadable.ControllerInvalid");
			const FString SubsystemInputRoute = Subsystem
				? DescribeRoute(Subsystem->GetPlayerInput(), ProducerDiagnostic) : TEXT("NotReadable.SubsystemInvalid");
			const UGGYGOLocalPlayer* Host = Cast<UGGYGOLocalPlayer>(Player);
			const FString PlayerResourceRoute = Host
				? DescribeRoute(Host->GetMovementInputOriginResource(), ResourceDiagnostic) : TEXT("NotReadable.ProjectPlayerInvalid");
			const FString ResourceOuterRoute = Resource
				? DescribeRoute(Resource->GetOuter(), PlayerDiagnostic) : TEXT("NotReadable.ResourceInvalid");
			const UGGYGOPlayerInput* Producer = Cast<UGGYGOPlayerInput>(ProducerDiagnostic.Identity.Get());
			const FString ProducerOuterRoute = Producer
				? DescribeRoute(Producer->GetOuter(), ControllerDiagnostic) : TEXT("NotReadable.ProducerInvalid");
			const FWorldContext* Context = World && GEngine ? GEngine->GetWorldContextFromWorld(World) : nullptr;
			const FString ContextRoute = Context
				? FString::Printf(TEXT("Context=%p World=%s GI=%s GICurrentContextSame=%d WorldType=%d"),
					static_cast<const void*>(Context), *DescribeRoute(Context->World(), WorldDiagnostic),
					*DescribeRoute(Context->OwningGameInstance, GIDiagnostic), GI && GI->GetWorldContext() == Context,
					static_cast<int32>(Context->WorldType))
				: TEXT("NotReadable.WorldInvalidOrContextMissing");
			const UGGYGOInputTestHeroComponent* Observer = Pawn ? Pawn->GetHeroForTest() : nullptr;
			const FString Rebuild = IsValid(Observer)
				? FString::Printf(TEXT("Observing=%d ActualCount=%d FirstActualTime=%.9f"),
					Observer->IsObservingNativeMappingRebuildForTest(), Observer->GetNativeMappingRebuildCountForTest(),
					Observer->GetFirstNativeMappingRebuildTimeForTest())
				: TEXT("NotReadable.OriginalPawnOrObserverInvalid");
			const FString Snapshot = FString::Printf(TEXT("[Input.NativeBirthSnapshot] Stage=%s Frame=%llu Qualification=%s\nLP=%s\nGI=%s\nPC=%s ActorBeingDestroyed=%d\nProducer=%s\nResource=%s\nPawn=%s\nSubsystem=%s\nWorld=%s\nLP.PC=%s\nPC.LP=%s\nPC.PlayerInput=%s\nSubsystem.PlayerInput=%s\nLP.OriginResource=%s\nResource.Outer=%s\nProducer.Outer=%s\nWorldContext=%s\nRebuild=%s"),
				Stage, static_cast<unsigned long long>(Frame), *Qualification,
				*PlayerDiagnostic.Describe(), *GIDiagnostic.Describe(), *ControllerDiagnostic.Describe(),
				Controller ? Controller->IsActorBeingDestroyed() : -1, *ProducerDiagnostic.Describe(),
				*ResourceDiagnostic.Describe(), *PawnDiagnostic.Describe(), *SubsystemDiagnostic.Describe(),
				*WorldDiagnostic.Describe(), *PlayerControllerRoute, *ControllerPlayerRoute, *ControllerInputRoute,
				*SubsystemInputRoute, *PlayerResourceRoute, *ResourceOuterRoute, *ProducerOuterRoute, *ContextRoute, *Rebuild);
			UE_LOG(LogGGYGONativeBirthDiagnostics, Verbose, TEXT("%s"), *Snapshot);
		}

		bool RunDigitalAssertions()
		{
			UGGYGOPlayerInput* Source = OriginalProducer.Get();
			AGGYGOInputTestPawn* Pawn = Fixture.GetPawn();
			if (!Test.TestTrue(TEXT("original native source/route survived the actual rebuild"),
				IsValid(Source) && IsValid(Pawn) && Fixture.GetPlayerInput() == Source
				&& Fixture.GetInputSubsystem()->GetPlayerInput() == Source)) { return false; }
			const UInputAction* MoveAction = Fixture.GetMoveAction();
			TSet<FKey> ActualMovementKeys;
			int32 ActualMovementMappings = 0;
			for (const FEnhancedActionKeyMapping& Mapping : Source->GetEnhancedActionMappingsView())
			{
				if (Mapping.Action == MoveAction)
				{
					++ActualMovementMappings;
					ActualMovementKeys.Add(Mapping.Key.GetVirtualKey());
				}
			}
			if (!Test.TestTrue(TEXT("first default Hero installation produced the actual W/A/S/D mappings"),
				ActualMovementMappings == 4 && ActualMovementKeys.Num() == 4 && ActualMovementKeys.Contains(EKeys::W)
				&& ActualMovementKeys.Contains(EKeys::A) && ActualMovementKeys.Contains(EKeys::S) && ActualMovementKeys.Contains(EKeys::D))
				|| !CheckCold(TEXT("after actual first mapping rebuild, before Begin"))) { return false; }

			FString Error;
			if (!Test.TestTrue(TEXT("production Begin issued and claimed the real source Session"),
				Source->BeginMovementInputSession(Pawn, Pawn->GetInputComponentForTest(), MoveAction, Session, Error)))
			{
				Test.AddError(TEXT("[Input.NativeBirthTest] Begin: ") + Error);
				return false;
			}
			if (!Test.TestTrue(TEXT("Begin returned a nonzero Session of the original native Producer"),
				Session.SessionSerial != 0 && Session.Producer.Get() == Source)
				|| !CheckCold(TEXT("after production Begin Claim, before any request"))) { return false; }
			Binding.Consumer = Pawn->GetInputComponentForTest();
			Binding.ConsumerBindingSerial = 1; // This probe issues one actual consumer binding, never a Source identity.
			Binding.SourceSession = Session;
			FGGYGOMovementInputFactDelegate Receiver = FGGYGOMovementInputFactDelegate::CreateLambda(
				[this](const FGGYGOMovementInputConsumerBindingId& OriginalBinding, const FGGYGOMovementInputFact& Fact)
				{
					Test.TestTrue(TEXT("every native fact retains the original consumer binding and Source Session"),
						OriginalBinding == Binding && Fact.Request.Session == Session && Fact.EventSerial != 0);
					Facts.Add(Fact);
					if (Fact.Kind == EGGYGOMovementInputFactKind::RequestStarted)
					{
						Test.TestTrue(TEXT("Started callback observes original qualification already Rearm"),
							IsValid(OriginalResource.Get()) && OriginalResource->GetQualification(OriginalProducer.Get())
								== EGGYGOMovementInputOriginQualification::Rearm);
						FGGYGOMovementInputRequestIdentity Committed;
						FString LookupError;
						Test.TestTrue(TEXT("Started callback can query the exact actually committed nonzero request"),
							IsValid(OriginalProducer.Get()) && OriginalProducer->GetMovementInputRequest(Session, Committed, LookupError)
								&& Committed == Fact.Request && Committed.RequestSerial != 0);
						Test.TestTrue(TEXT("Started contains only its explicit proof, no Session mode"),
							Fact.SessionMode == EGGYGOMovementInputSessionMode::Invalid && Fact.StartProof != EGGYGOMovementInputStartProof::Invalid);
					}
					else if (Fact.Kind != EGGYGOMovementInputFactKind::SessionOpened)
					{
						Test.TestTrue(TEXT("ordinary/terminal facts contain neither mode nor StartProof"),
							Fact.SessionMode == EGGYGOMovementInputSessionMode::Invalid && Fact.StartProof == EGGYGOMovementInputStartProof::Invalid);
					}
				});
			if (!Test.TestTrue(TEXT("production Attach accepted the original receiver"),
				Source->AttachMovementInputReceiver(Session, Binding, MoveTemp(Receiver), Error)))
			{
				Test.AddError(TEXT("[Input.NativeBirthTest] Attach: ") + Error);
				return false;
			}
			if (!Test.TestEqual(TEXT("untouched W/A/S/D produce only Opened, no fake Neutral or Unresolved"), Facts.Num(), 1)) { return false; }
			if (!Test.TestTrue(TEXT("Opened explicitly reports Cold with no request/proof"),
				Facts[0].Kind == EGGYGOMovementInputFactKind::SessionOpened && Facts[0].Request.RequestSerial == 0
				&& Facts[0].SessionMode == EGGYGOMovementInputSessionMode::Cold && Facts[0].StartProof == EGGYGOMovementInputStartProof::Invalid)) { return false; }
			FGGYGOMovementInputRequestIdentity Request;
			if (!Test.TestFalse(TEXT("before the first injected press no request exists"), Source->GetMovementInputRequest(Session, Request, Error))) { return false; }

			const FInputDeviceId Device = FInputDeviceId::CreateFromInternalId(0);
			const FInputKeyEventArgs FirstPress(nullptr, Device, EKeys::W, IE_Pressed, 1.0f, false, FPlatformTime::Cycles64());
			if (!Test.TestTrue(TEXT("public digital injection has a valid test device and no simulated-input flag"),
				Device.IsValid() && !FirstPress.IsSimulatedInput())) { return false; }
			// Public input injection exercises InputKey; it is not a physical keyboard/PIE/Run claim.
			Source->InputKey(FirstPress);
			if (!Test.TestEqual(TEXT("first W with A/S/D untouched issues exactly one Started"), Facts.Num(), 2)) { return false; }
			if (!Test.TestTrue(TEXT("first actual request uses ColdPhysicalPress, without prior Neutral"),
				Facts[1].Kind == EGGYGOMovementInputFactKind::RequestStarted && Facts[1].Request.RequestSerial != 0
				&& Facts[1].StartProof == EGGYGOMovementInputStartProof::ColdPhysicalPress)) { return false; }
			const FGGYGOMovementInputRequestIdentity FirstRequest = Facts[1].Request;
			Source->InputKey(FInputKeyEventArgs(nullptr, Device, EKeys::W, IE_Released, 0.0f, false, FPlatformTime::Cycles64()));
			if (!Test.TestEqual(TEXT("real-mechanism release reserves Released then Neutral"), Facts.Num(), 4)) { return false; }
			if (!Test.TestTrue(TEXT("release closes the exact first request before neutral"),
				Facts[2].Kind == EGGYGOMovementInputFactKind::RequestReleased && Facts[2].Request == FirstRequest
				&& Facts[3].Kind == EGGYGOMovementInputFactKind::NeutralConfirmed && Facts[3].Request.RequestSerial == 0)
				|| !Test.TestFalse(TEXT("released request is no longer queryable"), Source->GetMovementInputRequest(Session, Request, Error))) { return false; }
			Source->InputKey(FInputKeyEventArgs(nullptr, Device, EKeys::W, IE_Pressed, 1.0f, false, FPlatformTime::Cycles64()));
			if (!Test.TestEqual(TEXT("press after actual release produces the second Started"), Facts.Num(), 5)) { return false; }
			if (!Test.TestTrue(TEXT("second request keeps the original Session and uses a larger ID with release proof"),
				Facts[4].Kind == EGGYGOMovementInputFactKind::RequestStarted && Facts[4].Request.Session == Session
				&& Facts[4].Request.RequestSerial > FirstRequest.RequestSerial
				&& Facts[4].StartProof == EGGYGOMovementInputStartProof::ReleasedThenPhysicalPress)) { return false; }
			bDigitalSequenceCompleted = true;
			return true;
		}

		void Cleanup()
		{
			if (bCleaned) { return; }
			bCleaned = true;
			if (AGGYGOInputTestPawn* Pawn = Fixture.GetPawn(); IsValid(Pawn))
			{
				Pawn->GetHeroForTest()->StopObservingNativeMappingRebuildForTest();
				Test.TestFalse(TEXT("own original mapping observer detached before session/fixture cleanup"),
					Pawn->GetHeroForTest()->IsObservingNativeMappingRebuildForTest());
			}
			if (Session.SessionSerial != 0 && IsValid(OriginalProducer.Get()))
			{
				OriginalProducer->EndMovementInputSession(Session, TEXT("NativeBirthTestCleanup"));
				FGGYGOMovementInputRequestIdentity Request;
				FString Error;
				Test.TestFalse(TEXT("ended original Session cannot expose a request"), OriginalProducer->GetMovementInputRequest(Session, Request, Error));
			}
			if (bDigitalSequenceCompleted)
			{
				if (Test.TestEqual(TEXT("exact native sequence ends with one original Invalidated"), Facts.Num(), 6))
				{
					Test.TestTrue(TEXT("Invalidated retains the original second request"),
						Facts[5].Kind == EGGYGOMovementInputFactKind::SessionInvalidated && Facts[5].Request == Facts[4].Request);
					for (int32 Index = 1; Index < Facts.Num(); ++Index)
					{
						Test.TestTrue(TEXT("all actual events use strictly increasing original serials"), Facts[Index].EventSerial > Facts[Index - 1].EventSerial);
					}
				}
			}
			Fixture.Shutdown();
			if (OriginalResource.Get())
			{
				Test.TestTrue(TEXT("original resource is unavailable after the real LP/Controller cleanup"),
					OriginalResource->GetQualification(OriginalProducer.Get()) == EGGYGOMovementInputOriginQualification::Unavailable);
			}
			if (OriginalPlayer.Get()) { Test.TestNull(TEXT("original LocalPlayer subsystems deinitialized"), OriginalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>()); }
			if (OriginalGI.Get())
			{
				Test.TestEqual(TEXT("own native LocalPlayer removed"), OriginalGI->GetNumLocalPlayers(), 0);
				Test.TestNull(TEXT("own native GI context detached"), OriginalGI->GetWorldContext());
			}
			if (OriginalWorld && GEngine) { Test.TestNull(TEXT("own native world context removed"), GEngine->GetWorldContextFromWorld(OriginalWorld)); }
			Test.TestEqual(TEXT("native probe preserved the original global world"), static_cast<UWorld*>(GWorld), OriginalGlobalWorld);
			Test.TestEqual(TEXT("native probe preserved the original global viewport"), GEngine ? GEngine->GameViewport.Get() : nullptr, OriginalGlobalViewport);
		}

		FAutomationTestBase& Test;
		FGGYGOInputTestFixture Fixture;
		TStrongObjectPtr<ULocalPlayer> OriginalPlayer;
		TStrongObjectPtr<UGGYGOInputTestGameInstance> OriginalGI;
		TStrongObjectPtr<UGGYGOPlayerInput> OriginalProducer;
		TStrongObjectPtr<UGGYGOMovementInputOriginResource> OriginalResource;
		UWorld* OriginalWorld = nullptr;
		UWorld* OriginalGlobalWorld = nullptr;
		UGameViewportClient* OriginalGlobalViewport = nullptr;
		FGGYGOMovementInputSessionIdentity Session;
		FGGYGOMovementInputConsumerBindingId Binding;
		TArray<FGGYGOMovementInputFact> Facts;
		FNativeWeakObjectDiagnostic PlayerDiagnostic;
		FNativeWeakObjectDiagnostic GIDiagnostic;
		FNativeWeakObjectDiagnostic ProducerDiagnostic;
		FNativeWeakObjectDiagnostic ResourceDiagnostic;
		FNativeWeakObjectDiagnostic ControllerDiagnostic;
		FNativeWeakObjectDiagnostic PawnDiagnostic;
		FNativeWeakObjectDiagnostic SubsystemDiagnostic;
		FNativeWeakObjectDiagnostic WorldDiagnostic;
		bool bDigitalSequenceCompleted = false;
		bool bCleaned = false;
	};

	class FWaitForNativeMovementOriginRebuild : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForNativeMovementOriginRebuild(TSharedRef<FNativeMovementOriginProbe> InProbe)
			: Probe(MoveTemp(InProbe)), Deadline(FPlatformTime::Seconds() + 10.0)
		{}

		virtual bool Update() override
		{
			if (!Probe->CheckCold(TEXT("waiting for actual first rebuild notification")))
			{
				Probe->Cleanup();
				return true;
			}
			AGGYGOInputTestPawn* Pawn = Probe->Fixture.GetPawn();
			UGGYGOInputTestHeroComponent* Observer = IsValid(Pawn) ? Pawn->GetHeroForTest() : nullptr;
			if (!IsValid(Observer) || !Observer->IsObservingNativeMappingRebuildForTest())
			{
				Probe->Test.AddError(TEXT("[Input.NativeBirthTest] Original native rebuild observer/route lost; no successor is adopted."));
				Probe->Cleanup();
				return true;
			}
			if (Observer->GetNativeMappingRebuildCountForTest() == 0)
			{
				if (FPlatformTime::Seconds() < Deadline) { return false; }
				Probe->Test.AddError(TEXT("[Input.NativeBirthTest] 10-second timeout awaiting the original engine mapping rebuild notification; no real rebuild frame arrived. No immediate rebuild, manual Tick/Broadcast or qualification substitution is permitted."));
				Probe->Cleanup();
				return true;
			}
			if (Observer->GetFirstNativeMappingRebuildTimeForTest() > Deadline)
			{
				Probe->Test.AddError(TEXT("[Input.NativeBirthTest] Original first rebuild notification arrived after the 10-second deadline; late readiness cannot turn the timeout into success."));
				Probe->Cleanup();
				return true;
			}
			Probe->RunDigitalAssertions();
			Probe->Cleanup();
			return true;
		}

	private:
		TSharedRef<FNativeMovementOriginProbe> Probe;
		double Deadline;
	};

#if WITH_EDITOR
	/** Owns one native PIE request/session; the engine remains the only game-frame executor. */
	class FRunNativeMovementOriginWithOwnedPIE : public IAutomationLatentCommand
	{
	public:
		explicit FRunNativeMovementOriginWithOwnedPIE(FAutomationTestBase& InTest)
			: Test(InTest), OuterWorld(GWorld), OuterViewport(GEngine ? GEngine->GameViewport.Get() : nullptr)
		{}

		virtual ~FRunNativeMovementOriginWithOwnedPIE() override
		{
			if (Stage != EStage::Complete)
			{
				Fail(TEXT("latent command aborted before owned PIE cleanup completed"));
				Stage = EStage::Ending;
				CleanupProbe();
				CloseOwnedSession(true); // Native synchronous teardown when the latent queue is being discarded.
				VerifyReleased();
			}
			RemoveObservers();
		}

		virtual bool Update() override
		{
			if (Stage == EStage::Complete) { return true; }
			if (Stage == EStage::NotStarted)
			{
				Start();
				return false;
			}
			if (Stage == EStage::Ending) { return UpdateEnding(); }
			if (bFailed)
			{
				BeginEnding();
				return false;
			}
			if (Stage == EStage::Starting)
			{
				if (FPlatformTime::Seconds() >= StartupDeadline)
				{
					Fail(TEXT("10-second startup timeout: no real owned PostPIEStarted/ready context"));
					BeginEnding();
					return false;
				}
				if (bPostPIEStarted)
				{
					if (!IsOwnedContextCurrent())
					{
						Fail(TEXT("owned context/world/GI replaced or lost during startup"));
						BeginEnding();
						return false;
					}
					UWorld* World = OwnedWorld.Get();
					AGameModeBase* Mode = World->GetAuthGameMode();
					if (IsValid(Mode) && Mode->GetClass() != AGameModeBase::StaticClass())
					{
						Fail(TEXT("native PIE did not use the explicit GameModeBase test configuration"));
						BeginEnding();
						return false;
					}
					AGameStateBase* GameState = World->GetGameState();
					if (World->AreActorsInitialized() && World->HasBegunPlay()
						&& IsValid(Mode) && IsValid(GameState) && GameState->HasMatchStarted())
					{
						Probe = MakeShared<FNativeMovementOriginProbe>(Test);
						if (!Probe->Initialize())
						{
							BeginEnding();
							return false;
						}
						Wait = MakeUnique<FWaitForNativeMovementOriginRebuild>(Probe.ToSharedRef());
						Stage = EStage::Running;
						return false;
					}
				}
				return false;
			}
			if (!IsOwnedContextCurrent())
			{
				Fail(TEXT("owned PIE ended early or its context/world/GI was replaced during the original wait"));
				BeginEnding();
				return false;
			}
			// This is the original latent condition, invoked by the existing automation scheduler.
			if (Wait->Update()) { BeginEnding(); }
			return false;
		}

	private:
		enum class EStage : uint8 { NotStarted, Starting, Running, Ending, Complete };

		void Fail(const TCHAR* Reason)
		{
			if (bFailed) { return; }
			bFailed = true;
			Test.AddError(FString::Printf(TEXT("[Input.OwnedPIETest] Stage=%u Context=%s World=%s GI=%s: %s"),
				static_cast<uint32>(Stage), *OwnedContext.ToString(), *GetPathNameSafe(OwnedWorld.Get()),
				*GetPathNameSafe(OwnedGI.Get()), Reason));
		}

		int32 CountPIEContexts() const
		{
			const UEditorEngine* Engine = Editor.Get();
			int32 Count = 0;
			if (Engine)
			{
				for (const FWorldContext& Context : Engine->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE) { ++Count; }
				}
			}
			return Count;
		}

		bool IsOwnedSessionCurrent() const
		{
			const UEditorEngine* Engine = Editor.Get();
			if (!Engine || !RequestSettings.IsValid()) { return false; }
			const TOptional<FPlayInEditorSessionInfo> Info = Engine->GetPlayInEditorSessionInfo();
			return Info.IsSet() && Info->OriginalRequestParams.EditorPlaySettings == RequestSettings.Get()
				&& Info->OriginalRequestParams.SessionDestination == EPlaySessionDestinationType::InProcess
				&& Info->OriginalRequestParams.GameModeOverride == AGameModeBase::StaticClass();
		}

		const FWorldContext* FindOwnedContext() const
		{
			const UEditorEngine* Engine = Editor.Get();
			if (!Engine || OwnedContext.IsNone()) { return nullptr; }
			for (const FWorldContext& Context : Engine->GetWorldContexts())
			{
				if (Context.ContextHandle == OwnedContext) { return &Context; }
			}
			return nullptr;
		}

		bool IsOwnedContextCurrent() const
		{
			const UEditorEngine* Engine = Editor.Get();
			const FWorldContext* Context = FindOwnedContext();
			return IsOwnedSessionCurrent() && CountPIEContexts() == 1 && Context
				&& Context->WorldType == EWorldType::PIE && OwnedWorld.IsValid() && OwnedGI.IsValid()
				&& Context->World() == OwnedWorld.Get() && Context->OwningGameInstance == OwnedGI.Get()
				&& Engine->PlayWorld == OwnedWorld.Get() && !Engine->ShouldEndPlayMap();
		}

		void Start()
		{
			Stage = EStage::Starting;
			Editor = Cast<UEditorEngine>(GEngine);
			UEditorEngine* Engine = Editor.Get();
			if (!Engine || Engine != GEditor)
			{
				Fail(TEXT("native owned PIE requires the current EditorEngine"));
				return;
			}
			if (Engine->IsPlaySessionInProgress() || Engine->PlayWorld || Engine->ShouldEndPlayMap()
				|| Engine->IsSettingUpPlayWorld() || CountPIEContexts() != 0)
			{
				Fail(TEXT("existing, queued or initializing PIE is not owned by this test"));
				return;
			}
			TStrongObjectPtr<ULevelEditorPlaySettings> Settings(
				NewObject<ULevelEditorPlaySettings>(GetTransientPackage(), NAME_None, RF_Transient));
			Settings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
			Settings->SetRunUnderOneProcess(true);
			Settings->SetPlayNumberOfClients(1);
			Settings->bLaunchSeparateServer = false;
			Settings->GameGetsMouseControl = false;
			Settings->bShouldMinimizeEditorOnNonVRPIE = false;
			FRequestPlaySessionParams Params;
			Params.EditorPlaySettings = Settings.Get();
			Params.GameModeOverride = AGameModeBase::StaticClass();
			Params.bAllowOnlineSubsystem = false;
			PreHandle = FEditorDelegates::PreBeginPIE.AddRaw(this, &FRunNativeMovementOriginWithOwnedPIE::OnPreBegin);
			PostHandle = FEditorDelegates::PostPIEStarted.AddRaw(this, &FRunNativeMovementOriginWithOwnedPIE::OnPostStarted);
			EndHandle = FEditorDelegates::EndPIE.AddRaw(this, &FRunNativeMovementOriginWithOwnedPIE::OnEnded);
			CancelHandle = FEditorDelegates::CancelPIE.AddRaw(this, &FRunNativeMovementOriginWithOwnedPIE::OnCancelled);
			StartupDeadline = FPlatformTime::Seconds() + 10.0;
			Engine->RequestPlaySession(Params);
			const TOptional<FRequestPlaySessionParams> Queued = Engine->GetPlaySessionRequest();
			if (!Queued.IsSet() || !IsValid(Queued->EditorPlaySettings.Get()))
			{
				Fail(TEXT("native request did not retain its actual copied startup settings"));
				return;
			}
			RequestSettings = Queued->EditorPlaySettings.Get();
		}

		void OnPreBegin(bool bSimulating)
		{
			const UEditorEngine* Engine = Editor.Get();
			const TOptional<FRequestPlaySessionParams> Queued = Engine ? Engine->GetPlaySessionRequest() : TOptional<FRequestPlaySessionParams>();
			if (Stage != EStage::Starting || bSimulating || !Queued.IsSet()
				|| Queued->EditorPlaySettings != RequestSettings.Get())
			{
				Fail(TEXT("PreBeginPIE belongs to a different request or mode"));
				return;
			}
			bPreBeginObserved = true;
		}

		void OnPostStarted(bool bSimulating)
		{
			UEditorEngine* Engine = Editor.Get();
			if (Stage != EStage::Starting || bSimulating || !bPreBeginObserved
				|| !IsOwnedSessionCurrent() || CountPIEContexts() != 1)
			{
				Fail(TEXT("PostPIEStarted does not identify the sole native owned request"));
				return;
			}
			const FWorldContext* Context = Engine->GetWorldContextFromWorld(Engine->PlayWorld);
			if (!Context || Context->WorldType != EWorldType::PIE
				|| !IsValid(Context->World()) || !IsValid(Context->OwningGameInstance.Get()))
			{
				Fail(TEXT("PostPIEStarted has no valid exact PIE context/world/GI"));
				return;
			}
			OwnedContext = Context->ContextHandle;
			OwnedWorld = Context->World();
			OwnedGI = Context->OwningGameInstance.Get();
			bPostPIEStarted = true;
		}

		void OnEnded(bool)
		{
			if (Stage != EStage::Ending && IsOwnedSessionCurrent())
			{
				Fail(TEXT("native owned PIE ended before original Probe completion"));
			}
		}

		void OnCancelled()
		{
			if (Stage == EStage::Starting) { Fail(TEXT("native PIE startup request was cancelled")); }
		}

		void CleanupProbe()
		{
			Wait.Reset();
			if (Probe.IsValid()) { Probe->Cleanup(); }
			Probe.Reset();
		}

		void CloseOwnedSession(bool bSynchronous)
		{
			UEditorEngine* Engine = Editor.Get();
			if (!Engine) { return; }
			if (IsOwnedSessionCurrent())
			{
				// The native end entry affects the whole PIE session: never call it with any foreign context.
				for (const FWorldContext& Context : Engine->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE
						&& (!OwnedContext.IsNone() ? Context.ContextHandle != OwnedContext : Context.World() != Engine->PlayWorld))
					{
						Fail(TEXT("foreign PIE context prevents exact owned-session teardown"));
						return;
					}
				}
				if (bSynchronous || !Engine->PlayWorld) { Engine->EndPlayMap(); }
				else { Engine->RequestEndPlayMap(); }
				return;
			}
			const TOptional<FRequestPlaySessionParams> Queued = Engine->GetPlaySessionRequest();
			if (Queued.IsSet() && RequestSettings.IsValid() && Queued->EditorPlaySettings == RequestSettings.Get())
			{
				Engine->CancelRequestPlaySession();
			}
		}

		void BeginEnding()
		{
			Stage = EStage::Ending;
			CleanupProbe();
			EndingDeadline = FPlatformTime::Seconds() + 10.0;
			CloseOwnedSession(false);
		}

		void VerifyReleased()
		{
			UEditorEngine* Engine = Editor.Get();
			if (!Test.TestNotNull(TEXT("EditorEngine remains available for exact PIE cleanup proof"), Engine)) { return; }
			Test.TestTrue(TEXT("owned PIE context removed after native teardown"), FindOwnedContext() == nullptr);
			Test.TestTrue(TEXT("test's native PIE request/session released"), !IsOwnedSessionCurrent()
				&& (!Engine || !Engine->IsPlaySessionInProgress()));
			Test.TestTrue(TEXT("no PIE context remains after this exclusive test"), CountPIEContexts() == 0);
			Test.TestTrue(TEXT("outer global World restored by native PIE teardown"), GWorld == OuterWorld);
			Test.TestTrue(TEXT("outer global viewport restored by native PIE teardown"),
				GEngine && GEngine->GameViewport.Get() == OuterViewport);
		}

		bool UpdateEnding()
		{
			UEditorEngine* Engine = Editor.Get();
			if (FPlatformTime::Seconds() >= EndingDeadline)
			{
				Fail(TEXT("10-second teardown timeout before exact native request/session/context release was observed"));
				CloseOwnedSession(true); // Failure remains visible; this only releases exact owned resources.
			}
			else if (FindOwnedContext() || IsOwnedSessionCurrent()
				|| (Engine && Engine->IsPlaySessionInProgress())) { return false; }
			Stage = EStage::Complete;
			VerifyReleased();
			RemoveObservers();
			return true;
		}

		void RemoveObservers()
		{
			FEditorDelegates::PreBeginPIE.Remove(PreHandle);
			FEditorDelegates::PostPIEStarted.Remove(PostHandle);
			FEditorDelegates::EndPIE.Remove(EndHandle);
			FEditorDelegates::CancelPIE.Remove(CancelHandle);
			PreHandle.Reset();
			PostHandle.Reset();
			EndHandle.Reset();
			CancelHandle.Reset();
		}

		FAutomationTestBase& Test;
		TWeakObjectPtr<UEditorEngine> Editor;
		TWeakObjectPtr<ULevelEditorPlaySettings> RequestSettings;
		TWeakObjectPtr<UWorld> OwnedWorld;
		TWeakObjectPtr<UGameInstance> OwnedGI;
		FName OwnedContext;
		UWorld* OuterWorld;
		UGameViewportClient* OuterViewport;
		TSharedPtr<FNativeMovementOriginProbe> Probe;
		TUniquePtr<FWaitForNativeMovementOriginRebuild> Wait;
		FDelegateHandle PreHandle, PostHandle, EndHandle, CancelHandle;
		EStage Stage = EStage::NotStarted;
		double StartupDeadline = 0.0;
		double EndingDeadline = 0.0;
		bool bPreBeginObserved = false;
		bool bPostPIEStarted = false;
		bool bFailed = false;
	};
#endif // WITH_EDITOR
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGONativeMovementOriginFirstPressTest,
	"GGYGO.Input.MovementOrigin.NativeBirthFirstDigitalPress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGONativeMovementOriginFirstPressTest::RunTest(const FString& Parameters)
{
#if WITH_EDITOR
	ADD_LATENT_AUTOMATION_COMMAND(FRunNativeMovementOriginWithOwnedPIE(*this));
	return true;
#else
	AddError(TEXT("[Input.OwnedPIETest] This EditorContext leaf requires WITH_EDITOR native PIE support."));
	return false;
#endif
}
#endif // WITH_DEV_AUTOMATION_TESTS
