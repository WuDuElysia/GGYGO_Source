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
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Input/GGYGOInputComponent.h"
#include "Input/GGYGOInputConfig.h"
#include "Input/GGYGOMovementInputOriginResource.h"
#include "Input/GGYGOPlayerInput.h"
#include "InputAction.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
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
	// 需要 GI 子系统（如 MessageRouter）的夹具显式选原生 Init，与 Shutdown 成对。
	if (bNativeLifecycle) { Super::Init(); }
}

void UGGYGOInputTestGameInstance::Shutdown()
{
	if (bNativeLifecycle)
	{
		Super::Shutdown();
		return;
	}
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

AGGYGOInputTestGameMode::AGGYGOInputTestGameMode(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PlayerControllerClass = AGGYGOInputTestController::StaticClass();
}

bool UGGYGOInputTestHeroComponent::HasAbilityInputAssociationForTest(const UGGYGOAbilitySystemComponent* ExpectedASC) const
{
	return ExpectedASC && GetInputSessionAbilitySystem() == ExpectedASC;
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

void AGGYGOInputTestController::TickInputForTest(float DeltaTime)
{
	TickPlayerInput(DeltaTime, false);
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

UAbilitySystemComponent* AGGYGOInputTestPawn::GetAbilitySystemComponent() const
{
	return PawnExtension ? PawnExtension->GetGGYGOAbilitySystemComponent() : nullptr;
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

bool AGGYGOInputTestPawn::EnableSourceMovementProbeForTest()
{
	if (InputComponent || bSourceMovementProbe) { return false; }
	bSourceMovementProbe = true;
	return true;
}

void AGGYGOInputTestPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	if (!bSourceMovementProbe) { Hero->InitializePlayerInput(PlayerInputComponent); }
}

#if WITH_DEV_AUTOMATION_TESTS
struct FGGYGOInputTestFixture::FState
{
	bool bNativeMovementOrigin = false;
	bool bGameInstanceSubsystems = false;
	// 未设置时能力动作不映射任何键，只能经 ExecuteAbilityBinding 在绑定边界注入。
	FKey AbilityKey;
	TFunction<bool(AGGYGOInputTestPawn&)> PawnPreparation;
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
	// 仅记录 Source 探针自己的一份注册，归还时不得作用于后继 PlayerInput。
	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> NativeMappingSubsystem;
	TWeakObjectPtr<UEnhancedPlayerInput> NativeMappingPlayerInput;
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
	if (State->bGameInstanceSubsystems) { State->GameInstance->UseNativeLifecycleForTest(); }
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
		// Source 探针通过原 Subsystem 的公开入口安装自己的 IMC，等待真实重建。
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
	if (State->AbilityKey.IsValid())
	{
		State->Mapping->MapKey(State->AbilityAction.Get(), State->AbilityKey);
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
	if (State->PawnPreparation)
	{
		const TFunction<bool(AGGYGOInputTestPawn&)> Preparation = MoveTemp(State->PawnPreparation);
		if (!Preparation(*State->Pawn))
		{
			Test.AddError(FString::Printf(TEXT("[Input.Fixture] Pawn=%s Reason=PawnPreparationBeforeBindingFailed."),
				*GetPathNameSafe(State->Pawn)));
			return false;
		}
	}
	if (State->bNativeMovementOrigin
		&& !Test.TestTrue(TEXT("select Source probe ownership before the first input setup"),
			State->Pawn->EnableSourceMovementProbeForTest())) { return false; }
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
		State->Pawn->GetHeroForTest()->ConfigureMappingForTest(State->Mapping.Get(), 0);
	}
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
		const TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> OriginalSubsystem(GetInputSubsystem());
		const TWeakObjectPtr<UEnhancedPlayerInput> OriginalPlayerInput(GetPlayerInput());
		if (!Test.TestTrue(TEXT("Source IMC registration uses the original native input route"),
			OriginalSubsystem.IsValid() && OriginalPlayerInput.IsValid()
			&& OriginalSubsystem->GetPlayerInput() == OriginalPlayerInput.Get())) { return false; }
		// Add 会同步广播；先记录本次归还义务，不让 Hero 再创建另一份 Source Session。
		State->NativeMappingSubsystem = OriginalSubsystem;
		State->NativeMappingPlayerInput = OriginalPlayerInput;
		OriginalSubsystem->AddMappingContext(State->Mapping.Get(), 0);
		return RequireNativeMovementCold(Test, TEXT("fixture initialized/pending native rebuild"))
			&& Test.TestTrue(TEXT("native source route ready, waiting for the actual mapping rebuild"),
				OriginalSubsystem.IsValid() && OriginalPlayerInput.IsValid()
				&& State->NativeMappingSubsystem == OriginalSubsystem
				&& State->NativeMappingPlayerInput == OriginalPlayerInput
				&& GetInputSubsystem() == OriginalSubsystem.Get() && GetPlayerInput() == OriginalPlayerInput.Get()
				&& IsValid(State->Pawn) && State->Pawn->IsLocallyControlled()
				&& OriginalSubsystem->GetPlayerInput() == OriginalPlayerInput.Get()
				&& OriginalSubsystem->HasMappingContext(State->Mapping.Get()));
	}
	// Actual Hero initialization associates its original ASC after the real coordinator
	// initialized it. No fabricated notification or private session-validity override.
	State->Pawn->GetHeroForTest()->InitializePlayerInput(State->Pawn->GetInputComponentForTest());
	return Test.TestTrue(TEXT("real local Hero source gate established"), State->Pawn->IsLocallyControlled()
		&& State->Pawn->GetASCForTest()->GetAvatarActor() == State->Pawn
		&& State->Pawn->GetHeroForTest()->HasAbilityInputAssociationForTest(State->Pawn->GetASCForTest())
		&& !State->Pawn->GetASCForTest()->OnAbilityInputRetryable.IsBoundToObject(State->Pawn->GetHeroForTest())
		&& !State->Pawn->GetASCForTest()->OnAbilityGroupFreed.IsBoundToObject(State->Pawn->GetHeroForTest())
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
		State->Pawn->GetPawnExtensionForTest()->UninitializeAbilitySystem();
		State->Pawn->GetHeroForTest()->StopObservingLocalAbilitySystemForTest();
	}
	// Pawn 销毁可能连带销毁无 PlayerState 的 Controller；先归还原输入的 IMC。
	const TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> RegisteredSubsystem = State->NativeMappingSubsystem;
	const TWeakObjectPtr<UEnhancedPlayerInput> RegisteredPlayerInput = State->NativeMappingPlayerInput;
	State->NativeMappingSubsystem.Reset();
	State->NativeMappingPlayerInput.Reset();
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = RegisteredSubsystem.Get())
	{
		if (RegisteredPlayerInput.IsValid() && Subsystem->GetPlayerInput() == RegisteredPlayerInput.Get())
		{
			// 记录先摘除，Remove 的同步广播不能重复归还；CountRegistrations 只减自己的一份。
			Subsystem->RemoveMappingContext(State->Mapping.Get());
		}
		else
		{
			UE_LOG(LogGGYGONativeBirthDiagnostics, Error,
				TEXT("[Input.NativeBirthTest] 原 IMC 注册失去 PlayerInput，不向后继输入对象归还旧注册。"));
		}
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
	State->bGameInstanceSubsystems = false;
	State->AbilityKey = FKey();
	State->PawnPreparation.Reset();
}

void FGGYGOInputTestFixture::RequestGameInstanceSubsystemsForTest() { State->bGameInstanceSubsystems = true; }
void FGGYGOInputTestFixture::MapAbilityKeyForTest(const FKey& Key) { State->AbilityKey = Key; }
void FGGYGOInputTestFixture::PreparePawnBeforeBindingForTest(TFunction<bool(AGGYGOInputTestPawn&)> Preparation)
{
	State->PawnPreparation = MoveTemp(Preparation);
}

bool FGGYGOInputTestFixture::CompleteNativeMappingRebuildForTest(FAutomationTestBase& Test)
{
	UEnhancedInputLocalPlayerSubsystem* Subsystem = GetInputSubsystem();
	if (!Test.TestNotNull(TEXT("[Input.Fixture] mapping rebuild uses the original LocalPlayer subsystem"), Subsystem))
	{
		return false;
	}
	// 与 EnhancedInput 模块帧末顺序一致：先编译挂起的映射，再由子系统自己的入口广播重建完成。
	FModifyContextOptions Options;
	Options.bForceImmediately = true;
	Subsystem->RequestRebuildControlMappings(Options);
	Subsystem->ControlMappingsRebuiltThisFrame();
	return true;
}

bool FGGYGOInputTestFixture::PressAbilityKeyForTest(FAutomationTestBase& Test)
{
	return SendAbilityKeyForTest(Test, IE_Pressed);
}

bool FGGYGOInputTestFixture::ReleaseAbilityKeyForTest(FAutomationTestBase& Test)
{
	return SendAbilityKeyForTest(Test, IE_Released);
}

bool FGGYGOInputTestFixture::SendAbilityKeyForTest(FAutomationTestBase& Test, EInputEvent Event)
{
	UEnhancedPlayerInput* PlayerInput = GetPlayerInput();
	if (!State->AbilityKey.IsValid() || !PlayerInput)
	{
		Test.AddError(FString::Printf(TEXT("[Input.Fixture] Key=%s PlayerInput=%s Reason=AbilityKeyRouteUnavailable."),
			*State->AbilityKey.ToString(), *GetPathNameSafe(PlayerInput)));
		return false;
	}
	// 键事件经原 PlayerInput 入队，再走一次 PC 原生输入帧：评价 → Hero 绑定 → PostProcessInput → ASC。
	const bool bPressed = Event == IE_Pressed;
	PlayerInput->InputKey(FInputKeyEventArgs(nullptr, FInputDeviceId::CreateFromInternalId(0), State->AbilityKey,
		Event, bPressed ? 1.0f : 0.0f, false, FPlatformTime::Cycles64()));
	State->Controller->TickInputForTest(1.0f / 60.0f);
	return true;
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
	TestTrue(TEXT("Hero has the real original H/input association"), Hero->HasAbilityInputAssociationForTest(ASC));
	TestFalse(TEXT("Hero does not own retry waiting subscriptions"), ASC->OnAbilityInputRetryable.IsBoundToObject(Hero));
	TestFalse(TEXT("Hero does not own group wake subscriptions"), ASC->OnAbilityGroupFreed.IsBoundToObject(Hero));
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

	ASC->CancelAbilityHandle(ProbeHandle);
	ActivatedSpec = ASC->FindAbilitySpecFromHandle(ProbeHandle);
	if (!TestNotNull(TEXT("probe remains granted before request retirement"), ActivatedSpec)
		|| !TestFalse(TEXT("probe is inactive before pending-request retirement"), ActivatedSpec->IsActive())) { return false; }
	TestEqual(TEXT("a second original request is pending before release"), Fixture.ExecuteAbilityBinding(ETriggerEvent::Triggered), 1);
	Hero->ReleasePlayerInput();
	Fixture.GetController()->ConsumeInputForTest();
	TestEqual(TEXT("retired original request cannot activate after Hero release"), Probe->GetActivationCountForTest(), 1);
	TestFalse(TEXT("original H/input association returned"), Hero->HasAbilityInputAssociationForTest(ASC));
	TestEqual(TEXT("fixture input bindings returned"), Input->GetActionEventBindings().Num(), 0);
	TestFalse(TEXT("fixture IMC returned"), Fixture.GetInputSubsystem()->HasMappingContext(Fixture.GetMappingContext()));
	TestFalse(TEXT("fixture retry subscription returned"), ASC->OnAbilityInputRetryable.IsBoundToObject(Hero));
	TestFalse(TEXT("fixture group subscription returned"), ASC->OnAbilityGroupFreed.IsBoundToObject(Hero));
	Pawn->GetPawnExtensionForTest()->UninitializeAbilitySystem();
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
			if (!Test.TestTrue(TEXT("first Source IMC installation produced the actual W/A/S/D mappings"),
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
							IsValid(OriginalProducer.Get()) && OriginalProducer->QueryMovementInputRequest(Session, Committed, LookupError) == EGGYGOMovementInputRequestQueryResult::Held
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
			if (!Test.TestTrue(TEXT("before the first injected press no request exists"),
				Source->QueryMovementInputRequest(Session, Request, Error) == EGGYGOMovementInputRequestQueryResult::AwaitingPhysicalProof)) { return false; }

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
				|| !Test.TestTrue(TEXT("released request is no longer queryable"),
					Source->QueryMovementInputRequest(Session, Request, Error) == EGGYGOMovementInputRequestQueryResult::NotHeld)) { return false; }
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
				Test.TestTrue(TEXT("ended original Session cannot expose a request"),
					OriginalProducer->QueryMovementInputRequest(Session, Request, Error) == EGGYGOMovementInputRequestQueryResult::Unavailable);
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

	/** 精确期望诊断并观察原始日志；不改变生产日志、类别捕获或全局警告策略。 */
	class FScopedExpectedMovementProof final : public FOutputDevice
	{
	public:
		FScopedExpectedMovementProof(const FNativeMovementOriginProbe& Probe, bool bHeld, const TCHAR* ReleaseReason)
			: Test(Probe.Test)
			, Label(FString::Printf(TEXT("SourceProofUnavailable Held=%d Unproven=%s"), static_cast<int32>(bHeld), ReleaseReason))
		{
			const UGGYGOPlayerInput* Producer = Probe.OriginalProducer.Get();
			const AGGYGOInputTestPawn* Pawn = Probe.Fixture.GetPawn();
			ExpectedRaw = FString::Printf(
				TEXT("[Input.MovementSource] PlayerInput=%s Controller=%s Pawn=%s Component=%s Action=%s: Physical source cannot identify a movement request; requires an observed neutral then a real press. Key=W Device=0 Mapped=1 Held=%d Axes=0/1 RequiresRelease=1 Unproven=%s NativeGap=None; Reason=SourceProofUnavailable"),
				*GetPathNameSafe(Producer), *GetPathNameSafe(Producer ? Producer->GetOuter() : nullptr),
				*GetPathNameSafe(Pawn), *GetPathNameSafe(Pawn ? Pawn->GetInputComponentForTest() : nullptr),
				*GetPathNameSafe(Probe.Fixture.GetMoveAction()), static_cast<int32>(bHeld), ReleaseReason);
			if (!Test.TestNotNull(Label + TEXT(" original logger exists"), GLog)) { return; }
			GLog->AddOutputDevice(this);
			bRegistered = true;
			Test.TestTrue(Label + TEXT(" raw observer registered"), GLog->IsRedirectingTo(this));
			Test.AddExpectedMessagePlain(TEXT("LogGGYGOMovementInput: ") + ExpectedRaw,
				ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Exact, 1);
		}

		virtual ~FScopedExpectedMovementProof() override
		{
			if (!bRegistered) { return; }
			GLog->RemoveOutputDevice(this);
			TArray<FObservedMessage> Observed;
			{
				FScopeLock Lock(&Mutex);
				Observed = MoveTemp(Messages);
			}
			if (Test.TestEqual(Label + TEXT(" scoped source Warning/Error count"), Observed.Num(), 1))
			{
				// Automation 的 Warning 期望也接受更高级别；原始观察必须另外锁定恰 Warning。
				Test.TestTrue(Label + TEXT(" exact raw category, Warning severity and complete literal"),
					Observed[0].Category == FName(TEXT("LogGGYGOMovementInput"))
					&& Observed[0].Verbosity == ELogVerbosity::Warning
					&& Observed[0].Text.Equals(ExpectedRaw, ESearchCase::CaseSensitive));
			}
		}

		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category == FName(TEXT("LogGGYGOMovementInput"))
				&& (Verbosity == ELogVerbosity::Warning || Verbosity == ELogVerbosity::Error))
			{
				FScopeLock Lock(&Mutex);
				Messages.Add({FString(Text), Verbosity, Category});
			}
		}

	private:
		struct FObservedMessage { FString Text; ELogVerbosity::Type Verbosity; FName Category; };
		FAutomationTestBase& Test;
		FString Label;
		FString ExpectedRaw;
		FCriticalSection Mutex;
		TArray<FObservedMessage> Messages;
		bool bRegistered = false;
	};

	/** 在原真实 Source 上验证会话前 Held 及旧 End 的隔离，不借用 Hero 的私有令牌。 */
	bool RunNativeMovementSuccessorAssertions(FNativeMovementOriginProbe& Probe)
	{
		UGGYGOPlayerInput* Source = Probe.OriginalProducer.Get();
		AGGYGOInputTestPawn* Pawn = Probe.Fixture.GetPawn();
		if (!Probe.Test.TestTrue(TEXT("successor check retains the original live route"),
			IsValid(Source) && IsValid(Pawn) && Probe.Fixture.GetPlayerInput() == Source)) { return false; }
		Source->EndMovementInputSession(Probe.Session, TEXT("NativeBirthSuccessorCheck"));
		if (!Probe.Test.TestEqual(TEXT("original digital sequence has exactly its terminal fact before successor"), Probe.Facts.Num(), 6)) { return false; }
		FGGYGOMovementInputSessionIdentity Successor;
		TArray<FGGYGOMovementInputFact> Facts;
		ON_SCOPE_EXIT
		{
			if (Successor.SessionSerial != 0) { Source->EndMovementInputSession(Successor, TEXT("NativeBirthSuccessorCleanup")); }
			Source->InputKey(FInputKeyEventArgs(nullptr, FInputDeviceId::CreateFromInternalId(0), EKeys::W,
				IE_Released, 0.0f, false, FPlatformTime::Cycles64()));
		};
		FString Error;
		if (!Probe.Test.TestTrue(TEXT("same original Producer issues a new real successor Session"),
			Source->BeginMovementInputSession(Pawn, Pawn->GetInputComponentForTest(), Probe.Fixture.GetMoveAction(), Successor, Error)))
		{
			Probe.Test.AddError(Error);
			return false;
		}
		FGGYGOMovementInputConsumerBindingId Binding;
		Binding.Consumer = Pawn->GetInputComponentForTest();
		Binding.ConsumerBindingSerial = 2;
		Binding.SourceSession = Successor;
		FGGYGOMovementInputRequestIdentity Request;
		{
			FScopedExpectedMovementProof ExpectedProof(Probe, true, TEXT("NativeBirthSuccessorCheck"));
			if (!Probe.Test.TestTrue(TEXT("successor receiver attaches only to its own returned Source Session"),
				Source->AttachMovementInputReceiver(Successor, Binding,
					FGGYGOMovementInputFactDelegate::CreateLambda([&Facts, &Probe, Binding](const FGGYGOMovementInputConsumerBindingId& OriginalBinding,
						const FGGYGOMovementInputFact& Fact)
					{
						Probe.Test.TestTrue(TEXT("all successor facts preserve their real returned Session and consumer binding"),
							OriginalBinding == Binding && Fact.Request.Session == Binding.SourceSession && Fact.EventSerial != 0);
						Facts.Add(Fact);
					}), Error))) { return false; }
			if (!Probe.Test.TestTrue(TEXT("held before successor cannot start or inherit the old request"),
				Facts.Num() == 2 && Facts[0].Kind == EGGYGOMovementInputFactKind::SessionOpened
				&& Facts[0].SessionMode == EGGYGOMovementInputSessionMode::Rearm
				&& Facts[1].Kind == EGGYGOMovementInputFactKind::SourceUnresolved && Facts[1].Request.RequestSerial == 0
				&& Source->QueryMovementInputRequest(Successor, Request, Error) == EGGYGOMovementInputRequestQueryResult::AwaitingPhysicalProof)) { return false; }
		}
		Source->EndMovementInputSession(Probe.Session, TEXT("StaleEndBeforeSuccessorRelease"));
		if (!Probe.Test.TestTrue(TEXT("End(old) preserves the successor awaiting real release"),
			Facts.Num() == 2 && Source->QueryMovementInputRequest(Successor, Request, Error)
				== EGGYGOMovementInputRequestQueryResult::AwaitingPhysicalProof)) { return false; }
		const FInputDeviceId Device = FInputDeviceId::CreateFromInternalId(0);
		Source->InputKey(FInputKeyEventArgs(nullptr, Device, EKeys::W, IE_Released, 0.0f, false, FPlatformTime::Cycles64()));
		if (!Probe.Test.TestTrue(TEXT("real successor release confirms neutral without a Started"),
			Facts.Num() == 3 && Facts[2].Kind == EGGYGOMovementInputFactKind::NeutralConfirmed
			&& Source->QueryMovementInputRequest(Successor, Request, Error) == EGGYGOMovementInputRequestQueryResult::NotHeld)) { return false; }
		Source->InputKey(FInputKeyEventArgs(nullptr, Device, EKeys::W, IE_Pressed, 1.0f, false, FPlatformTime::Cycles64()));
		if (!Probe.Test.TestTrue(TEXT("release then new press creates only the successor's qualified request"),
			Facts.Num() == 4 && Facts[3].Kind == EGGYGOMovementInputFactKind::RequestStarted
			&& Facts[3].StartProof == EGGYGOMovementInputStartProof::ReleasedThenPhysicalPress
			&& Source->QueryMovementInputRequest(Successor, Request, Error) == EGGYGOMovementInputRequestQueryResult::Held
			&& Request == Facts[3].Request && Request.Session == Successor && Request.RequestSerial != 0)) { return false; }
		const FGGYGOMovementInputRequestIdentity OriginalRequest = Request;
		Source->EndMovementInputSession(Probe.Session, TEXT("StaleEndWhileSuccessorHeld"));
		if (!Probe.Test.TestTrue(TEXT("End(old) cannot clear a live successor request or publish another terminal fact"),
			Facts.Num() == 4 && Source->QueryMovementInputRequest(Successor, Request, Error) == EGGYGOMovementInputRequestQueryResult::Held
			&& Request == OriginalRequest)) { return false; }
		Source->EndMovementInputSession(Successor, TEXT("NativeBirthSuccessorCompleted"));
		return Probe.Test.TestTrue(TEXT("only the successor's own End invalidates its exact request once"),
			Facts.Num() == 5 && Facts[4].Kind == EGGYGOMovementInputFactKind::SessionInvalidated
			&& Facts[4].Request == OriginalRequest
			&& Source->QueryMovementInputRequest(Successor, Request, Error) == EGGYGOMovementInputRequestQueryResult::Unavailable);
	}

	class FWaitForNativeMovementOriginRebuild : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForNativeMovementOriginRebuild(TSharedRef<FNativeMovementOriginProbe> InProbe)
			: Probe(MoveTemp(InProbe)), Deadline(FPlatformTime::Seconds() + 10.0)
		{}

		virtual bool Update() override
		{
			if (Stage != EStage::OriginalSource) { return UpdateHeroRebuild(); }
			if (!Probe->CheckCold(TEXT("waiting for actual first rebuild notification")))
			{
				return Finish();
			}
			AGGYGOInputTestPawn* Pawn = Probe->Fixture.GetPawn();
			UGGYGOInputTestHeroComponent* Observer = IsValid(Pawn) ? Pawn->GetHeroForTest() : nullptr;
			if (!IsValid(Observer) || !Observer->IsObservingNativeMappingRebuildForTest())
			{
				Probe->Test.AddError(TEXT("[Input.NativeBirthTest] Original native rebuild observer/route lost; no successor is adopted."));
				return Finish();
			}
			if (Observer->GetNativeMappingRebuildCountForTest() == 0)
			{
				if (FPlatformTime::Seconds() < Deadline) { return false; }
				Probe->Test.AddError(TEXT("[Input.NativeBirthTest] 10-second timeout awaiting the original engine mapping rebuild notification; no real rebuild frame arrived. No immediate rebuild, manual Tick/Broadcast or qualification substitution is permitted."));
				return Finish();
			}
			if (Observer->GetFirstNativeMappingRebuildTimeForTest() > Deadline)
			{
				Probe->Test.AddError(TEXT("[Input.NativeBirthTest] Original first rebuild notification arrived after the 10-second deadline; late readiness cannot turn the timeout into success."));
				return Finish();
			}
			// 先验证已编译的共享 IMC；不能借后续新 IMC 的 Add／Remove 请求让缺失的安装请求假通过。
			if (!Probe->RunDigitalAssertions() || !RunNativeMovementSuccessorAssertions(*Probe)
				|| !BeginHeroInstallation(true))
			{
				return Finish();
			}
			Stage = EStage::SharedHeroMapping;
			return false;
		}

	private:
		bool Finish()
		{
			// 先归还日志观察，再清理夹具；后续清理不属于这项负例。
			ExpectedFreshProof.Reset();
			Probe->Cleanup();
			return true;
		}

		enum class EStage : uint8 { OriginalSource, FirstHeroMapping, SharedHeroMapping };

		bool BeginHeroInstallation(bool bUseAlreadyAppliedMapping)
		{
			AGGYGOInputTestPawn* Pawn = Probe->Fixture.GetPawn();
			UEnhancedInputLocalPlayerSubsystem* Subsystem = Probe->Fixture.GetInputSubsystem();
			UGGYGOCharacterMovementComponent* Movement = Pawn ? Cast<UGGYGOCharacterMovementComponent>(Pawn->GetCharacterMovement()) : nullptr;
			if (!Probe->Test.TestTrue(TEXT("Hero installation retains the native fixture's original input and CMC"),
				IsValid(Pawn) && IsValid(Subsystem) && IsValid(Movement)
				&& Subsystem->GetPlayerInput() == Probe->OriginalProducer.Get())) { return false; }
			if (!bUseAlreadyAppliedMapping)
			{
				FreshMapping.Reset(NewObject<UGGYGOInputTestMappingContext>(Probe->Fixture.GetGameInstance(), NAME_None, RF_Transient));
				for (const FKey& Key : { EKeys::W, EKeys::A, EKeys::S, EKeys::D })
				{
					FreshMapping->MapKey(Probe->Fixture.GetMoveAction(), Key);
				}
			}
			const UInputMappingContext* Mapping = bUseAlreadyAppliedMapping ? Probe->Fixture.GetMappingContext() : FreshMapping.Get();
			if (!Probe->Test.TestEqual(TEXT("first and shared IMC cases have their real pre-installation state"),
				Subsystem->HasMappingContext(Mapping), bUseAlreadyAppliedMapping)) { return false; }
			UGGYGOInputTestHeroComponent* Hero = Pawn->GetHeroForTest();
			// 每次安装独立观察下一次真实通知，保留实际到达时间，不能以晚到回调抹掉超时。
			Hero->StopObservingNativeMappingRebuildForTest();
			if (!Probe->Test.TestTrue(TEXT("observe the same original subsystem for this Hero installation"),
				Hero->ObserveNativeMappingRebuildForTest(Subsystem))) { return false; }
			BeforeRebuildCount = Hero->GetNativeMappingRebuildCountForTest();
			BeforeBindingSerial = Movement->GetMovementInputBindingSerial();
			Hero->ConfigureMappingForTest(Mapping, 0);
			if (!bUseAlreadyAppliedMapping)
			{
				ExpectedFreshProof = MakeUnique<FScopedExpectedMovementProof>(*Probe, false, TEXT("HeroInputReleased"));
			}
			HeroDeadline = FPlatformTime::Seconds() + 10.0;
			Hero->InitializePlayerInput(Pawn->GetInputComponentForTest());
			return Probe->Test.TestTrue(TEXT("Hero installation registers its IMC and defers compilation/binding to the real engine frame"),
				Subsystem->HasMappingContext(Mapping) && Hero->GetNativeMappingRebuildCountForTest() == BeforeRebuildCount
				&& Movement->GetMovementInputBindingSerial() == BeforeBindingSerial);
		}

		bool CheckAttachedHero(FGGYGOMovementOwnerSyncScopeId& OutScope)
		{
			UGGYGOCharacterMovementComponent* Movement = Cast<UGGYGOCharacterMovementComponent>(Probe->Fixture.GetPawn()->GetCharacterMovement());
			FString Error;
			if (!Probe->Test.TestTrue(TEXT("actual native rebuild allocates exactly one original CMC binding"),
				Movement->GetMovementInputBindingSerial() == BeforeBindingSerial + 1
				&& Movement->GetMovementOwnerSyncScope(OutScope, Error))) { return false; }
			FGGYGOQualifiedMovementIntent Intent;
			const EGGYGOQualifiedMovementIntentQueryResult Query = Movement->QueryQualifiedMovementIntent(OutScope, Intent, Error);
			return Probe->Test.TestTrue(TEXT("public CMC query confirms real Source Attach without inventing a Held grant"),
				Query == EGGYGOQualifiedMovementIntentQueryResult::AwaitingPhysicalProof || Query == EGGYGOQualifiedMovementIntentQueryResult::NotHeld);
		}

		bool UpdateHeroRebuild()
		{
			AGGYGOInputTestPawn* Pawn = Probe->Fixture.GetPawn();
			UGGYGOInputTestHeroComponent* Hero = IsValid(Pawn) ? Pawn->GetHeroForTest() : nullptr;
			if (!IsValid(Hero) || !Hero->IsObservingNativeMappingRebuildForTest())
			{
				Probe->Test.AddError(TEXT("Hero regression lost its original native rebuild observer."));
				return Finish();
			}
			const bool bExpired = FPlatformTime::Seconds() >= HeroDeadline;
			if (bExpired || Hero->GetNativeMappingRebuildCountForTest() > BeforeRebuildCount)
			{
				if (!Probe->Test.TestTrue(TEXT("each Hero installation receives one actual rebuild before its own 10-second deadline"),
					Hero->GetNativeMappingRebuildCountForTest() == BeforeRebuildCount + 1
					&& Hero->GetFirstNativeMappingRebuildTimeForTest() <= HeroDeadline))
				{
					return Finish();
				}
				FGGYGOMovementOwnerSyncScopeId CurrentScope;
				if (!CheckAttachedHero(CurrentScope)) { return Finish(); }
				ExpectedFreshProof.Reset();
				UGGYGOInputComponent* Input = Pawn->GetInputComponentForTest();
				UGGYGOCharacterMovementComponent* Movement = Cast<UGGYGOCharacterMovementComponent>(Pawn->GetCharacterMovement());
				UEnhancedInputLocalPlayerSubsystem* Subsystem = Probe->Fixture.GetInputSubsystem();
				if (Stage == EStage::SharedHeroMapping)
				{
					for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : Input->GetActionEventBindings())
					{
						if (Binding->GetAction() == Probe->Fixture.GetMoveAction() && Binding->GetTriggerEvent() == ETriggerEvent::Triggered)
						{
							OldMoveBindings.Add(Binding->Clone());
						}
					}
					if (!Probe->Test.TestEqual(TEXT("clone exactly the original Hero movement callback before replacement"), OldMoveBindings.Num(), 1))
					{ return Finish(); }
					Hero->ReleasePlayerInput();
					if (!Probe->Test.TestTrue(TEXT("shared Hero release preserves the independent registered IMC holder"),
						Subsystem->HasMappingContext(Probe->Fixture.GetMappingContext())))
					{ return Finish(); }
					if (!BeginHeroInstallation(false)) { return Finish(); }
					Stage = EStage::FirstHeroMapping;
					return false;
				}
				const uint64 CurrentBindingSerial = Movement->GetMovementInputBindingSerial();
				const FInputActionInstance Instance(Probe->Fixture.GetMoveAction());
				for (const TUniquePtr<FEnhancedInputActionEventBinding>& OldBinding : OldMoveBindings) { OldBinding->Execute(Instance); }
				Probe->OriginalProducer->EndMovementInputSession(Probe->Session, TEXT("OldEndAfterHeroSuccessorAttached"));
				FGGYGOMovementOwnerSyncScopeId AfterOldCallbacks;
				FString Error;
				if (!Probe->Test.TestTrue(TEXT("old movement callback and End(old) preserve the attached Hero successor"),
					Movement->GetMovementInputBindingSerial() == CurrentBindingSerial
					&& Movement->GetMovementOwnerSyncScope(AfterOldCallbacks, Error)
					&& AfterOldCallbacks == CurrentScope) || !CheckAttachedHero(AfterOldCallbacks))
				{ return Finish(); }
				Hero->ReleasePlayerInput();
				FGGYGOQualifiedMovementIntent Intent;
				Probe->Test.TestTrue(TEXT("current Hero release retires its binding and leaves the other IMC holder intact"),
					Movement->GetMovementInputBindingSerial() == CurrentBindingSerial + 1
					&& Movement->QueryQualifiedMovementIntent(CurrentScope, Intent, Error) == EGGYGOQualifiedMovementIntentQueryResult::Unavailable
					&& !Subsystem->HasMappingContext(FreshMapping.Get())
					&& Subsystem->HasMappingContext(Probe->Fixture.GetMappingContext()));
				return Finish();
			}
			return false;
		}

		TSharedRef<FNativeMovementOriginProbe> Probe;
		double Deadline;
		double HeroDeadline = 0.0;
		int32 BeforeRebuildCount = 0;
		uint64 BeforeBindingSerial = 0;
		EStage Stage = EStage::OriginalSource;
		TStrongObjectPtr<UGGYGOInputTestMappingContext> FreshMapping;
		TArray<TUniquePtr<FEnhancedInputActionEventBinding>> OldMoveBindings;
		TUniquePtr<FScopedExpectedMovementProof> ExpectedFreshProof;
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
					if (IsValid(Mode) && Mode->GetClass() != AGGYGOInputTestGameMode::StaticClass())
					{
						Fail(TEXT("native PIE did not use the explicit Input test GameMode configuration"));
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
				&& Info->OriginalRequestParams.GameModeOverride == AGGYGOInputTestGameMode::StaticClass();
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
			Params.GameModeOverride = AGGYGOInputTestGameMode::StaticClass();
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
