#include "Camera/Tests/GGYGOCameraLifecycleTestTypes.h"

#include "Camera/CameraPhotography.h"
#include "Camera/GGYGOCameraMode_ThirdPerson.h"
#include "Camera/GGYGOPlayerCameraManager.h"

#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/HitResult.h"
#include "Engine/NetworkDelegates.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Components/GameFrameworkComponentManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include <limits>

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCameraLifecycleTestTypes)

void UGGYGOCameraLifecycleTestModeBase::ConfigureDefaultsForTest(const FVector& Location,
	const FRotator& Rotation, float InFieldOfView,
	const FGGYGOCameraPenetrationRequest& PenetrationRequest, float InBlendTime)
{
	TestViewLocation = Location;
	TestViewRotation = Rotation;
	TestFieldOfView = InFieldOfView;
	bTestPenetrationEnabled = PenetrationRequest.bEnabled;
	TestPenetrationPivot = PenetrationRequest.PivotLocation;
	TestPenetrationProbeRadius = PenetrationRequest.ProbeRadius;
	TestPenetrationRecoverySpeed = PenetrationRequest.RecoverySpeed;
	BlendTime = InBlendTime;
	BlendFunction = EGGYGOCameraModeBlendFunction::Linear;
	BlendExponent = 1.0f;
}

void UGGYGOCameraLifecycleTestModeBase::OnActivation()
{
	Super::OnActivation();

	// Test setup changes the CDO at runtime. CameraModeStack owns and reuses instances,
	// so copy the configured values whenever this test mode is activated.
	const UGGYGOCameraLifecycleTestModeBase* Defaults =
		GetClass()->GetDefaultObject<UGGYGOCameraLifecycleTestModeBase>();
	TestViewLocation = Defaults->TestViewLocation;
	TestViewRotation = Defaults->TestViewRotation;
	TestFieldOfView = Defaults->TestFieldOfView;
	bTestPenetrationEnabled = Defaults->bTestPenetrationEnabled;
	TestPenetrationPivot = Defaults->TestPenetrationPivot;
	TestPenetrationProbeRadius = Defaults->TestPenetrationProbeRadius;
	TestPenetrationRecoverySpeed = Defaults->TestPenetrationRecoverySpeed;
	BlendTime = Defaults->BlendTime;
	BlendFunction = Defaults->BlendFunction;
	BlendExponent = Defaults->BlendExponent;
}

FGGYGOCameraEvaluationResult UGGYGOCameraLifecycleTestModeBase::UpdateView(float DeltaTime)
{
	View.Location = TestViewLocation;
	View.Rotation = TestViewRotation;
	View.ControlRotation = TestViewRotation;
	View.FieldOfView = TestFieldOfView;

	CameraPenetrationRequest.bEnabled = bTestPenetrationEnabled;
	CameraPenetrationRequest.PivotLocation = TestPenetrationPivot;
	CameraPenetrationRequest.ProbeRadius = TestPenetrationProbeRadius;
	CameraPenetrationRequest.RecoverySpeed = TestPenetrationRecoverySpeed;
	return FGGYGOCameraEvaluationResult::Success();
}

void UGGYGOCameraSteeringTestMode::ConfigureSteeringForTest()
{
	bEnableWalkRunSteeringOffset = true;
	SteeringOffsetResponse.ExternalCurve = nullptr;
	SteeringOffsetResponse.EditorCurveData.Reset();
	SteeringOffsetResponse.EditorCurveData.AddKey(0.0f, 0.0f);
	SteeringOffsetResponse.EditorCurveData.AddKey(100.0f, 1.0f);
	SteeringOffsetAmplitude = 40.0f;
	SteeringOffsetMaxDistance = 30.0f;
	WalkSteeringOffsetScale = 0.25f;
	RunSteeringOffsetScale = 1.0f;
	SteeringOffsetEnterSpeed = 6.0f;
	SteeringOffsetReturnSpeed = 4.0f;
}

FGGYGOCameraEvaluationResult UGGYGOCameraLifecycleTestComponent::PushModeForTest(TSubclassOf<UGGYGOCameraMode> ModeClass)
{
	if (!IsValid(CameraModeStack))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraModeStack"), TEXT("missing-or-invalid-stack"));
	}
	return CameraModeStack->PushCameraMode(ModeClass);
}

FMinimalViewInfo UGGYGOCameraLifecycleTestComponent::EvaluateViewForTest(float DeltaTime)
{
	FMinimalViewInfo ViewInfo;
	GetCameraView(DeltaTime, ViewInfo);
	return ViewInfo;
}

FMinimalViewInfo UGGYGOCameraLifecycleTestComponent::EvaluateViewForTest(
	float DeltaTime, const FMinimalViewInfo& OriginalView)
{
	FMinimalViewInfo ViewInfo = OriginalView;
	GetCameraView(DeltaTime, ViewInfo);
	return ViewInfo;
}

FGGYGOCameraEvaluationResult UGGYGOCameraLifecycleTestComponent::EvaluateStackForTest(float DeltaTime,
	FGGYGOCameraModeView& OutView, FGGYGOCameraPenetrationRequest& OutRequest)
{
	if (!IsValid(CameraModeStack))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraModeStack"), TEXT("missing-or-invalid-stack"));
	}
	return CameraModeStack->EvaluateStack(DeltaTime, OutView, OutRequest);
}

bool UGGYGOCameraLifecycleTestComponent::IsModeStackActiveForTest() const
{
	return CameraModeStack && CameraModeStack->IsStackActivated();
}

AGGYGOCameraLifecycleTestPawn::AGGYGOCameraLifecycleTestPawn(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TestRootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("TestRoot"));
	SetRootComponent(TestRootComponent);

	AbilitySystemComponent = CreateDefaultSubobject<UGGYGOAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	HeroComponent = CreateDefaultSubobject<UGGYGOCameraLifecycleTestHeroComponent>(TEXT("HeroComponent"));
	CameraComponent = CreateDefaultSubobject<UGGYGOCameraLifecycleTestComponent>(TEXT("CameraComponent"));
	CameraComponent->SetupAttachment(TestRootComponent);
}

UGGYGOCameraLifecycleTestAbility::UGGYGOCameraLifecycleTestAbility(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UGGYGOCameraLifecycleTestAbility::FinishForTest()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, false, false);
}

void UGGYGOCameraLifecycleTestAbility::ConfigureCameraForTest(
	TSubclassOf<UGGYGOCameraMode> ModeClass, const FGGYGOCameraOffset& Offset)
{
	AbilityCameraMode = ModeClass;
	CameraOffset = Offset;
}

// BEGIN 05-B6-T1 fixture constructors
AGGYGOCameraRealUninitializeTestPawn::AGGYGOCameraRealUninitializeTestPawn(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TestRootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("TestRoot"));
	SetRootComponent(TestRootComponent);
	PawnExtensionComponent = CreateDefaultSubobject<UGGYGOPawnExtensionComponent>(TEXT("PawnExtension"));
	HeroComponent = CreateDefaultSubobject<UGGYGOHeroComponent>(TEXT("Hero"));
	CameraComponent = CreateDefaultSubobject<UGGYGOCameraLifecycleTestComponent>(TEXT("Camera"));
	CameraComponent->SetupAttachment(TestRootComponent);
}

UGGYGOCameraSurvivingLifecycleTestAbility::UGGYGOCameraSurvivingLifecycleTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	FGameplayTagContainer AssetTags = GetAssetTags();
	AssetTags.AddTag(GGYGOGameplayTags::Ability_Behavior_SurvivesDeath);
	SetAssetTags(AssetTags);
}
// END 05-B6-T1 fixture constructors

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	bool IsFiniteLocationAndFOV(const FMinimalViewInfo& ViewInfo)
	{
		return FMath::IsFinite(ViewInfo.Location.X)
			&& FMath::IsFinite(ViewInfo.Location.Y)
			&& FMath::IsFinite(ViewInfo.Location.Z)
			&& FMath::IsFinite(ViewInfo.FOV);
	}

	struct FGGYGOCameraLifecycleTestWorld
	{
		UEngine* Engine = nullptr;
		UWorld* World = nullptr;

		explicit FGGYGOCameraLifecycleTestWorld(UEngine* InEngine)
			: Engine(InEngine)
		{
			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Init);
			if (World && Engine)
			{
				Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			}
		}

		~FGGYGOCameraLifecycleTestWorld()
		{
			if (World)
			{
				// Keep the context valid through actor/component teardown.
				World->DestroyWorld(false);
				if (Engine)
				{
					Engine->DestroyWorldContext(World);
				}
				if (UPackage* Package = World->GetPackage())
				{
					Package->SetDirtyFlag(false);
				}
			}
		}
	};

	void ConfigureTestMode(TSubclassOf<UGGYGOCameraLifecycleTestModeBase> ModeClass,
		const FVector& Location, const FRotator& Rotation, float FieldOfView,
		const FGGYGOCameraPenetrationRequest& Request, float BlendTime = 0.0f)
	{
		if (UGGYGOCameraLifecycleTestModeBase* Defaults = ModeClass ?
			Cast<UGGYGOCameraLifecycleTestModeBase>(ModeClass->GetDefaultObject()) : nullptr)
		{
			Defaults->ConfigureDefaultsForTest(Location, Rotation, FieldOfView, Request, BlendTime);
		}
	}

	AGGYGOCameraLifecycleTestPawn* SpawnCameraTestPawn(UWorld* World)
	{
		AGGYGOCameraLifecycleTestPawn* Pawn = World ? World->SpawnActor<AGGYGOCameraLifecycleTestPawn>() : nullptr;
		if (Pawn)
		{
			Pawn->GetAbilitySystemForTest()->InitAbilityActorInfo(Pawn, Pawn);
		}
		return Pawn;
	}

	FGameplayAbilitySpecHandle GrantCameraTestAbility(UGGYGOAbilitySystemComponent* ASC,
		TSubclassOf<UGGYGOGameplayAbility> AbilityClass)
	{
		return ASC ? ASC->GiveAbility(FGameplayAbilitySpec(AbilityClass, 1)) : FGameplayAbilitySpecHandle();
	}

	struct FGGYGOCameraRealUninitializeFixture
	{
		UEngine* Engine = GEngine;
		TStrongObjectPtr<UGameInstance> GameInstance;
		UWorld* World = nullptr;
		UGameFrameworkComponentManager* Manager = nullptr;
		AGGYGOCameraRealUninitializeTestHost* Host = nullptr;
		AGGYGOCameraRealUninitializeTestPawn* OldPawn = nullptr;
		AGGYGOCameraRealUninitializeTestPawn* NewPawn = nullptr;
		TWeakObjectPtr<UGGYGOCameraLifecycleTestAbility> SurvivingAbility;
		TWeakObjectPtr<UGGYGOCameraLifecycleTestAbility> SuccessorAbility;
		bool bGameInstanceInitialized = false;
		FNetDelegates::FReceivedNetworkEncryptionToken SavedEncryptionToken = FNetDelegates::OnReceivedNetworkEncryptionToken;
		FNetDelegates::FReceivedNetworkEncryptionAck SavedEncryptionAck = FNetDelegates::OnReceivedNetworkEncryptionAck;
		FNetDelegates::FReceivedNetworkEncryptionFailure SavedEncryptionFailure = FNetDelegates::OnReceivedNetworkEncryptionFailure;

		bool CheckPawnReady(FAutomationTestBase& Test, AGGYGOCameraRealUninitializeTestPawn* Pawn) const
		{
			TArray<UGGYGOPawnExtensionComponent*> Extensions;
			Pawn->GetComponents(Extensions);
			UGGYGOPawnExtensionComponent* Extension = Pawn->GetPawnExtensionForTest();
			UGGYGOHeroComponent* Hero = Pawn->GetHeroForTest();
			UGGYGOCameraLifecycleTestComponent* Camera = Pawn->GetCameraForTest();
			const FString Prefix = Pawn->GetName() + TEXT(": ");
			return Test.TestTrue(Prefix + TEXT("exactly one real PawnExtension and no Pawn-owned ASC"),
				Extensions.Num() == 1 && Extensions[0] == Extension
				&& !Pawn->FindComponentByClass<UGGYGOAbilitySystemComponent>())
				&& Test.TestTrue(Prefix + TEXT("Actor initialization and public BeginPlay reached registered components"),
					Pawn->IsActorInitialized() && Pawn->HasActorBegunPlay() && Extension->IsRegistered()
					&& Hero->IsRegistered() && Camera->IsRegistered() && Extension->HasBegunPlay()
					&& Hero->HasBegunPlay() && Camera->HasBegunPlay())
				&& Test.TestTrue(Prefix + TEXT("real manager records PawnExtension Spawned and Hero DataAvailable"),
					UGameFrameworkComponentManager::GetForActor(Pawn) == Manager
					&& Extension->GetInitState() == GGYGOGameplayTags::InitState_Spawned
					&& Hero->GetInitState() == GGYGOGameplayTags::InitState_DataAvailable)
				&& Test.TestTrue(Prefix + TEXT("Hero BeginPlay bound the actual camera mode delegate"),
					Camera->DetermineCameraModeDelegate.IsBoundToObject(Hero))
				&& Test.TestTrue(Prefix + TEXT("normal path has no Controller, LocalPlayer input, or ASC binding"),
					Pawn->HasAuthority() && !Pawn->GetController() && !Pawn->IsLocallyControlled()
					&& !Pawn->InputComponent && !Extension->GetGGYGOAbilitySystemComponent());
		}

		bool Initialize(FAutomationTestBase& Test)
		{
			if (!Test.TestNotNull(TEXT("real Engine"), Engine)) { return false; }
			GameInstance.Reset(NewObject<UGameInstance>(Engine));
			if (!Test.TestNotNull(TEXT("base GameInstance"), GameInstance.Get())) { return false; }
			// Base Init creates the actual GI subsystem collection; an empty test Init is insufficient.
			bGameInstanceInitialized = true;
			GameInstance->InitializeStandalone(FName(TEXT("GGYGOCameraRealUninitializeTestWorld")));
			World = GameInstance->GetWorld();
			if (!Test.TestNotNull(TEXT("GI-owned real World"), World)) { return false; }
			const FWorldContext* Context = Engine->GetWorldContextFromWorld(World);
			if (!Test.TestTrue(TEXT("initialized Game World and real GI WorldContext"),
				World->IsGameWorld() && World->IsInitialized() && World->GetGameInstance() == GameInstance.Get()
				&& Context && Context->World() == World && Context->OwningGameInstance == GameInstance.Get()
				&& GameInstance->GetWorldContext() == Context && GameInstance->GetLocalPlayers().IsEmpty())) { return false; }
			Manager = UGameInstance::GetSubsystem<UGameFrameworkComponentManager>(GameInstance.Get());
			if (!Test.TestNotNull(TEXT("base GI Init created the ComponentManager"), Manager)) { return false; }
			World->InitializeActorsForPlay(FURL());
			if (!Test.TestTrue(TEXT("World routes real Actor initialization"), World->AreActorsInitialized())) { return false; }
			Host = World->SpawnActor<AGGYGOCameraRealUninitializeTestHost>();
			OldPawn = World->SpawnActor<AGGYGOCameraRealUninitializeTestPawn>();
			NewPawn = World->SpawnActor<AGGYGOCameraRealUninitializeTestPawn>();
			if (!Test.TestNotNull(TEXT("external ASC host"), Host)
				|| !Test.TestNotNull(TEXT("old lifecycle Pawn"), OldPawn)
				|| !Test.TestNotNull(TEXT("new lifecycle Pawn"), NewPawn)) { return false; }
			Host->DispatchBeginPlay();
			OldPawn->DispatchBeginPlay();
			NewPawn->DispatchBeginPlay();
			UGGYGOAbilitySystemComponent* ASC = Host->GetGGYGOAbilitySystemComponent();
			return Test.TestTrue(TEXT("production host PostInitializeComponents established the sole initialized ASC Owner"),
				Host->HasAuthority() && Host->IsActorInitialized() && Host->HasActorBegunPlay()
				&& ASC && ASC->IsRegistered() && ASC->HasBeenInitialized() && ASC->HasBegunPlay()
				&& ASC->GetOwnerActor() == Host && !ASC->GetAvatarActor() && !Host->GetAvatarPawn())
				&& CheckPawnReady(Test, OldPawn) && CheckPawnReady(Test, NewPawn);
		}

		~FGGYGOCameraRealUninitializeFixture()
		{
			// Every early return keeps the real GI/manager alive through ability and Actor EndPlay.
			if (UGGYGOCameraLifecycleTestAbility* Ability = SurvivingAbility.Get(); Ability && Ability->IsActive())
			{
				Ability->FinishForTest();
			}
			if (UGGYGOCameraLifecycleTestAbility* Ability = SuccessorAbility.Get(); Ability && Ability->IsActive())
			{
				Ability->FinishForTest();
			}
			if (IsValid(Host))
			{
				Host->GetGGYGOAbilitySystemComponent()->CancelAllAbilities();
				Host->DetachAvatar();
			}
			if (World)
			{
				if (IsValid(NewPawn)) { World->DestroyActor(NewPawn); }
				if (IsValid(OldPawn)) { World->DestroyActor(OldPawn); }
				if (IsValid(Host)) { World->DestroyActor(Host); }
			}
			if (bGameInstanceInitialized && GameInstance.IsValid()) { GameInstance->Shutdown(); }
			if (World)
			{
				World->DestroyWorld(false);
				if (Engine) { Engine->DestroyWorldContext(World); }
				if (UPackage* Package = World->GetPackage()) { Package->SetDirtyFlag(false); }
			}
			// Shutdown unbinds Token/Ack and leaves Failure bound in this engine version.
			// Restore all three originals, including an originally unbound delegate.
			FNetDelegates::OnReceivedNetworkEncryptionToken = SavedEncryptionToken;
			FNetDelegates::OnReceivedNetworkEncryptionAck = SavedEncryptionAck;
			FNetDelegates::OnReceivedNetworkEncryptionFailure = SavedEncryptionFailure;
			GameInstance.Reset();
		}
	};
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCameraWalkRunSteeringCompositionTest,
	"GGYGO.Camera.WalkRunSteeringComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCameraWalkRunSteeringCompositionTest::RunTest(const FString& Parameters)
{
	FGGYGOCameraLifecycleTestWorld TestWorld(GEngine);
	AGGYGOCameraLifecycleTestPawn* Pawn = SpawnCameraTestPawn(TestWorld.World);
	if (!TestNotNull(TEXT("composition fixture Pawn"), Pawn)) { return false; }
	UGGYGOCameraModeStack* Stack = NewObject<UGGYGOCameraModeStack>(Pawn->GetCameraForTest());
	UGGYGOCameraSteeringTestMode* Mode = NewObject<UGGYGOCameraSteeringTestMode>(Stack);
	Mode->ConfigureSteeringForTest();
	if (!TestTrue(TEXT("authored inline response admitted"), Mode->ValidateConfiguration().IsSuccess())) { return false; }

	// These are geometric inputs, not injected native Movement results or production steering evidence.
	float RunTarget = 0.0f;
	TestTrue(TEXT("rear view right turn target evaluated"),
		Mode->EvaluateTargetForTest(100.0f, FVector::ForwardVector, 1.0f, FRotator::ZeroRotator, RunTarget).IsSuccess());
	TestTrue(TEXT("rear right turn shifts left, bounded by authored world cap"), FMath::IsNearlyEqual(RunTarget, -30.0f));
	float Target = 0.0f;
	TestTrue(TEXT("rear left turn target evaluated"),
		Mode->EvaluateTargetForTest(-100.0f, FVector::ForwardVector, 1.0f, FRotator::ZeroRotator, Target).IsSuccess());
	TestTrue(TEXT("rear left turn shifts right"), FMath::IsNearlyEqual(Target, 30.0f));
	TestTrue(TEXT("front view target evaluated"),
		Mode->EvaluateTargetForTest(100.0f, FVector::ForwardVector, 1.0f, FRotator(0.0f, 180.0f, 0.0f), Target).IsSuccess());
	TestTrue(TEXT("front view reverses screen sign for the same world outside"), FMath::IsNearlyEqual(Target, 30.0f));
	TestTrue(TEXT("side view target evaluated"),
		Mode->EvaluateTargetForTest(100.0f, FVector::ForwardVector, 1.0f, FRotator(0.0f, 90.0f, 0.0f), Target).IsSuccess());
	TestTrue(TEXT("side view horizontal projection vanishes"), FMath::IsNearlyZero(Target));
	// Retain the exact failing R1 scene and print its old basis; changing the assertion tolerance
	// or snapping a side-view deadzone would conceal the degree-factor precision error.
	const double OriginalMatrixSideTarget = 30.0 * FVector::DotProduct(-FVector::RightVector,
		FRotationMatrix(FRotator(0.0f, 90.0f, 0.0f)).GetUnitAxis(EAxis::Y));
	AddInfo(FString::Printf(TEXT("Exact side view: original matrix target=%.12g cm, double basis target=%.12g cm."),
		OriginalMatrixSideTarget, static_cast<double>(Target)));
	TestTrue(TEXT("view just before side-on evaluated"),
		Mode->EvaluateTargetForTest(100.0f, FVector::ForwardVector, 1.0f, FRotator(0.0, 89.99, 0.0), Target).IsSuccess());
	TestTrue(TEXT("before side-on keeps small left projection without snapping"), Target < -0.004f && Target > -0.006f);
	TestTrue(TEXT("view just after side-on evaluated"),
		Mode->EvaluateTargetForTest(100.0f, FVector::ForwardVector, 1.0f, FRotator(0.0, 90.01, 0.0), Target).IsSuccess());
	TestTrue(TEXT("after side-on keeps small right projection without snapping"), Target > 0.004f && Target < 0.006f);
	TestTrue(TEXT("pitched and rolled camera target evaluated"),
		Mode->EvaluateTargetForTest(100.0f, FVector::ForwardVector, 1.0f, FRotator(30.0, 90.0, 90.0), Target).IsSuccess());
	TestTrue(TEXT("combined pitch and roll use UE Euler right axis, giving half outside projection"), FMath::IsNearlyEqual(Target, -15.0f));
	TestTrue(TEXT("walk target evaluated"),
		Mode->EvaluateTargetForTest(100.0f, FVector::ForwardVector, 0.0f, FRotator::ZeroRotator, Target).IsSuccess());
	TestTrue(TEXT("walk composition is weaker than run"), FMath::IsNearlyEqual(Target, -10.0f) && FMath::Abs(Target) < FMath::Abs(RunTarget));

	TestTrue(TEXT("presentation enters via original camera delta"), Mode->AdvancePresentationForTest(0.1f, RunTarget).IsSuccess());
	const float Entered = Mode->GetLateralOffsetForTest();
	TestTrue(TEXT("entry moves continuously toward target"), Entered < 0.0f && Entered > RunTarget);
	TestTrue(TEXT("normal inapplicability can return toward zero"), Mode->AdvancePresentationForTest(0.1f, 0.0f).IsSuccess());
	TestTrue(TEXT("return reduces retained presentation"), FMath::Abs(Mode->GetLateralOffsetForTest()) < FMath::Abs(Entered));
	Mode->OnDeactivation();
	TestEqual(TEXT("deactivation clears mode presentation"), Mode->GetLateralOffsetForTest(), 0.0f);
	Mode->AdvancePresentationForTest(0.1f, RunTarget);
	Mode->OnActivation();
	TestEqual(TEXT("reused activation starts without retained steering"), Mode->GetLateralOffsetForTest(), 0.0f);

	Mode->ClearResponseForTest();
	Target = 123.0f;
	const FGGYGOCameraEvaluationResult MissingCurve = Mode->EvaluateTargetForTest(
		100.0f, FVector::ForwardVector, 1.0f, FRotator::ZeroRotator, Target);
	TestFalse(TEXT("enabled missing response fails explicitly"), MissingCurve.IsSuccess());
	TestEqual(TEXT("curve failure identifies configuration"), MissingCurve.Field, FName(TEXT("SteeringOffsetResponse")));
	TestEqual(TEXT("failure does not replace caller target with normal zero"), Target, 123.0f);
	Mode->SetSteeringEnabledForTest(false);
	TestTrue(TEXT("explicit disabled mode does not require unused response"), Mode->UpdateCameraMode(0.0f).IsSuccess());
	const FGGYGOCameraModeView BaselineView = Mode->GetCameraModeView();
	Mode->ConfigureSteeringForTest();
	Mode->SetAmplitudeForTest(-1.0f);
	const FGGYGOCameraEvaluationResult InvalidAmplitude = Mode->UpdateCameraMode(0.0f);
	TestFalse(TEXT("enabled illegal amplitude fails mode admission"), InvalidAmplitude.IsSuccess());
	TestEqual(TEXT("illegal amplitude names original field"), InvalidAmplitude.Field, FName(TEXT("SteeringOffsetAmplitude")));
	TestTrue(TEXT("failed mode admission does not replace view"), Mode->GetCameraModeView().Location == BaselineView.Location);
	Mode->ConfigureSteeringForTest();
	Target = 123.0f;
	const FGGYGOCameraEvaluationResult InvalidSample = Mode->EvaluateTargetForTest(
		std::numeric_limits<float>::quiet_NaN(), FVector::ForwardVector, 1.0f, FRotator::ZeroRotator, Target);
	TestFalse(TEXT("non-finite geometric input fails"), InvalidSample.IsSuccess());
	TestEqual(TEXT("invalid input does not pretend to be zero turn"), Target, 123.0f);
	const FGGYGOCameraEvaluationResult MissingMovement = Mode->UpdateCameraMode(0.1f);
	TestFalse(TEXT("enabled source failure returns through UpdateView result seam"), MissingMovement.IsSuccess());
	TestEqual(TEXT("source failure names original dependency"), MissingMovement.Field, FName(TEXT("SteeringSource")));
	TestTrue(TEXT("source failure retains mode and target diagnostic provenance"),
		MissingMovement.Mode.Get() == Mode && MissingMovement.Reason.Contains(Pawn->GetPathName()));
	TestTrue(TEXT("source failure does not publish replacement mode view"), Mode->GetCameraModeView().Location == BaselineView.Location);
	TestEqual(TEXT("source failure clears only retained steering presentation"), Mode->GetLateralOffsetForTest(), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCameraPhotographyPublicationAdmissionTest,
	"GGYGO.Camera.PhotographyPublicationAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCameraPhotographyPublicationAdmissionTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOCameraLifecycleTestWorld TestWorld(GEngine);
	UWorld* World = TestWorld.World;
	if (!TestNotNull(TEXT("existing camera test World"), World)) { return false; }
	AddInfo(FString::Printf(
		TEXT("Photography supported=%d; this leaf checks the direct virtual entry and native cache contract, not paused LevelTick dispatch or photography-provider movement."),
		FCameraPhotographyManager::IsSupported(World) ? 1 : 0));

	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	APlayerController* Controller = World->SpawnActor<APlayerController>(SpawnParams);
	AGGYGOCameraLifecycleTestPawn* Pawn = World->SpawnActor<AGGYGOCameraLifecycleTestPawn>(SpawnParams);
	SpawnParams.Owner = Controller;
	AGGYGOPlayerCameraManager* Manager = World->SpawnActor<AGGYGOPlayerCameraManager>(SpawnParams);
	if (!TestNotNull(TEXT("native Controller"), Controller)
		|| !TestNotNull(TEXT("existing camera Pawn type"), Pawn)
		|| !TestNotNull(TEXT("project camera Manager"), Manager)) { return false; }
	Manager->InitializeFor(Controller);
	APlayerCameraManager* NativeEntry = Manager;
	const bool bOriginalFullTickWhenPaused = Controller->ShouldPerformFullTickWhenPaused();
	if (!TestTrue(TEXT("native initialization waits for provider activation"),
		Manager->GetCameraPublicationState() == EGGYGOCameraPublicationState::NotActivated)
		|| !TestFalse(TEXT("initial camera seed is not confirmed"), Manager->HasConfirmedNativeCameraView())) { return false; }

	const auto CheckCache = [this, Manager](const FString& Context,
		const FMinimalViewInfo& Current, const FMinimalViewInfo& Previous, float CurrentTime, float PreviousTime)
	{
		TestTrue(Context + TEXT(" preserves current native POV"),
			FMinimalViewInfo::StaticStruct()->CompareScriptStruct(&Manager->GetCameraCacheView(), &Current, 0));
		TestTrue(Context + TEXT(" preserves previous native POV"),
			FMinimalViewInfo::StaticStruct()->CompareScriptStruct(&Manager->GetLastFrameCameraCacheView(), &Previous, 0));
		TestEqual(Context + TEXT(" preserves current cache timestamp"), Manager->GetCameraCacheTime(), CurrentTime, 0.0f);
		TestEqual(Context + TEXT(" preserves previous cache timestamp"), Manager->GetLastFrameCameraCacheTime(), PreviousTime, 0.0f);
	};
	const auto AdvanceClock = [this, World, Manager]()
	{
		// Real World time advancement, not private cache/time/state injection.
		World->Tick(LEVELTICK_TimeOnly, 0.25f);
		const float GameTime = static_cast<float>(World->GetTimeSeconds());
		return TestTrue(TEXT("native Fill has a different game time to write; unchanged timestamps cannot pass vacuously"),
			FMath::IsFinite(GameTime) && GameTime > Manager->GetCameraCacheTime());
	};
	const FString ExternalWriteDiagnostic = FString::Printf(
		TEXT("Module=[Camera] Manager=[%s] Field=[NativeCache] Reason=[external-cache-seed-is-not-a-confirmed-native-update]; write rejected."),
		*Manager->GetPathName());

	const FMinimalViewInfo Seed = Manager->GetCameraCacheView();
	const FMinimalViewInfo PreviousSeed = Manager->GetLastFrameCameraCacheView();
	const float SeedTime = Manager->GetCameraCacheTime();
	const float PreviousSeedTime = Manager->GetLastFrameCameraCacheTime();
	if (!AdvanceClock()) { return false; }
	NativeEntry->UpdateCameraPhotographyOnly();
	CheckCache(TEXT("NotActivated photography entry"), Seed, PreviousSeed, SeedTime, PreviousSeedTime);
	TestFalse(TEXT("photography entry does not confirm the initialization seed"), Manager->HasConfirmedNativeCameraView());

	// Qualified native control bypasses the new override, reproducing the old entry.
	// Even without a supported backend, native Fill must actually change the timestamp.
	AddExpectedMessagePlain(ExternalWriteDiagnostic, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Exact, 1);
	Manager->APlayerCameraManager::UpdateCameraPhotographyOnly();
	if (!TestEqual(TEXT("old native entry actually advances current cache timestamp"),
		Manager->GetCameraCacheTime(), static_cast<float>(World->GetTimeSeconds()), 0.0f)
		|| !TestTrue(TEXT("old native entry changed the original timestamp"),
			Manager->GetCameraCacheTime() != SeedTime)) { return false; }

	UGGYGOCameraLifecycleTestComponent* Camera = Pawn->GetCameraForTest();
	if (!TestNotNull(TEXT("existing registered GG camera"), Camera)
		|| !TestTrue(TEXT("fixture camera is registered before activation"), Camera->IsRegistered())
		|| !TestTrue(TEXT("fixture Pawn enables native camera component selection"), Pawn->bFindCameraComponentWhenViewTarget)
		|| !TestTrue(TEXT("fixture Manager uses native component selection"), Manager->CameraStyle == NAME_Default)
		|| !TestFalse(TEXT("synthetic fixture does not replace a Hero arbiter"), Camera->DetermineCameraModeDelegate.IsBound())) { return false; }
	// This minimal Game World registers components without initializing actors.
	// Native OnRegister therefore defers auto-activation; use the public lifecycle.
	Camera->Activate();
	TInlineComponentArray<UCameraComponent*> FixtureCameras;
	Pawn->GetComponents(FixtureCameras);
	if (!TestTrue(TEXT("exact fixture camera is the native target's only selectable active camera"),
		FixtureCameras.Num() == 1 && FixtureCameras[0] == Camera && Camera->IsActive())) { return false; }
	// Native, asset-free configured mode; no CDO changes or new test type.
	Camera->DetermineCameraModeDelegate.BindLambda([]() { return UGGYGOCameraMode_ThirdPerson::StaticClass(); });
	const FGGYGOCameraEvaluationResult Activated = Manager->ActivateCameraEvaluation(Pawn, Camera);
	if (!TestTrue(TEXT("public activation evaluates the GG source successfully"), Activated.IsSuccess())
		|| !TestTrue(TEXT("valid native update establishes a confirmed cache"), Manager->HasConfirmedNativeCameraView())
		|| !TestTrue(TEXT("valid activation enters Running"),
			Manager->GetCameraPublicationState() == EGGYGOCameraPublicationState::Running)) { return false; }

	const FMinimalViewInfo RunningView = Manager->GetCameraCacheView();
	const FMinimalViewInfo PreviousRunningView = Manager->GetLastFrameCameraCacheView();
	const float RunningTime = Manager->GetCameraCacheTime();
	if (!AdvanceClock()) { return false; }
	AddExpectedMessagePlain(ExternalWriteDiagnostic, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Exact, 1);
	NativeEntry->UpdateCameraPhotographyOnly();
	TestEqual(TEXT("Running still delegates to native photography-only Fill"),
		Manager->GetCameraCacheTime(), static_cast<float>(World->GetTimeSeconds()), 0.0f);
	TestEqual(TEXT("Running retains native previous timestamp ordering"), Manager->GetLastFrameCameraCacheTime(), RunningTime, 0.0f);
	TestTrue(TEXT("Running preserves the existing out-of-scope POV publication behavior"),
		FMinimalViewInfo::StaticStruct()->CompareScriptStruct(&Manager->GetCameraCacheView(), &RunningView, 0)
		&& FMinimalViewInfo::StaticStruct()->CompareScriptStruct(&Manager->GetLastFrameCameraCacheView(), &PreviousRunningView, 0));
	TestTrue(TEXT("Running photography entry keeps admission and confirmation"),
		Manager->GetCameraPublicationState() == EGGYGOCameraPublicationState::Running && Manager->HasConfirmedNativeCameraView());

	FViewTargetTransitionParams InvalidTransition;
	InvalidTransition.BlendTime = -1.0f;
	const FGGYGOCameraEvaluationResult ExpectedFailure = FGGYGOCameraEvaluationResult::Failure(
		nullptr, TEXT("ViewTargetTransition"), TEXT("non-finite-or-negative-transition-input"));
	const FString StopDiagnostic = FString::Printf(
		TEXT("Module=[Camera] Manager=[%s] Controller=[%s] World=[%s] Target=[%s] Component=[%s] Mode=[%s] ModeClass=[%s] Field=[%s] Reason=[%s] ConfirmedNativeView=[1]; explicit original Restart required."),
		*Manager->GetPathName(), *Controller->GetPathName(), *World->GetPathName(), *Pawn->GetPathName(), *Camera->GetPathName(),
		*ExpectedFailure.ModePath, *ExpectedFailure.ModeClassPath, *ExpectedFailure.Field.ToString(), *ExpectedFailure.Reason);
	AddExpectedMessagePlain(StopDiagnostic, ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, 1);
	Manager->SetViewTarget(Pawn, InvalidTransition);
	if (!TestTrue(TEXT("invalid public transition stops the original admitted source"),
		Manager->GetCameraPublicationState() == EGGYGOCameraPublicationState::Stopped)
		|| !TestTrue(TEXT("Stopped retains its confirmed native view"), Manager->HasConfirmedNativeCameraView())
		|| !TestTrue(TEXT("original failure remains explicit"),
			Manager->GetCameraPublicationResult().Field == ExpectedFailure.Field
			&& Manager->GetCameraPublicationResult().Reason == ExpectedFailure.Reason)) { return false; }

	const FMinimalViewInfo StoppedView = Manager->GetCameraCacheView();
	const FMinimalViewInfo PreviousStoppedView = Manager->GetLastFrameCameraCacheView();
	const float StoppedTime = Manager->GetCameraCacheTime();
	const float PreviousStoppedTime = Manager->GetLastFrameCameraCacheTime();
	if (!AdvanceClock()) { return false; }
	NativeEntry->UpdateCameraPhotographyOnly();
	CheckCache(TEXT("Stopped photography entry"), StoppedView, PreviousStoppedView, StoppedTime, PreviousStoppedTime);
	TestTrue(TEXT("photography entry cannot recover Stopped or consume its original failure"),
		Manager->GetCameraPublicationState() == EGGYGOCameraPublicationState::Stopped
		&& Manager->GetCameraPublicationResult().Field == ExpectedFailure.Field
		&& Manager->GetCameraPublicationResult().Reason == ExpectedFailure.Reason
		&& Manager->HasConfirmedNativeCameraView());
	TestEqual(TEXT("photography admission does not change the Controller paused full-tick policy"),
		Controller->ShouldPerformFullTickWhenPaused(), bOriginalFullTickWhenPaused);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCameraOffsetOwnershipTest,
	"GGYGO.Camera.OffsetOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCameraOffsetOwnershipTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOCameraLifecycleTestWorld TestWorld(GEngine);
	if (!TestNotNull(TEXT("registered physics test World"), TestWorld.World)) { return false; }

	// This synthetic leaf owns its fixed arbiter; do not overwrite a Hero binding.
	const auto BindFixedTestArbiter = [this](UGGYGOCameraLifecycleTestComponent* Camera,
		TSubclassOf<UGGYGOCameraMode> ModeClass)
	{
		if (!TestFalse(TEXT("manual fixture must not overwrite an existing camera arbiter"),
			Camera->DetermineCameraModeDelegate.IsBound()))
		{
			return;
		}
		Camera->DetermineCameraModeDelegate.BindLambda([ModeClass]() { return ModeClass; });
		TestTrue(TEXT("manual fixture binds its required fixed camera arbiter"),
			Camera->DetermineCameraModeDelegate.IsBound());
	};
	const auto CheckSucceeded = [this](const FString& Context, const FGGYGOCameraEvaluationResult& Result)
	{
		TestTrue(Context + FString::Printf(TEXT(" actual Result succeeds: Mode=[%s] Class=[%s] Field=[%s] Reason=[%s]"),
			*Result.ModePath, *Result.ModeClassPath, *Result.Field.ToString(), *Result.Reason), Result.IsSuccess());
	};
	const auto ExpectRejectedDelta = [this](UGGYGOCameraLifecycleTestComponent* Camera,
		const TCHAR* Reason, int32 Occurrences)
	{
		// Bad Delta is rejected before arbitration, so the original mode provenance is None.
		const FString Message = FString::Printf(
			TEXT("Camera evaluation rejected: Module=[Camera] Component=[%s] Target=[%s] Mode=[None] ModeClass=[None] SelectedClass=[] Field=[DeltaTime] Reason=[%s]."),
			*GetPathNameSafe(Camera), *GetPathNameSafe(Camera->GetTargetActor()), Reason);
		AddExpectedMessagePlain(Message, ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, Occurrences);
	};
	const auto EvaluateRejectedDelta = [this](UGGYGOCameraLifecycleTestComponent* Camera,
		float DeltaTime, const FMinimalViewInfo& OriginalView, const FString& Context)
	{
		const FTransform OriginalTransform = Camera->GetComponentTransform();
		const float OriginalComponentFOV = Camera->FieldOfView;
		const float OriginalAlpha = Camera->GetOffsetAlphaForTest();
		const FMinimalViewInfo View = Camera->EvaluateViewForTest(DeltaTime, OriginalView);
		TestTrue(Context + TEXT(" preserves the caller's original published View fields exactly"),
			View.Location == OriginalView.Location && View.Rotation == OriginalView.Rotation
			&& View.FOV == OriginalView.FOV && View.OrthoWidth == OriginalView.OrthoWidth
			&& View.OrthoNearClipPlane == OriginalView.OrthoNearClipPlane
			&& View.OrthoFarClipPlane == OriginalView.OrthoFarClipPlane
			&& View.AspectRatio == OriginalView.AspectRatio
			&& View.bConstrainAspectRatio == OriginalView.bConstrainAspectRatio
			&& View.bUseFieldOfViewForLOD == OriginalView.bUseFieldOfViewForLOD
			&& View.ProjectionMode == OriginalView.ProjectionMode
			&& View.PostProcessBlendWeight == OriginalView.PostProcessBlendWeight
			&& FPostProcessSettings::StaticStruct()->CompareScriptStruct(
				&View.PostProcessSettings, &OriginalView.PostProcessSettings, 0));
		TestTrue(Context + TEXT(" does not publish a component transform"),
			Camera->GetComponentTransform().Equals(OriginalTransform, 0.0));
		TestEqual(Context + TEXT(" does not publish component FOV"), Camera->FieldOfView, OriginalComponentFOV, 0.0f);
		TestEqual(Context + TEXT(" does not advance Offset alpha"), Camera->GetOffsetAlphaForTest(), OriginalAlpha, 0.0f);
		return View;
	};

	const auto CheckRejectedOffsetStep = [this](UGGYGOCameraLifecycleTestComponent* Camera,
		float DeltaTime, const TCHAR* Reason, const FString& Context)
	{
		const float OriginalAlpha = Camera->GetOffsetAlphaForTest();
		const FGGYGOCameraEvaluationResult Result = Camera->AdvanceOffsetForTest(DeltaTime);
		TestFalse(Context + TEXT(" actual Offset Result rejects the step"), Result.IsSuccess());
		TestEqual(Context + TEXT(" reports the original DeltaTime field"), Result.Field, FName(TEXT("DeltaTime")));
		TestEqual(Context + TEXT(" reports the exact rejection reason"), Result.Reason, FString(Reason));
		TestEqual(Context + TEXT(" leaves Offset alpha unchanged"), Camera->GetOffsetAlphaForTest(), OriginalAlpha, 0.0f);
	};

	FGGYGOCameraPenetrationRequest NoPenetration;
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeA::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, 80.0f, NoPenetration);
	AGGYGOCameraLifecycleTestPawn* Pawn = SpawnCameraTestPawn(TestWorld.World);
	if (!TestNotNull(TEXT("synthetic Pawn"), Pawn)) { return false; }
	UGGYGOCameraLifecycleTestComponent* Camera = Pawn->GetCameraForTest();
	BindFixedTestArbiter(Camera, UGGYGOCameraLifecycleTestModeA::StaticClass());
	CheckSucceeded(TEXT("existing push: Camera"), Camera->PushModeForTest(UGGYGOCameraLifecycleTestModeA::StaticClass()));

	const FGGYGOCameraOffsetHandle InvalidHandle;
	TestFalse(TEXT("default offset token is invalid"), InvalidHandle.IsValid());

	FGGYGOCameraOffset FirstOffset;
	FirstOffset.LocationOffset = FVector(10.0f, 0.0f, 0.0f);
	FirstOffset.FieldOfViewDelta = 2.0f;
	FirstOffset.BlendInTime = 1.0f;
	FirstOffset.BlendOutTime = 0.25f;
	const FGGYGOCameraOffsetHandle FirstHandle = Camera->SetCameraOffset(FirstOffset);
	TestTrue(TEXT("first offset returns a valid token"), FirstHandle.IsValid());
	TestEqual(TEXT("fresh Camera starts its token sequence at one"), FirstHandle.Value, static_cast<uint64>(1));

	FMinimalViewInfo FirstView = Camera->EvaluateViewForTest(0.25f);
	TestTrue(TEXT("first offset begins from the current alpha"),
		FMath::IsNearlyEqual(FirstView.Location.X, 2.5f, 0.01f));
	TestTrue(TEXT("first FOV offset is applied at the same alpha"),
		FMath::IsNearlyEqual(FirstView.FOV, 80.5f, 0.01f));

	FGGYGOCameraOffset ReplacementOffset;
	ReplacementOffset.LocationOffset = FVector(40.0f, 0.0f, 0.0f);
	ReplacementOffset.FieldOfViewDelta = 4.0f;
	ReplacementOffset.BlendInTime = 1.0f;
	ReplacementOffset.BlendOutTime = 0.25f;
	const FGGYGOCameraOffsetHandle ReplacementHandle = Camera->SetCameraOffset(ReplacementOffset);
	TestTrue(TEXT("replacement returns a valid token"), ReplacementHandle.IsValid());
	TestTrue(TEXT("replacement token strictly increases"), ReplacementHandle.Value > FirstHandle.Value);
	TestTrue(TEXT("stale token cannot clear replacement"), !Camera->ClearCameraOffset(FirstHandle));
	TestTrue(TEXT("invalid token cannot clear replacement"), !Camera->ClearCameraOffset(InvalidHandle));

	const FMinimalViewInfo ReplacementView = Camera->EvaluateViewForTest(0.0f);
	TestTrue(TEXT("replacement preserves current blend alpha"),
		FMath::IsNearlyEqual(Camera->GetOffsetAlphaForTest(), 0.25f, 0.001f));
	TestTrue(TEXT("single slot contains only the replacement offset"),
		FMath::IsNearlyEqual(ReplacementView.Location.X, 10.0f, 0.01f));
	TestTrue(TEXT("replacement FOV is not combined with old offset"),
		FMath::IsNearlyEqual(ReplacementView.FOV, 81.0f, 0.01f));
	TestTrue(TEXT("repeated stale release remains inert"), !Camera->ClearCameraOffset(FirstHandle));
	TestTrue(TEXT("current token clears exactly once"), Camera->ClearCameraOffset(ReplacementHandle));
	TestFalse(TEXT("duplicate current release is rejected"), Camera->ClearCameraOffset(ReplacementHandle));
	const FMinimalViewInfo ClearedView = Camera->EvaluateViewForTest(0.25f);
	TestTrue(TEXT("zero blend-out leaves the base view"), FMath::IsNearlyEqual(ClearedView.Location.X, 0.0f, 0.01f));

	FGGYGOCameraOffset BeforeResetOffset;
	BeforeResetOffset.LocationOffset = FVector(77.0f, 0.0f, 0.0f);
	BeforeResetOffset.BlendInTime = 0.0f;
	const FGGYGOCameraOffsetHandle BeforeResetHandle = Camera->SetCameraOffset(BeforeResetOffset);
	Camera->EvaluateViewForTest(0.0f);
	Camera->ResetCameraRuntimeState();
	TestFalse(TEXT("Reset clears the active mode stack"), Camera->IsModeStackActiveForTest());
	TestFalse(TEXT("Reset invalidates the old offset token"), Camera->ClearCameraOffset(BeforeResetHandle));
	CheckSucceeded(TEXT("existing push: Camera"), Camera->PushModeForTest(UGGYGOCameraLifecycleTestModeA::StaticClass()));
	const FMinimalViewInfo AfterResetView = Camera->EvaluateViewForTest(0.0f);
	TestTrue(TEXT("Reset clears the old offset value"), FMath::IsNearlyEqual(AfterResetView.Location.X, 0.0f, 0.01f));

	const FGGYGOCameraOffsetHandle AfterResetHandle = Camera->SetCameraOffset(BeforeResetOffset);
	TestTrue(TEXT("Reset does not reuse a prior token"), AfterResetHandle.Value > BeforeResetHandle.Value);
	TestTrue(TEXT("new token is valid after Reset"), AfterResetHandle.IsValid());

	AGGYGOCameraLifecycleTestPawn* DeltaTimePawn = SpawnCameraTestPawn(TestWorld.World);
	if (!TestNotNull(TEXT("DeltaTime safety Pawn"), DeltaTimePawn)) { return false; }
	UGGYGOCameraLifecycleTestComponent* DeltaTimeCamera = DeltaTimePawn->GetCameraForTest();
	BindFixedTestArbiter(DeltaTimeCamera, UGGYGOCameraLifecycleTestModeA::StaticClass());
	CheckSucceeded(TEXT("existing push: DeltaTimeCamera"), DeltaTimeCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeA::StaticClass()));
	FGGYGOCameraOffset NormalOffset;
	NormalOffset.LocationOffset = FVector(10.0f, 0.0f, 0.0f);
	NormalOffset.FieldOfViewDelta = 4.0f;
	NormalOffset.BlendInTime = 1.0f;
	NormalOffset.BlendOutTime = 1.0f;
	const FGGYGOCameraOffsetHandle NormalHandle = DeltaTimeCamera->SetCameraOffset(NormalOffset);
	const float QuietNaN = std::numeric_limits<float>::quiet_NaN();
	const float PositiveInfinity = std::numeric_limits<float>::infinity();
	const FMinimalViewInfo DeltaTimeEntryView = DeltaTimeCamera->EvaluateViewForTest(0.0f);
	ExpectRejectedDelta(DeltaTimeCamera, TEXT("non-finite"), 1);
	ExpectRejectedDelta(DeltaTimeCamera, TEXT("negative"), 2);
	CheckRejectedOffsetStep(DeltaTimeCamera, QuietNaN, TEXT("non-finite"), TEXT("NaN offset step"));
	CheckRejectedOffsetStep(DeltaTimeCamera, PositiveInfinity, TEXT("non-finite"), TEXT("infinite offset step"));
	CheckRejectedOffsetStep(DeltaTimeCamera, -1.0f, TEXT("negative"), TEXT("negative active offset step"));
	TestTrue(TEXT("offset alpha rejects non-finite and negative steps"),
		FMath::IsNearlyEqual(DeltaTimeCamera->GetOffsetAlphaForTest(), 0.0f, 0.001f));
	FMinimalViewInfo SafeDeltaView = EvaluateRejectedDelta(DeltaTimeCamera, QuietNaN, DeltaTimeEntryView, TEXT("NaN active Getter"));
	TestTrue(TEXT("NaN DeltaTime cannot contaminate location or FOV"),
		IsFiniteLocationAndFOV(SafeDeltaView));
	TestTrue(TEXT("NaN DeltaTime advances no offset alpha"),
		FMath::IsNearlyEqual(SafeDeltaView.Location.X, 0.0f, 0.01f)
		&& FMath::IsNearlyEqual(SafeDeltaView.FOV, 80.0f, 0.01f));
	const FMinimalViewInfo BeforeNegativeActiveView = SafeDeltaView;
	SafeDeltaView = EvaluateRejectedDelta(DeltaTimeCamera, -1.0f, BeforeNegativeActiveView, TEXT("negative active Getter"));
	TestTrue(TEXT("negative DeltaTime cannot advance or contaminate an offset"),
		IsFiniteLocationAndFOV(SafeDeltaView)
		&& FMath::IsNearlyEqual(SafeDeltaView.Location.X, 0.0f, 0.01f));
	SafeDeltaView = DeltaTimeCamera->EvaluateViewForTest(0.5f);
	TestTrue(TEXT("a normal DeltaTime still advances the request after invalid DeltaTime"),
		FMath::IsNearlyEqual(SafeDeltaView.Location.X, 5.0f, 0.01f)
		&& FMath::IsNearlyEqual(SafeDeltaView.FOV, 82.0f, 0.01f));
	TestTrue(TEXT("normal test offset can still be released"), DeltaTimeCamera->ClearCameraOffset(NormalHandle));
	CheckRejectedOffsetStep(DeltaTimeCamera, -1.0f, TEXT("negative"), TEXT("negative returning offset step"));
	TestTrue(TEXT("negative return step does not advance offset alpha"),
		FMath::IsNearlyEqual(DeltaTimeCamera->GetOffsetAlphaForTest(), 0.5f, 0.001f));
	const FMinimalViewInfo BeforeNegativeReturnView = SafeDeltaView;
	SafeDeltaView = EvaluateRejectedDelta(DeltaTimeCamera, -1.0f, BeforeNegativeReturnView, TEXT("negative return Getter"));
	TestTrue(TEXT("negative DeltaTime cannot contaminate the return blend"),
		FMath::IsNearlyEqual(SafeDeltaView.Location.X, 5.0f, 0.01f));
	SafeDeltaView = DeltaTimeCamera->EvaluateViewForTest(0.5f);
	TestTrue(TEXT("valid DeltaTime completes the return after a rejected negative step"),
		FMath::IsNearlyEqual(SafeDeltaView.Location.X, 0.0f, 0.01f));

	// BEGIN 05-B5-StrictInput-T: rejected requests preserve the entry state.
	FGGYGOCameraOffset OwnerOffset;
	OwnerOffset.LocationOffset = FVector(12.0f, -6.0f, 4.0f);
	OwnerOffset.FieldOfViewDelta = -4.0f;
	OwnerOffset.BlendInTime = 2.0f;
	OwnerOffset.BlendOutTime = 4.0f;

	struct FRejectedOffsetCase
	{
		FString Name;
		FGGYGOCameraOffset Offset;
		const TCHAR* Field;
		const TCHAR* Reason;
		bool bMixed;
	};
	TArray<FRejectedOffsetCase> InvalidCases;
	const float NonFiniteValues[] = { QuietNaN, PositiveInfinity, -PositiveInfinity };
	const TCHAR* NonFiniteNames[] = { TEXT("NaN"), TEXT("+Inf"), TEXT("-Inf") };
	const TCHAR* LocationFields[] =
		{ TEXT("LocationOffset.X"), TEXT("LocationOffset.Y"), TEXT("LocationOffset.Z") };
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		for (int32 ValueIndex = 0; ValueIndex < 3; ++ValueIndex)
		{
			FGGYGOCameraOffset Offset = OwnerOffset;
			Offset.LocationOffset[Axis] = NonFiniteValues[ValueIndex];
			InvalidCases.Add({ FString::Printf(TEXT("%s %s"), LocationFields[Axis], NonFiniteNames[ValueIndex]),
				Offset, LocationFields[Axis], TEXT("non-finite"), false });
		}
	}
	for (int32 ValueIndex = 0; ValueIndex < 3; ++ValueIndex)
	{
		FGGYGOCameraOffset Offset = OwnerOffset;
		Offset.FieldOfViewDelta = NonFiniteValues[ValueIndex];
		InvalidCases.Add({ FString::Printf(TEXT("FieldOfViewDelta %s"), NonFiniteNames[ValueIndex]),
			Offset, TEXT("FieldOfViewDelta"), TEXT("non-finite"), false });
	}
	const float InvalidTimes[] = { QuietNaN, PositiveInfinity, -PositiveInfinity, -0.25f };
	const TCHAR* InvalidTimeNames[] = { TEXT("NaN"), TEXT("+Inf"), TEXT("-Inf"), TEXT("negative") };
	const bool BlendOutFields[] = { false, true };
	for (const bool bBlendOut : BlendOutFields)
	{
		for (int32 ValueIndex = 0; ValueIndex < 4; ++ValueIndex)
		{
			FGGYGOCameraOffset Offset = OwnerOffset;
			const TCHAR* Field = bBlendOut ? TEXT("BlendOutTime") : TEXT("BlendInTime");
			if (bBlendOut)
			{
				Offset.BlendOutTime = InvalidTimes[ValueIndex];
			}
			else
			{
				Offset.BlendInTime = InvalidTimes[ValueIndex];
			}
			InvalidCases.Add({ FString::Printf(TEXT("%s %s"), Field, InvalidTimeNames[ValueIndex]),
				Offset, Field, ValueIndex == 3 ? TEXT("negative") : TEXT("non-finite"), false });
		}
	}

	// Preserve both original mixed-invalid requests and their finite View checks.
	FGGYGOCameraOffset InvalidOffset;
	InvalidOffset.LocationOffset = FVector(QuietNaN, PositiveInfinity, 0.0f);
	InvalidOffset.FieldOfViewDelta = QuietNaN;
	InvalidOffset.BlendInTime = QuietNaN;
	InvalidOffset.BlendOutTime = PositiveInfinity;
	InvalidCases.Add({ TEXT("original mixed NaN/Inf"), InvalidOffset,
		TEXT("LocationOffset.X"), TEXT("non-finite"), true });

	InvalidOffset.LocationOffset = FVector(PositiveInfinity, -PositiveInfinity, 0.0f);
	InvalidOffset.FieldOfViewDelta = PositiveInfinity;
	InvalidOffset.BlendInTime = PositiveInfinity;
	InvalidOffset.BlendOutTime = -PositiveInfinity;
	InvalidCases.Add({ TEXT("original mixed Inf"), InvalidOffset,
		TEXT("LocationOffset.X"), TEXT("non-finite"), true });

	const bool ReturningStates[] = { false, true };
	for (const bool bReturning : ReturningStates)
	{
		for (const FRejectedOffsetCase& InvalidCase : InvalidCases)
		{
			const FString Context = FString::Printf(TEXT("%s / %s"),
				bReturning ? TEXT("returning") : TEXT("active"), *InvalidCase.Name);
			// Unique object paths keep each exact expected Error at one occurrence.
			AGGYGOCameraLifecycleTestPawn* CasePawn = SpawnCameraTestPawn(TestWorld.World);
			if (!TestNotNull(Context + TEXT(" Pawn"), CasePawn)) { return false; }
			UGGYGOCameraLifecycleTestComponent* CaseCamera = CasePawn->GetCameraForTest();
			if (!TestNotNull(Context + TEXT(" Camera"), CaseCamera)) { return false; }
			BindFixedTestArbiter(CaseCamera, UGGYGOCameraLifecycleTestModeA::StaticClass());
			CheckSucceeded(Context + TEXT(" existing push"), CaseCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeA::StaticClass()));
			const FGGYGOCameraOffsetHandle OwnerHandle = CaseCamera->SetCameraOffset(OwnerOffset);
			if (!TestTrue(Context + TEXT(" owner request accepted"), OwnerHandle.IsValid())) { return false; }
			TestEqual(Context + TEXT(" fresh owner token"), OwnerHandle.Value, static_cast<uint64>(1));
			CaseCamera->EvaluateViewForTest(1.0f);
			TestEqual(Context + TEXT(" owner reaches half alpha"), CaseCamera->GetOffsetAlphaForTest(), 0.5f);
			if (bReturning)
			{
				TestTrue(Context + TEXT(" owner clears exactly once before rejection"),
					CaseCamera->ClearCameraOffset(OwnerHandle));
				TestFalse(Context + TEXT(" duplicate owner release before rejection"),
					CaseCamera->ClearCameraOffset(OwnerHandle));
				CaseCamera->EvaluateViewForTest(0.5f);
			}
			const float ExpectedAlpha = bReturning ? 0.375f : 0.5f;
			const auto CheckOwnerState = [&](const TCHAR* Stage)
			{
				const FString Label = Context + TEXT(" / ") + Stage;
				const FGGYGOCameraOffset& Actual = CaseCamera->GetOffsetForTest();
				TestTrue(Label + TEXT(" LocationOffset preserved"), Actual.LocationOffset == OwnerOffset.LocationOffset);
				TestEqual(Label + TEXT(" FOV delta preserved"), Actual.FieldOfViewDelta, OwnerOffset.FieldOfViewDelta, 0.0f);
				TestEqual(Label + TEXT(" BlendInTime preserved"), Actual.BlendInTime, OwnerOffset.BlendInTime, 0.0f);
				TestEqual(Label + TEXT(" BlendOutTime preserved"), Actual.BlendOutTime, OwnerOffset.BlendOutTime, 0.0f);
				TestEqual(Label + TEXT(" active preserved"), CaseCamera->IsOffsetActiveForTest(), !bReturning);
				TestEqual(Label + TEXT(" alpha preserved"), CaseCamera->GetOffsetAlphaForTest(), ExpectedAlpha, 0.0f);
			};
			CheckOwnerState(TEXT("before request"));
			const FMinimalViewInfo BeforeRejectView = CaseCamera->EvaluateViewForTest(0.0f);
			TestTrue(Context + TEXT(" owner View matches its partial offset"),
				IsFiniteLocationAndFOV(BeforeRejectView)
				&& BeforeRejectView.Location.Equals(OwnerOffset.LocationOffset * ExpectedAlpha, 0.001f)
				&& FMath::IsNearlyEqual(BeforeRejectView.FOV, 80.0f + OwnerOffset.FieldOfViewDelta * ExpectedAlpha, 0.001f));

			const FString ExpectedRejection = FString::Printf(
				TEXT("Camera SetCameraOffset rejected: Component=[%s] Owner=[%s] Field=[%s] Reason=[%s]."),
				*GetPathNameSafe(CaseCamera), *GetPathNameSafe(CaseCamera->GetOwner()),
				InvalidCase.Field, InvalidCase.Reason);
			AddExpectedMessagePlain(ExpectedRejection, ELogVerbosity::Error,
				EAutomationExpectedMessageFlags::Exact, 1);
			const FGGYGOCameraOffsetHandle RejectedHandle = CaseCamera->SetCameraOffset(InvalidCase.Offset);
			TestFalse(Context + TEXT(" invalid request returns Invalid"), RejectedHandle.IsValid());
			CheckOwnerState(TEXT("after rejected request"));
			TestFalse(Context + TEXT(" rejected handle cannot release owner"),
				CaseCamera->ClearCameraOffset(RejectedHandle));
			CheckOwnerState(TEXT("after rejected handle release"));

			const FMinimalViewInfo AfterRejectView = CaseCamera->EvaluateViewForTest(0.0f);
			TestTrue(Context + TEXT(" rejection keeps a finite unchanged View"),
				IsFiniteLocationAndFOV(AfterRejectView)
				&& AfterRejectView.Location.Equals(BeforeRejectView.Location, 0.001f)
				&& FMath::IsNearlyEqual(AfterRejectView.FOV, BeforeRejectView.FOV, 0.001f));
			if (InvalidCase.bMixed)
			{
				// Both original infinite calls remain; one identical consecutive diagnostic is expected.
				ExpectRejectedDelta(CaseCamera, TEXT("non-finite"), 1);
				const FMinimalViewInfo BeforePositiveInfinityView = AfterRejectView;
				SafeDeltaView = EvaluateRejectedDelta(CaseCamera, PositiveInfinity, BeforePositiveInfinityView,
					Context + TEXT(" positive infinite Getter"));
				TestTrue(Context + TEXT(" non-finite offset fields produce a finite final View"),
					IsFiniteLocationAndFOV(SafeDeltaView)
					&& SafeDeltaView.Location.Equals(BeforeRejectView.Location, 0.001f)
					&& FMath::IsNearlyEqual(SafeDeltaView.FOV, BeforeRejectView.FOV, 0.001f));
				const FMinimalViewInfo BeforeNegativeInfinityView = SafeDeltaView;
				SafeDeltaView = EvaluateRejectedDelta(CaseCamera, -PositiveInfinity, BeforeNegativeInfinityView,
					Context + TEXT(" negative infinite Getter"));
				TestTrue(Context + TEXT(" non-finite DeltaTime after invalid blend-out stays finite"),
					IsFiniteLocationAndFOV(SafeDeltaView)
					&& SafeDeltaView.Location.Equals(BeforeRejectView.Location, 0.001f)
					&& FMath::IsNearlyEqual(SafeDeltaView.FOV, BeforeRejectView.FOV, 0.001f));
				CheckOwnerState(TEXT("after mixed-invalid View pulls"));
			}

			if (bReturning)
			{
				TestFalse(Context + TEXT(" rejection cannot revive released owner token"),
					CaseCamera->ClearCameraOffset(OwnerHandle));
			}
			else
			{
				TestTrue(Context + TEXT(" owner still clears exactly once after rejection"),
					CaseCamera->ClearCameraOffset(OwnerHandle));
			}
			TestFalse(Context + TEXT(" repeated owner release stays inert"),
				CaseCamera->ClearCameraOffset(OwnerHandle));
			const FMinimalViewInfo ContinuedReturnView = CaseCamera->EvaluateViewForTest(0.5f);
			const float ContinuedAlpha = ExpectedAlpha - 0.125f;
			TestEqual(Context + TEXT(" original blend-out continues"),
				CaseCamera->GetOffsetAlphaForTest(), ContinuedAlpha);
			TestTrue(Context + TEXT(" original View continues its return"),
				IsFiniteLocationAndFOV(ContinuedReturnView)
				&& ContinuedReturnView.Location.Equals(OwnerOffset.LocationOffset * ContinuedAlpha, 0.001f)
				&& FMath::IsNearlyEqual(ContinuedReturnView.FOV,
					80.0f + OwnerOffset.FieldOfViewDelta * ContinuedAlpha, 0.001f));

			FGGYGOCameraOffset RecoveredOffset;
			RecoveredOffset.LocationOffset = FVector(7.0f, 0.0f, 0.0f);
			RecoveredOffset.FieldOfViewDelta = 1.0f;
			RecoveredOffset.BlendInTime = 0.0f;
			RecoveredOffset.BlendOutTime = 0.0f;
			const FGGYGOCameraOffsetHandle RecoveredHandle = CaseCamera->SetCameraOffset(RecoveredOffset);
			TestTrue(Context + TEXT(" a normal request remains usable after non-finite requests"),
				RecoveredHandle.IsValid());
			TestEqual(Context + TEXT(" rejection consumes no token"),
				RecoveredHandle.Value, OwnerHandle.Value + static_cast<uint64>(1));
			TestFalse(Context + TEXT(" old owner cannot clear the successor"),
				CaseCamera->ClearCameraOffset(OwnerHandle));
			SafeDeltaView = CaseCamera->EvaluateViewForTest(0.0f);
			TestTrue(Context + TEXT(" legal zero blend-in applies immediately"),
				IsFiniteLocationAndFOV(SafeDeltaView)
				&& SafeDeltaView.Location.Equals(RecoveredOffset.LocationOffset, 0.001f)
				&& FMath::IsNearlyEqual(SafeDeltaView.FOV, 81.0f, 0.001f)
				&& CaseCamera->GetOffsetAlphaForTest() == 1.0f);
			TestTrue(Context + TEXT(" legal zero blend-out owner clears"),
				CaseCamera->ClearCameraOffset(RecoveredHandle));
			TestFalse(Context + TEXT(" legal zero blend-out owner clears only once"),
				CaseCamera->ClearCameraOffset(RecoveredHandle));
			SafeDeltaView = CaseCamera->EvaluateViewForTest(0.0f);
			TestTrue(Context + TEXT(" legal zero blend-out completes immediately"),
				IsFiniteLocationAndFOV(SafeDeltaView) && SafeDeltaView.Location.IsNearlyZero()
				&& FMath::IsNearlyEqual(SafeDeltaView.FOV, 80.0f, 0.001f)
				&& CaseCamera->GetOffsetAlphaForTest() == 0.0f);

			FGGYGOCameraOffset ZeroOffset;
			ZeroOffset.BlendInTime = 0.0f;
			ZeroOffset.BlendOutTime = 0.0f;
			const FGGYGOCameraOffsetHandle ZeroHandle = CaseCamera->SetCameraOffset(ZeroOffset);
			TestTrue(Context + TEXT(" full-zero Offset is accepted"), ZeroHandle.IsValid());
			TestEqual(Context + TEXT(" full-zero request receives the next token"),
				ZeroHandle.Value, RecoveredHandle.Value + static_cast<uint64>(1));
			TestTrue(Context + TEXT(" full-zero request is active"), CaseCamera->IsOffsetActiveForTest());
			SafeDeltaView = CaseCamera->EvaluateViewForTest(0.0f);
			TestTrue(Context + TEXT(" full-zero request keeps the finite base View"),
				IsFiniteLocationAndFOV(SafeDeltaView) && SafeDeltaView.Location.IsNearlyZero()
				&& FMath::IsNearlyEqual(SafeDeltaView.FOV, 80.0f, 0.001f));
			TestFalse(Context + TEXT(" prior legal request cannot release full-zero owner"),
				CaseCamera->ClearCameraOffset(RecoveredHandle));
			TestTrue(Context + TEXT(" full-zero owner clears exactly once"),
				CaseCamera->ClearCameraOffset(ZeroHandle));
			TestFalse(Context + TEXT(" full-zero duplicate release is rejected"),
				CaseCamera->ClearCameraOffset(ZeroHandle));
		}
	}
	// END 05-B5-StrictInput-T

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCameraModeOwnershipAndAvatarReceiverTest,
	"GGYGO.Camera.ModeOwnershipAndAvatarReceiver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCameraModeOwnershipAndAvatarReceiverTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOCameraLifecycleTestWorld TestWorld(GEngine);
	if (!TestNotNull(TEXT("registered physics test World"), TestWorld.World)) { return false; }

	FGGYGOCameraPenetrationRequest NoPenetration;
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeA::StaticClass(), FVector(20.0f, 0.0f, 0.0f),
		FRotator::ZeroRotator, 80.0f, NoPenetration);
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeB::StaticClass(), FVector(20.0f, 0.0f, 0.0f),
		FRotator::ZeroRotator, 80.0f, NoPenetration);

	// Exercise Spec/generation matching on an independent Hero so the later two Heroes
	// begin with generation 1 and can detect cleanup accidentally routed to the new Avatar.
	FGGYGOCameraRealUninitializeFixture GenerationFixture;
	if (!GenerationFixture.Initialize(*this)) { return false; }
	AGGYGOCameraRealUninitializeTestPawn* GenerationPawn = GenerationFixture.OldPawn;
	if (!TestNotNull(TEXT("generation Pawn"), GenerationPawn)) { return false; }
	UGGYGOAbilitySystemComponent* GenerationASC = GenerationFixture.Host->GetGGYGOAbilitySystemComponent();
	UGGYGOPawnExtensionComponent* GenerationExtension = GenerationPawn->GetPawnExtensionForTest();
	GenerationFixture.Host->AttachAvatar(GenerationPawn);
	const FGGYGOPawnASCResourceHandle GenerationResource = GenerationExtension->GetCurrentLocalAbilitySystemResource();
	const FGGYGOPawnASCResourceIdentity GenerationIdentity = GenerationResource.GetIdentity();
	if (!TestTrue(TEXT("generation Pawn has the original ready resource from the real host"),
		GenerationResource.HasResource() && GenerationExtension->IsLocalAbilitySystemResourceReady(GenerationResource)
		&& GenerationIdentity.ASC.Get() == GenerationASC && GenerationIdentity.Pawn.Get() == GenerationPawn
		&& GenerationFixture.Host->GetAvatarPawn() == GenerationPawn
		&& GenerationASC->GetAvatarActor() == GenerationPawn)) { return false; }
	if (!TestTrue(TEXT("generation cleanup retains the original no-input-session precondition"),
		!GenerationPawn->GetController() && !GenerationPawn->IsLocallyControlled()
		&& !GenerationPawn->InputComponent && GenerationFixture.GameInstance->GetLocalPlayers().IsEmpty())) { return false; }

	int32 GenerationReadyNoticeCount = 0;
	int32 GenerationReleasedNoticeCount = 0;
	bool bGenerationNoticeResourceMatches = true;
	const FDelegateHandle GenerationNoticeHandle = GenerationExtension->RegisterLocalAbilitySystemNoticeAndCall(
		FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateLambda([&](const FGGYGOPawnASCLocalNotice& Notice)
		{
			if (Notice.Kind == EGGYGOPawnASCLocalNoticeKind::Ready)
			{
				++GenerationReadyNoticeCount;
				bGenerationNoticeResourceMatches &= Notice.Resource.HasSameResource(GenerationResource);
			}
			else if (Notice.Kind == EGGYGOPawnASCLocalNoticeKind::Released)
			{
				++GenerationReleasedNoticeCount;
				bGenerationNoticeResourceMatches &= Notice.Resource.HasSameResource(GenerationResource);
			}
		}));
	ON_SCOPE_EXIT
	{
		if (GenerationNoticeHandle.IsValid())
		{
			GenerationExtension->UnregisterLocalAbilitySystemNotice(GenerationNoticeHandle);
		}
	};
	if (!TestTrue(TEXT("typed observer replays the original ready resource exactly once"),
		GenerationNoticeHandle.IsValid() && GenerationReadyNoticeCount == 1
		&& GenerationReleasedNoticeCount == 0 && bGenerationNoticeResourceMatches)) { return false; }
	const FGameplayAbilitySpecHandle SpecA = GrantCameraTestAbility(GenerationASC,
		UGGYGOCameraLifecycleTestAbility::StaticClass());
	const FGameplayAbilitySpecHandle SpecB = GrantCameraTestAbility(GenerationASC,
		UGGYGOCameraLifecycleTestSecondaryAbility::StaticClass());
	if (!TestTrue(TEXT("two real GAS Spec handles are available"), SpecA.IsValid() && SpecB.IsValid())) { return false; }

	UGGYGOHeroComponent* GenerationHero = GenerationPawn->GetHeroForTest();
	const uint64 GenerationA1 = GenerationHero->SetAbilityCameraMode(
		UGGYGOCameraLifecycleTestModeA::StaticClass(), SpecA);
	const uint64 GenerationB = GenerationHero->SetAbilityCameraMode(
		UGGYGOCameraLifecycleTestModeB::StaticClass(), SpecB);
	const uint64 GenerationA2 = GenerationHero->SetAbilityCameraMode(
		UGGYGOCameraLifecycleTestModeA::StaticClass(), SpecA);
	TestTrue(TEXT("each valid Hero request returns a nonzero generation"),
		GenerationA1 != 0 && GenerationB != 0 && GenerationA2 != 0);
	TestTrue(TEXT("request generations increase across replacement"),
		GenerationA1 < GenerationB && GenerationB < GenerationA2);
	TestFalse(TEXT("old generation cannot clear a replacement for the same Spec"),
		GenerationHero->ClearAbilityCameraMode(SpecA, GenerationA1));
	TestFalse(TEXT("generation paired with the wrong Spec is rejected"),
		GenerationHero->ClearAbilityCameraMode(SpecB, GenerationA2));
	TestEqual(TEXT("rejected stale releases leave the latest mode in place"),
		GenerationHero->DetermineCameraMode().Get(), UGGYGOCameraLifecycleTestModeA::StaticClass());
	TestTrue(TEXT("matching Spec and generation release the latest request"),
		GenerationHero->ClearAbilityCameraMode(SpecA, GenerationA2));
	TestFalse(TEXT("duplicate generation release is rejected"),
		GenerationHero->ClearAbilityCameraMode(SpecA, GenerationA2));
	TestEqual(TEXT("releasing Spec A reveals Spec B's mode"),
		GenerationHero->DetermineCameraMode().Get(), UGGYGOCameraLifecycleTestModeB::StaticClass());
	TestTrue(TEXT("Spec B releases with its own generation"),
		GenerationHero->ClearAbilityCameraMode(SpecB, GenerationB));
	TestNull(TEXT("all exact requests released"), GenerationHero->DetermineCameraMode().Get());
	TestEqual(TEXT("invalid Spec cannot allocate a generation"),
		GenerationHero->SetAbilityCameraMode(UGGYGOCameraLifecycleTestModeA::StaticClass(),
			FGameplayAbilitySpecHandle()), static_cast<uint64>(0));

	// Real host detach publishes the original resource's typed Released notice to Hero.
	// Keep the no-input-session cleanup assertions on the production lifecycle path.
	UGGYGOCameraLifecycleTestComponent* GenerationCamera = GenerationPawn->GetCameraForTest();
	const uint64 BeforeUninitializedGeneration = GenerationHero->SetAbilityCameraMode(
		UGGYGOCameraLifecycleTestModeA::StaticClass(), SpecA);
	FGGYGOCameraOffset PreUninitializedOffset;
	PreUninitializedOffset.LocationOffset = FVector(25.0f, 0.0f, 0.0f);
	PreUninitializedOffset.BlendInTime = 0.0f;
	PreUninitializedOffset.BlendOutTime = 0.0f;
	const FGGYGOCameraOffsetHandle PreUninitializedHandle = GenerationCamera->SetCameraOffset(PreUninitializedOffset);
	GenerationCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeA::StaticClass());
	TestTrue(TEXT("pre-uninitialization Hero mode and Camera offset are active"),
		BeforeUninitializedGeneration != 0 && PreUninitializedHandle.IsValid()
		&& GenerationHero->DetermineCameraMode().Get() == UGGYGOCameraLifecycleTestModeA::StaticClass()
		&& GenerationCamera->IsModeStackActiveForTest());
	GenerationFixture.Host->DetachAvatar(GenerationPawn);
	TestTrue(TEXT("host detach withdraws the original resource and publishes typed Released exactly once"),
		GenerationReleasedNoticeCount == 1 && bGenerationNoticeResourceMatches
		&& !GenerationExtension->IsLocalAbilitySystemResourceInstalled(GenerationResource)
		&& !GenerationExtension->GetCurrentLocalAbilitySystemResource().HasResource()
		&& !GenerationFixture.Host->GetAvatarPawn() && !GenerationASC->GetAvatarActor());
	TestTrue(TEXT("released generation Pawn still has no input session"),
		!GenerationPawn->GetController() && !GenerationPawn->IsLocallyControlled()
		&& !GenerationPawn->InputComponent && GenerationFixture.GameInstance->GetLocalPlayers().IsEmpty());
	TestNull(TEXT("uninitialization clears Hero ability mode overrides"),
		GenerationHero->DetermineCameraMode().Get());
	TestFalse(TEXT("uninitialization resets the Camera mode stack"), GenerationCamera->IsModeStackActiveForTest());
	TestFalse(TEXT("uninitialization invalidates the previous Camera offset token"),
		GenerationCamera->ClearCameraOffset(PreUninitializedHandle));
	const uint64 AfterUninitializedGeneration = GenerationHero->SetAbilityCameraMode(
		UGGYGOCameraLifecycleTestModeB::StaticClass(), SpecA);
	TestTrue(TEXT("Hero generation remains strictly monotonic across uninitialization"),
		AfterUninitializedGeneration > BeforeUninitializedGeneration);
	TestFalse(TEXT("pre-uninitialization generation cannot clear a later request"),
		GenerationHero->ClearAbilityCameraMode(SpecA, BeforeUninitializedGeneration));
	TestEqual(TEXT("stale pre-uninitialization clear preserves the successor override"),
		GenerationHero->DetermineCameraMode().Get(), UGGYGOCameraLifecycleTestModeB::StaticClass());
	GenerationCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeA::StaticClass());
	const FMinimalViewInfo AfterUninitializedView = GenerationCamera->EvaluateViewForTest(0.0f);
	TestTrue(TEXT("Hero cleanup reset the old Camera offset value"),
		FMath::IsNearlyEqual(AfterUninitializedView.Location.X, 20.0f, 0.01f));

	AGGYGOCameraLifecycleTestPawn* OriginalPawn = SpawnCameraTestPawn(TestWorld.World);
	AGGYGOCameraLifecycleTestPawn* NewAvatarPawn = SpawnCameraTestPawn(TestWorld.World);
	if (!TestNotNull(TEXT("original Pawn"), OriginalPawn)
		|| !TestNotNull(TEXT("new Avatar Pawn"), NewAvatarPawn)) { return false; }
	UGGYGOCameraLifecycleTestComponent* OriginalCamera = OriginalPawn->GetCameraForTest();
	UGGYGOCameraLifecycleTestComponent* NewAvatarCamera = NewAvatarPawn->GetCameraForTest();
	OriginalCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeA::StaticClass());
	NewAvatarCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeA::StaticClass());

	UGGYGOAbilitySystemComponent* ASC = OriginalPawn->GetAbilitySystemForTest();
	const FGameplayAbilitySpecHandle AbilityHandle = GrantCameraTestAbility(ASC,
		UGGYGOCameraLifecycleTestAbility::StaticClass());
	FGameplayAbilitySpec* AbilitySpec = ASC->FindAbilitySpecFromHandle(AbilityHandle);
	UGGYGOCameraLifecycleTestAbility* Ability = AbilitySpec
		? Cast<UGGYGOCameraLifecycleTestAbility>(AbilitySpec->GetPrimaryInstance()) : nullptr;
	if (!TestTrue(TEXT("instanced camera test ability exists"), AbilityHandle.IsValid() && Ability)) { return false; }

	FGGYGOCameraOffset AbilityOffset;
	AbilityOffset.LocationOffset = FVector(40.0f, 0.0f, 0.0f);
	AbilityOffset.BlendInTime = 0.0f;
	AbilityOffset.BlendOutTime = 0.0f;
	Ability->ConfigureCameraForTest(UGGYGOCameraLifecycleTestModeB::StaticClass(), AbilityOffset);
	if (!TestTrue(TEXT("camera ability activates through GAS"), ASC->TryActivateAbility(AbilityHandle))) { return false; }
	TestTrue(TEXT("ability mode is registered on its activating Hero"),
		OriginalPawn->GetHeroForTest()->DetermineCameraMode().Get() == UGGYGOCameraLifecycleTestModeB::StaticClass());
	const FMinimalViewInfo OriginalViewDuringAbility = OriginalCamera->EvaluateViewForTest(0.0f);
	TestTrue(TEXT("ability offset is visible on the original Camera"),
		FMath::IsNearlyEqual(OriginalViewDuringAbility.Location.X, 60.0f, 0.01f));
	TestTrue(TEXT("new Avatar starts without replayed mode or offset"),
		NewAvatarPawn->GetHeroForTest()->DetermineCameraMode().Get() == nullptr
		&& FMath::IsNearlyEqual(NewAvatarCamera->EvaluateViewForTest(0.0f).Location.X, 20.0f, 0.01f));

	ASC->InitAbilityActorInfo(OriginalPawn, NewAvatarPawn);
	TestNull(TEXT("Avatar change does not replay the old request"),
		NewAvatarPawn->GetHeroForTest()->DetermineCameraMode().Get());
	TestTrue(TEXT("Avatar change does not replay the old offset"),
		FMath::IsNearlyEqual(NewAvatarCamera->EvaluateViewForTest(0.0f).Location.X, 20.0f, 0.01f));

	// Create an unrelated lease with the same local generation/token values on the new Avatar.
	// Correct GA cleanup must stay with the original receivers and leave these untouched.
	const uint64 NewAvatarGeneration = NewAvatarPawn->GetHeroForTest()->SetAbilityCameraMode(
		UGGYGOCameraLifecycleTestModeA::StaticClass(), AbilityHandle);
	FGGYGOCameraOffset NewAvatarOffset;
	NewAvatarOffset.LocationOffset = FVector(70.0f, 0.0f, 0.0f);
	NewAvatarOffset.BlendInTime = 0.0f;
	NewAvatarOffset.BlendOutTime = 0.0f;
	const FGGYGOCameraOffsetHandle NewAvatarHandle = NewAvatarCamera->SetCameraOffset(NewAvatarOffset);
	TestEqual(TEXT("fresh Hero generation collides numerically with original generation"),
		NewAvatarGeneration, static_cast<uint64>(1));
	TestEqual(TEXT("fresh Camera token collides numerically with original token"),
		NewAvatarHandle.Value, static_cast<uint64>(1));
	const FMinimalViewInfo NewAvatarViewWithOwnLease = NewAvatarCamera->EvaluateViewForTest(0.0f);
	TestTrue(TEXT("new Avatar's own offset is visible"),
		FMath::IsNearlyEqual(NewAvatarViewWithOwnLease.Location.X, 90.0f, 0.01f));

	Ability->FinishForTest();
	TestNull(TEXT("ability end clears the mode from its original Hero"),
		OriginalPawn->GetHeroForTest()->DetermineCameraMode().Get());
	TestEqual(TEXT("ability end preserves the new Hero's colliding Spec/generation lease"),
		NewAvatarPawn->GetHeroForTest()->DetermineCameraMode().Get(), UGGYGOCameraLifecycleTestModeA::StaticClass());
	TestTrue(TEXT("ability end clears the offset from its original Camera"),
		FMath::IsNearlyEqual(OriginalCamera->EvaluateViewForTest(0.0f).Location.X, 20.0f, 0.01f));
	TestTrue(TEXT("ability end preserves the new Camera's colliding token lease"),
		FMath::IsNearlyEqual(NewAvatarCamera->EvaluateViewForTest(0.0f).Location.X, 90.0f, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCameraPenetrationAggregationAndFinalResolveTest,
	"GGYGO.Camera.PenetrationAggregationAndFinalResolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCameraPenetrationAggregationAndFinalResolveTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOCameraLifecycleTestWorld TestWorld(GEngine);
	if (!TestNotNull(TEXT("registered physics test World"), TestWorld.World)) { return false; }

	// This synthetic leaf owns its fixed arbiter; do not overwrite a Hero binding.
	const auto BindFixedTestArbiter = [this](UGGYGOCameraLifecycleTestComponent* Camera,
		TSubclassOf<UGGYGOCameraMode> ModeClass)
	{
		if (!TestFalse(TEXT("manual fixture must not overwrite an existing camera arbiter"),
			Camera->DetermineCameraModeDelegate.IsBound()))
		{
			return;
		}
		Camera->DetermineCameraModeDelegate.BindLambda([ModeClass]() { return ModeClass; });
		TestTrue(TEXT("manual fixture binds its required fixed camera arbiter"),
			Camera->DetermineCameraModeDelegate.IsBound());
	};
	const auto CheckSucceeded = [this](const FString& Context, const FGGYGOCameraEvaluationResult& Result)
	{
		TestTrue(Context + FString::Printf(TEXT(" actual Result succeeds: Mode=[%s] Class=[%s] Field=[%s] Reason=[%s]"),
			*Result.ModePath, *Result.ModeClassPath, *Result.Field.ToString(), *Result.Reason), Result.IsSuccess());
	};
	const auto ExpectRejectedDelta = [this](UGGYGOCameraLifecycleTestComponent* Camera,
		const TCHAR* Reason, int32 Occurrences)
	{
		// Bad Delta is rejected before arbitration, so the original mode provenance is None.
		const FString Message = FString::Printf(
			TEXT("Camera evaluation rejected: Module=[Camera] Component=[%s] Target=[%s] Mode=[None] ModeClass=[None] SelectedClass=[] Field=[DeltaTime] Reason=[%s]."),
			*GetPathNameSafe(Camera), *GetPathNameSafe(Camera->GetTargetActor()), Reason);
		AddExpectedMessagePlain(Message, ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, Occurrences);
	};
	const auto EvaluateRejectedDelta = [this](UGGYGOCameraLifecycleTestComponent* Camera,
		float DeltaTime, const FMinimalViewInfo& OriginalView, const FString& Context)
	{
		const FTransform OriginalTransform = Camera->GetComponentTransform();
		const float OriginalComponentFOV = Camera->FieldOfView;
		const float OriginalAlpha = Camera->GetOffsetAlphaForTest();
		const FMinimalViewInfo View = Camera->EvaluateViewForTest(DeltaTime, OriginalView);
		TestTrue(Context + TEXT(" preserves the caller's original published View fields exactly"),
			View.Location == OriginalView.Location && View.Rotation == OriginalView.Rotation
			&& View.FOV == OriginalView.FOV && View.OrthoWidth == OriginalView.OrthoWidth
			&& View.OrthoNearClipPlane == OriginalView.OrthoNearClipPlane
			&& View.OrthoFarClipPlane == OriginalView.OrthoFarClipPlane
			&& View.AspectRatio == OriginalView.AspectRatio
			&& View.bConstrainAspectRatio == OriginalView.bConstrainAspectRatio
			&& View.bUseFieldOfViewForLOD == OriginalView.bUseFieldOfViewForLOD
			&& View.ProjectionMode == OriginalView.ProjectionMode
			&& View.PostProcessBlendWeight == OriginalView.PostProcessBlendWeight
			&& FPostProcessSettings::StaticStruct()->CompareScriptStruct(
				&View.PostProcessSettings, &OriginalView.PostProcessSettings, 0));
		TestTrue(Context + TEXT(" does not publish a component transform"),
			Camera->GetComponentTransform().Equals(OriginalTransform, 0.0));
		TestEqual(Context + TEXT(" does not publish component FOV"), Camera->FieldOfView, OriginalComponentFOV, 0.0f);
		TestEqual(Context + TEXT(" does not advance Offset alpha"), Camera->GetOffsetAlphaForTest(), OriginalAlpha, 0.0f);
		return View;
	};

	FGGYGOCameraPenetrationRequest RequestA;
	RequestA.bEnabled = true;
	RequestA.PivotLocation = FVector::ZeroVector;
	RequestA.ProbeRadius = 12.0f;
	RequestA.RecoverySpeed = 6.0f;
	FGGYGOCameraPenetrationRequest RequestB;
	RequestB.bEnabled = true;
	RequestB.PivotLocation = FVector(10.0f, 0.0f, 0.0f);
	RequestB.ProbeRadius = 30.0f;
	RequestB.RecoverySpeed = 2.0f;
	FGGYGOCameraPenetrationRequest InvisibleRequest;
	InvisibleRequest.bEnabled = true;
	InvisibleRequest.PivotLocation = FVector(1000.0f, 0.0f, 0.0f);
	InvisibleRequest.ProbeRadius = 100.0f;
	InvisibleRequest.RecoverySpeed = 0.0f;
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeA::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, 80.0f, RequestA);
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeB::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, 80.0f, RequestB, 1.0f);
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeC::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, 80.0f, InvisibleRequest, 1.0f);

	AGGYGOCameraLifecycleTestPawn* AggregationPawn = SpawnCameraTestPawn(TestWorld.World);
	if (!TestNotNull(TEXT("aggregation Pawn"), AggregationPawn)) { return false; }
	UGGYGOCameraLifecycleTestComponent* AggregationCamera = AggregationPawn->GetCameraForTest();
	CheckSucceeded(TEXT("existing push: AggregationCamera"), AggregationCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeA::StaticClass()));
	FGGYGOCameraModeView BlendedView;
	FGGYGOCameraPenetrationRequest BlendedRequest;
	CheckSucceeded(TEXT("existing stack: AggregationCamera"), AggregationCamera->EvaluateStackForTest(0.0f, BlendedView, BlendedRequest));
	CheckSucceeded(TEXT("existing push: AggregationCamera"), AggregationCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeB::StaticClass()));
	CheckSucceeded(TEXT("existing stack: AggregationCamera"), AggregationCamera->EvaluateStackForTest(0.5f, BlendedView, BlendedRequest));
	TestTrue(TEXT("visible participating modes enable penetration protection"), BlendedRequest.bEnabled);
	TestTrue(TEXT("Pivot is normalized by the participating visible contributions"),
		BlendedRequest.PivotLocation.Equals(FVector(5.0f, 0.0f, 0.0f), 0.01f));
	TestEqual(TEXT("ProbeRadius uses the largest participating value"), BlendedRequest.ProbeRadius, 30.0f);
	TestEqual(TEXT("recovery uses the smallest positive participating speed"), BlendedRequest.RecoverySpeed, 2.0f);

	CheckSucceeded(TEXT("existing push: AggregationCamera"), AggregationCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeC::StaticClass()));
	CheckSucceeded(TEXT("existing stack: AggregationCamera"), AggregationCamera->EvaluateStackForTest(0.0f, BlendedView, BlendedRequest));
	TestTrue(TEXT("zero-contribution request is excluded"),
		BlendedRequest.PivotLocation.Equals(FVector(5.0f, 0.0f, 0.0f), 0.01f)
		&& FMath::IsNearlyEqual(BlendedRequest.ProbeRadius, 30.0f)
		&& FMath::IsNearlyEqual(BlendedRequest.RecoverySpeed, 2.0f));

	CheckSucceeded(TEXT("existing stack: AggregationCamera"), AggregationCamera->EvaluateStackForTest(0.25f, BlendedView, BlendedRequest));
	const FVector ExpectedThreeModePivot(255.625f, 0.0f, 0.0f);
	TestTrue(TEXT("Pivot follows the actual top/middle/base stack contributions"),
		BlendedRequest.PivotLocation.Equals(ExpectedThreeModePivot, 0.02f));
	TestEqual(TEXT("visible top mode contributes to maximum radius"), BlendedRequest.ProbeRadius, 100.0f);
	TestEqual(TEXT("zero recovery does not override available positive speeds"),
		BlendedRequest.RecoverySpeed, 2.0f);

	// A fresh component receives defaults where every visible request uses immediate recovery.
	FGGYGOCameraPenetrationRequest AllZeroA = RequestA;
	AllZeroA.RecoverySpeed = 0.0f;
	FGGYGOCameraPenetrationRequest AllZeroB = RequestB;
	AllZeroB.RecoverySpeed = 0.0f;
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeA::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, 80.0f, AllZeroA);
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeB::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, 80.0f, AllZeroB, 1.0f);
	AGGYGOCameraLifecycleTestPawn* AllZeroPawn = SpawnCameraTestPawn(TestWorld.World);
	if (!TestNotNull(TEXT("all-zero recovery Pawn"), AllZeroPawn)) { return false; }
	UGGYGOCameraLifecycleTestComponent* AllZeroCamera = AllZeroPawn->GetCameraForTest();
	CheckSucceeded(TEXT("existing push: AllZeroCamera"), AllZeroCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeA::StaticClass()));
	CheckSucceeded(TEXT("existing stack: AllZeroCamera"), AllZeroCamera->EvaluateStackForTest(0.0f, BlendedView, BlendedRequest));
	CheckSucceeded(TEXT("existing push: AllZeroCamera"), AllZeroCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeB::StaticClass()));
	CheckSucceeded(TEXT("existing stack: AllZeroCamera"), AllZeroCamera->EvaluateStackForTest(0.5f, BlendedView, BlendedRequest));
	TestTrue(TEXT("all-zero participants keep penetration enabled"), BlendedRequest.bEnabled);
	TestEqual(TEXT("all-zero recovery aggregation means immediate recovery"), BlendedRequest.RecoverySpeed, 0.0f);

	FGGYGOCameraPenetrationRequest CollisionRequest;
	CollisionRequest.bEnabled = true;
	CollisionRequest.PivotLocation = FVector::ZeroVector;
	CollisionRequest.ProbeRadius = 4.0f;
	CollisionRequest.RecoverySpeed = 0.0f;
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeA::StaticClass(), FVector(20.0f, 0.0f, 0.0f),
		FRotator::ZeroRotator, 80.0f, CollisionRequest);
	AGGYGOCameraLifecycleTestPawn* CollisionPawn = SpawnCameraTestPawn(TestWorld.World);
	if (!TestNotNull(TEXT("final resolve Pawn"), CollisionPawn)) { return false; }
	UGGYGOCameraLifecycleTestComponent* CollisionCamera = CollisionPawn->GetCameraForTest();
	BindFixedTestArbiter(CollisionCamera, UGGYGOCameraLifecycleTestModeA::StaticClass());
	CheckSucceeded(TEXT("existing push: CollisionCamera"), CollisionCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeA::StaticClass()));

	AActor* Blocker = TestWorld.World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("synthetic collision blocker"), Blocker)) { return false; }
	UBoxComponent* BlockerBox = NewObject<UBoxComponent>(Blocker, TEXT("CameraChannelBlocker"));
	Blocker->SetRootComponent(BlockerBox);
	BlockerBox->SetBoxExtent(FVector(2.0f, 100.0f, 100.0f));
	BlockerBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	BlockerBox->SetCollisionObjectType(ECC_WorldStatic);
	BlockerBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	BlockerBox->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
	Blocker->SetActorLocation(FVector(65.0f, 0.0f, 0.0f));
	BlockerBox->RegisterComponent();

	FCollisionQueryParams PreflightQueryParams(
		SCENE_QUERY_STAT(GGYGOCameraLifecyclePreflight), /*bTraceComplex=*/false);
	PreflightQueryParams.AddIgnoredActor(CollisionPawn);
	FHitResult PreflightHit;
	const bool bPreflightBlocked = TestWorld.World->LineTraceSingleByChannel(
		PreflightHit, FVector::ZeroVector, FVector(100.0f, 0.0f, 0.0f), ECC_Camera, PreflightQueryParams);
	TestTrue(TEXT("PhysicsScene preflight ECC_Camera trace hits the registered blocker"),
		bPreflightBlocked && PreflightHit.GetActor() == Blocker);

	FGGYGOCameraOffset CollisionOffset;
	CollisionOffset.LocationOffset = FVector(80.0f, 0.0f, 0.0f);
	CollisionOffset.BlendInTime = 0.0f;
	CollisionOffset.BlendOutTime = 0.0f;
	const FGGYGOCameraOffsetHandle CollisionOffsetHandle = CollisionCamera->SetCameraOffset(CollisionOffset);
	TestTrue(TEXT("final resolve offset request has a valid lease"), CollisionOffsetHandle.IsValid());
	const FMinimalViewInfo ResolvedView = CollisionCamera->EvaluateViewForTest(0.0f);
	TestTrue(TEXT("Offset extends the base desired position before collision resolution"),
		ResolvedView.Location.X > 20.0f);
	TestTrue(TEXT("ECC_Camera collision clips the post-Offset position before the wall"),
		ResolvedView.Location.X < 63.0f);

	// A separate Pawn and mode instance exercise the zero-radius line-query branch
	// against the same real ECC_Camera blocker.
	FGGYGOCameraPenetrationRequest LineRequest = CollisionRequest;
	LineRequest.ProbeRadius = 0.0f;
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeC::StaticClass(), FVector(20.0f, 0.0f, 0.0f),
		FRotator::ZeroRotator, 80.0f, LineRequest);
	AGGYGOCameraLifecycleTestPawn* LineTracePawn = SpawnCameraTestPawn(TestWorld.World);
	if (!TestNotNull(TEXT("zero-radius line-trace Pawn"), LineTracePawn)) { return false; }
	UGGYGOCameraLifecycleTestComponent* LineTraceCamera = LineTracePawn->GetCameraForTest();
	BindFixedTestArbiter(LineTraceCamera, UGGYGOCameraLifecycleTestModeC::StaticClass());
	CheckSucceeded(TEXT("existing push: LineTraceCamera"), LineTraceCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeC::StaticClass()));
	FGGYGOCameraModeView LineStackView;
	FGGYGOCameraPenetrationRequest LineStackRequest;
	CheckSucceeded(TEXT("existing stack: LineTraceCamera"), LineTraceCamera->EvaluateStackForTest(0.0f, LineStackView, LineStackRequest));
	TestTrue(TEXT("fresh line-trace mode requests penetration with a zero probe radius"),
		LineStackRequest.bEnabled && FMath::IsNearlyZero(LineStackRequest.ProbeRadius));
	const FGGYGOCameraOffsetHandle LineTraceOffsetHandle = LineTraceCamera->SetCameraOffset(CollisionOffset);
	TestTrue(TEXT("line-trace Camera offset request has a valid lease"), LineTraceOffsetHandle.IsValid());
	const FMinimalViewInfo LineTraceResolvedView = LineTraceCamera->EvaluateViewForTest(0.0f);
	TestTrue(TEXT("zero-radius ECC_Camera line trace clips the post-Offset position at the wall"),
		LineTraceResolvedView.Location.X > 20.0f && LineTraceResolvedView.Location.X < 64.0f);

	BlockerBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	const FMinimalViewInfo ImmediatelyRecoveredView = CollisionCamera->EvaluateViewForTest(0.0f);
	TestTrue(TEXT("zero RecoverySpeed restores the clear final position immediately"),
		FMath::IsNearlyEqual(ImmediatelyRecoveredView.Location.X, 100.0f, 0.1f));

	// Invalid Delta rejects publication and preserves the caller's original output.
	// Reset must restore penetration recovery before the next valid view request.
	FGGYGOCameraPenetrationRequest RecoveryResetRequest = CollisionRequest;
	RecoveryResetRequest.RecoverySpeed = 2.0f;
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeB::StaticClass(), FVector(20.0f, 0.0f, 0.0f),
		FRotator::ZeroRotator, 80.0f, RecoveryResetRequest);
	AGGYGOCameraLifecycleTestPawn* RecoveryResetPawn = SpawnCameraTestPawn(TestWorld.World);
	if (!TestNotNull(TEXT("penetration reset Pawn"), RecoveryResetPawn)) { return false; }
	UGGYGOCameraLifecycleTestComponent* RecoveryResetCamera = RecoveryResetPawn->GetCameraForTest();
	BindFixedTestArbiter(RecoveryResetCamera, UGGYGOCameraLifecycleTestModeB::StaticClass());
	CheckSucceeded(TEXT("existing push: RecoveryResetCamera"), RecoveryResetCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeB::StaticClass()));
	BlockerBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	BlockerBox->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
	const FGGYGOCameraOffsetHandle BeforeRecoveryResetHandle =
		RecoveryResetCamera->SetCameraOffset(CollisionOffset);
	const FMinimalViewInfo BeforeRecoveryResetView = RecoveryResetCamera->EvaluateViewForTest(0.0f);
	TestTrue(TEXT("positive-speed request starts from the same real ECC_Camera obstruction"),
		BeforeRecoveryResetView.Location.X > 20.0f && BeforeRecoveryResetView.Location.X < 63.0f);

	BlockerBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ExpectRejectedDelta(RecoveryResetCamera, TEXT("non-finite"), 1);
	ExpectRejectedDelta(RecoveryResetCamera, TEXT("negative"), 1);
	const FMinimalViewInfo BeforeNaNRecoveryView = BeforeRecoveryResetView;
	const FMinimalViewInfo NaNRecoveryView = EvaluateRejectedDelta(RecoveryResetCamera,
		std::numeric_limits<float>::quiet_NaN(), BeforeNaNRecoveryView, TEXT("NaN recovery Getter"));
	const FMinimalViewInfo BeforeNegativeRecoveryView = NaNRecoveryView;
	const FMinimalViewInfo NegativeRecoveryView = EvaluateRejectedDelta(RecoveryResetCamera,
		-1.0f, BeforeNegativeRecoveryView, TEXT("negative recovery Getter"));
	TestTrue(TEXT("invalid and negative DeltaTime keep the recovery View finite and unchanged"),
		IsFiniteLocationAndFOV(NaNRecoveryView) && IsFiniteLocationAndFOV(NegativeRecoveryView)
		&& FMath::IsNearlyEqual(NaNRecoveryView.Location.X, BeforeRecoveryResetView.Location.X, 0.01f)
		&& FMath::IsNearlyEqual(NegativeRecoveryView.Location.X, BeforeRecoveryResetView.Location.X, 0.01f));

	RecoveryResetCamera->ResetCameraRuntimeState();
	TestFalse(TEXT("Reset invalidates the pre-reset offset lease"),
		RecoveryResetCamera->ClearCameraOffset(BeforeRecoveryResetHandle));
	CheckSucceeded(TEXT("existing push: RecoveryResetCamera"), RecoveryResetCamera->PushModeForTest(UGGYGOCameraLifecycleTestModeB::StaticClass()));
	const FGGYGOCameraOffsetHandle AfterRecoveryResetHandle =
		RecoveryResetCamera->SetCameraOffset(CollisionOffset);
	TestTrue(TEXT("recovery reset keeps offset token issuance monotonic"),
		AfterRecoveryResetHandle.Value > BeforeRecoveryResetHandle.Value);
	const FMinimalViewInfo AfterRecoveryResetView = RecoveryResetCamera->EvaluateViewForTest(0.0f);
	TestTrue(TEXT("Reset returns penetration recovery to the clear ratio"),
		FMath::IsNearlyEqual(AfterRecoveryResetView.Location.X, 100.0f, 0.1f));
	return true;
}

// BEGIN 05-B6-T1 independent fixture and leaf
namespace
{

	bool IsCameraLifecycleSpecActive(UGGYGOAbilitySystemComponent* ASC, FGameplayAbilitySpecHandle Handle)
	{
		const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(Handle);
		return Spec && Spec->IsActive();
	}

	bool IsCameraLifecycleRuntimeEmpty(const AGGYGOCameraRealUninitializeTestPawn* Pawn)
	{
		const UGGYGOCameraLifecycleTestComponent* Camera = Pawn->GetCameraForTest();
		return !Pawn->GetHeroForTest()->DetermineCameraMode() && !Camera->IsModeStackActiveForTest()
			&& !Camera->IsOffsetActiveForTest() && Camera->GetOffsetForTest().LocationOffset == FVector::ZeroVector
			&& Camera->GetOffsetForTest().FieldOfViewDelta == 0.0f && Camera->GetOffsetAlphaForTest() == 0.0f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCameraRealUninitializeAndSurvivingAbilityEndTest,
	"GGYGO.Camera.RealUninitializeAndSurvivingAbilityEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCameraRealUninitializeAndSurvivingAbilityEndTest::RunTest(const FString& Parameters)
{
	FGGYGOCameraRealUninitializeFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	UGGYGOAbilitySystemComponent* ASC = Fixture.Host->GetGGYGOAbilitySystemComponent();
	UGGYGOCameraLifecycleTestComponent* OldCamera = Fixture.OldPawn->GetCameraForTest();
	UGGYGOCameraLifecycleTestComponent* NewCamera = Fixture.NewPawn->GetCameraForTest();
	if (!TestTrue(TEXT("both fresh receivers start with no camera requests"),
		IsCameraLifecycleRuntimeEmpty(Fixture.OldPawn) && IsCameraLifecycleRuntimeEmpty(Fixture.NewPawn))) { return false; }
	Fixture.Host->AttachAvatar(Fixture.OldPawn);
	if (!TestTrue(TEXT("public Attach binds host, ASC Owner/Avatar and old PawnExtension"),
		Fixture.Host->GetAvatarPawn() == Fixture.OldPawn && ASC->GetOwnerActor() == Fixture.Host
		&& ASC->GetAvatarActor() == Fixture.OldPawn
		&& Fixture.OldPawn->GetPawnExtensionForTest()->GetGGYGOAbilitySystemComponent() == ASC)) { return false; }

	const FGameplayAbilitySpecHandle OrdinaryHandle = ASC->GiveAbility(
		FGameplayAbilitySpec(UGGYGOCameraLifecycleTestSecondaryAbility::StaticClass(), 1));
	const FGameplayAbilitySpecHandle SurvivingHandle = ASC->GiveAbility(
		FGameplayAbilitySpec(UGGYGOCameraSurvivingLifecycleTestAbility::StaticClass(), 1));
	const FGameplayAbilitySpec* OrdinarySpec = ASC->FindAbilitySpecFromHandle(OrdinaryHandle);
	const FGameplayAbilitySpec* SurvivingSpec = ASC->FindAbilitySpecFromHandle(SurvivingHandle);
	UGGYGOCameraSurvivingLifecycleTestAbility* SurvivingAbility = SurvivingSpec
		? Cast<UGGYGOCameraSurvivingLifecycleTestAbility>(SurvivingSpec->GetPrimaryInstance()) : nullptr;
	Fixture.SurvivingAbility = SurvivingAbility;
	if (!TestTrue(TEXT("real ordinary and surviving Specs/instance exist"),
		OrdinaryHandle.IsValid() && SurvivingHandle.IsValid() && OrdinarySpec && OrdinarySpec->Ability
		&& SurvivingSpec && SurvivingSpec->Ability && SurvivingAbility)
		|| !TestTrue(TEXT("GAS cancellation reads the constructor-set surviving CDO asset tag"),
			SurvivingSpec->Ability->HasAnyFlags(RF_ClassDefaultObject)
			&& SurvivingSpec->Ability->GetAssetTags().HasTagExact(GGYGOGameplayTags::Ability_Behavior_SurvivesDeath)
			&& !OrdinarySpec->Ability->GetAssetTags().HasTag(GGYGOGameplayTags::Ability_Behavior_SurvivesDeath))) { return false; }

	FGGYGOCameraPenetrationRequest NoPenetration;
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeA::StaticClass(), FVector(20.0f, 0.0f, 0.0f),
		FRotator::ZeroRotator, 80.0f, NoPenetration);
	ConfigureTestMode(UGGYGOCameraLifecycleTestModeB::StaticClass(), FVector(100.0f, 0.0f, 0.0f),
		FRotator::ZeroRotator, 90.0f, NoPenetration);
	FGGYGOCameraOffset OldOffset;
	OldOffset.LocationOffset = FVector(40.0f, 0.0f, 0.0f);
	OldOffset.FieldOfViewDelta = 7.0f;
	OldOffset.BlendInTime = 0.0f;
	OldOffset.BlendOutTime = 0.0f;
	SurvivingAbility->ConfigureCameraForTest(UGGYGOCameraLifecycleTestModeA::StaticClass(), OldOffset);
	if (!TestTrue(TEXT("ordinary control activates through GAS"), ASC->TryActivateAbility(OrdinaryHandle))
		|| !TestTrue(TEXT("surviving camera ability activates through GAS"), ASC->TryActivateAbility(SurvivingHandle))
		|| !TestTrue(TEXT("both abilities are actually active before detach"),
			IsCameraLifecycleSpecActive(ASC, OrdinaryHandle) && IsCameraLifecycleSpecActive(ASC, SurvivingHandle))) { return false; }
	const FMinimalViewInfo OldView = OldCamera->EvaluateViewForTest(0.0f);
	if (!TestTrue(TEXT("surviving GA owns a visible old Hero mode and Camera Offset before detach"),
		Fixture.OldPawn->GetHeroForTest()->DetermineCameraMode().Get() == UGGYGOCameraLifecycleTestModeA::StaticClass()
		&& OldCamera->IsModeStackActiveForTest() && OldCamera->IsOffsetActiveForTest()
		&& FMath::IsNearlyEqual(OldCamera->GetOffsetAlphaForTest(), 1.0f)
		&& OldView.Location.Equals(FVector(60.0f, 0.0f, 0.0f), 0.01f)
		&& FMath::IsNearlyEqual(OldView.FOV, 87.0f, 0.01f))) { return false; }

	// Production Detach invokes PawnExtension Uninitialize and the Hero BeginPlay subscription.
	// No direct Hero/Camera cleanup or private state injection is used for the asserted reset.
	Fixture.Host->DetachAvatar(Fixture.OldPawn);
	if (!TestTrue(TEXT("real detach clears Avatar/cache while preserving the stable ASC Owner"),
		!Fixture.Host->GetAvatarPawn() && !ASC->GetAvatarActor() && ASC->GetOwnerActor() == Fixture.Host
		&& !Fixture.OldPawn->GetPawnExtensionForTest()->GetGGYGOAbilitySystemComponent())
		|| !TestTrue(TEXT("detach cancels ordinary GA and leaves SurvivesDeath GA actually active"),
			!IsCameraLifecycleSpecActive(ASC, OrdinaryHandle) && IsCameraLifecycleSpecActive(ASC, SurvivingHandle)
			&& SurvivingAbility->IsActive())
		|| !TestTrue(TEXT("real broadcast empties the old Hero, mode stack and active Offset immediately"),
			IsCameraLifecycleRuntimeEmpty(Fixture.OldPawn))) { return false; }

	Fixture.Host->AttachAvatar(Fixture.NewPawn);
	if (!TestTrue(TEXT("public Attach binds the same persistent ASC to the new Pawn"),
		Fixture.Host->GetAvatarPawn() == Fixture.NewPawn && ASC->GetOwnerActor() == Fixture.Host
		&& ASC->GetAvatarActor() == Fixture.NewPawn
		&& Fixture.NewPawn->GetPawnExtensionForTest()->GetGGYGOAbilitySystemComponent() == ASC
		&& !Fixture.OldPawn->GetPawnExtensionForTest()->GetGGYGOAbilitySystemComponent())
		|| !TestTrue(TEXT("surviving ability stays active without replaying any request on the new receiver"),
			IsCameraLifecycleSpecActive(ASC, SurvivingHandle) && IsCameraLifecycleRuntimeEmpty(Fixture.NewPawn)
			&& IsCameraLifecycleRuntimeEmpty(Fixture.OldPawn))) { return false; }
	const FGameplayAbilitySpecHandle SuccessorHandle = ASC->GiveAbility(
		FGameplayAbilitySpec(UGGYGOCameraLifecycleTestAbility::StaticClass(), 1));
	const FGameplayAbilitySpec* SuccessorSpec = ASC->FindAbilitySpecFromHandle(SuccessorHandle);
	UGGYGOCameraLifecycleTestAbility* SuccessorAbility = SuccessorSpec
		? Cast<UGGYGOCameraLifecycleTestAbility>(SuccessorSpec->GetPrimaryInstance()) : nullptr;
	Fixture.SuccessorAbility = SuccessorAbility;
	if (!TestTrue(TEXT("distinct successor camera Spec and instance exist"),
		SuccessorHandle.IsValid() && SuccessorHandle != SurvivingHandle && SuccessorAbility)) { return false; }
	FGGYGOCameraOffset NewOffset;
	NewOffset.LocationOffset = FVector(70.0f, 0.0f, 0.0f);
	NewOffset.FieldOfViewDelta = 11.0f;
	NewOffset.BlendInTime = 0.0f;
	NewOffset.BlendOutTime = 0.0f;
	SuccessorAbility->ConfigureCameraForTest(UGGYGOCameraLifecycleTestModeB::StaticClass(), NewOffset);
	if (!TestTrue(TEXT("new Avatar's own camera GA activates through GAS"),
		ASC->TryActivateAbility(SuccessorHandle) && IsCameraLifecycleSpecActive(ASC, SuccessorHandle))) { return false; }
	const FMinimalViewInfo NewViewBeforeOldEnd = NewCamera->EvaluateViewForTest(0.0f);
	if (!TestTrue(TEXT("successor GA owns the new Hero mode and visible Camera Offset"),
		Fixture.NewPawn->GetHeroForTest()->DetermineCameraMode().Get() == UGGYGOCameraLifecycleTestModeB::StaticClass()
		&& NewCamera->IsOffsetActiveForTest() && FMath::IsNearlyEqual(NewCamera->GetOffsetAlphaForTest(), 1.0f)
		&& NewViewBeforeOldEnd.Location.Equals(FVector(170.0f, 0.0f, 0.0f), 0.01f)
		&& FMath::IsNearlyEqual(NewViewBeforeOldEnd.FOV, 101.0f, 0.01f))) { return false; }

	// Both distinct Camera components received their first Offset only through their own GA.
	// Their local token values therefore both start at 1; End must use the actual old receiver.
	SurvivingAbility->FinishForTest();
	const FMinimalViewInfo NewViewAfterOldEnd = NewCamera->EvaluateViewForTest(0.0f);
	return TestTrue(TEXT("surviving GA really ends after the Avatar change"),
		!SurvivingAbility->IsActive() && !IsCameraLifecycleSpecActive(ASC, SurvivingHandle))
		&& TestTrue(TEXT("late old BaseGA End leaves the original receiver empty"), IsCameraLifecycleRuntimeEmpty(Fixture.OldPawn))
		&& TestTrue(TEXT("late old BaseGA End preserves the successor mode, Offset and active ability"),
			IsCameraLifecycleSpecActive(ASC, SuccessorHandle) && SuccessorAbility->IsActive()
			&& Fixture.NewPawn->GetHeroForTest()->DetermineCameraMode().Get() == UGGYGOCameraLifecycleTestModeB::StaticClass()
			&& NewCamera->IsModeStackActiveForTest() && NewCamera->IsOffsetActiveForTest()
			&& NewCamera->GetOffsetForTest().LocationOffset.Equals(NewOffset.LocationOffset, 0.01f)
			&& FMath::IsNearlyEqual(NewCamera->GetOffsetForTest().FieldOfViewDelta, NewOffset.FieldOfViewDelta, 0.01f)
			&& FMath::IsNearlyEqual(NewCamera->GetOffsetAlphaForTest(), 1.0f)
			&& NewViewAfterOldEnd.Location.Equals(NewViewBeforeOldEnd.Location, 0.01f)
			&& FMath::IsNearlyEqual(NewViewAfterOldEnd.FOV, NewViewBeforeOldEnd.FOV, 0.01f)
			&& Fixture.Host->GetAvatarPawn() == Fixture.NewPawn && ASC->GetAvatarActor() == Fixture.NewPawn
			&& ASC->GetOwnerActor() == Fixture.Host);
}
// END 05-B6-T1 independent fixture and leaf
#endif
