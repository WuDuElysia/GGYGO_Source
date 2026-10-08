/**
 * @file GGYGOHeroComponent.cpp
 * @brief 管理原输入会话、Action 请求关联及相机模式仲裁的生命周期。
 */
#include "Character/Components/GGYGOHeroComponent.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Camera/GGYGOCameraComponent.h"
#include "Camera/GGYGOPlayerCameraManager.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Input/GGYGOInputComponent.h"
#include "Input/GGYGOPlayerInput.h"
#include "System/GGYGOGameplayTags.h"

#include "Components/GameFrameworkComponentManager.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHeroComponent)

class UActorComponent;

const FName UGGYGOHeroComponent::NAME_ActorFeatureName("Hero");

// Each record follows one resource lifetime; none owns physical held or ASC execution.
struct UGGYGOHeroComponent::FPlayerInputSession
{
	FPlayerInputSession(UGGYGOInputComponent* InComponent, UEnhancedInputLocalPlayerSubsystem* InSubsystem,
		UEnhancedPlayerInput* InPlayerInput, UGGYGOCharacterMovementComponent* InForceWalkMovement, uint64 InGeneration)
		: Component(InComponent), Subsystem(InSubsystem), PlayerInput(InPlayerInput),
		  ForceWalkMovement(InForceWalkMovement), Generation(InGeneration)
	{}

	struct FRegisteredMapping
	{
		TWeakObjectPtr<const UInputMappingContext> MappingContext;
		int32 RegisteredPriority = 0;
	};

	const TWeakObjectPtr<UGGYGOInputComponent> Component;
	const TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> Subsystem;
	const TWeakObjectPtr<UEnhancedPlayerInput> PlayerInput;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> ForceWalkMovement;
	const uint64 Generation;
	TArray<uint32> BindHandles;
	TArray<FRegisteredMapping> Mappings;
	TArray<TSharedPtr<FAbilityActionBinding>> ActionBindings;
	TArray<TSharedPtr<FAbilityInputObservation>> Observations;
	TSharedPtr<FAbilityInputAssociation> AbilityAssociation;
	bool bRetired = false;
};

struct UGGYGOHeroComponent::FAbilityActionBinding
{
	FAbilityActionBinding(const UInputAction* InAction, const FGameplayTag& InTag,
		const TSharedPtr<FPlayerInputSession>& InSession)
		: SourceAction(InAction), InputTag(InTag), Session(InSession), SourceActionPath(GetPathNameSafe(InAction))
	{
	}

	const TWeakObjectPtr<const UInputAction> SourceAction;
	const FGameplayTag InputTag;
	const TWeakPtr<FPlayerInputSession> Session;
	const FString SourceActionPath;
	bool bRetired = false;
	TSharedPtr<FAbilityInputObservation> Observation;
};

struct UGGYGOHeroComponent::FAbilityInputObservation
{
	FAbilityInputObservation(const TSharedPtr<FAbilityActionBinding>& InBinding, double InDeadline)
		: Binding(InBinding), Session(InBinding->Session), Request{InBinding->InputTag, {}, InDeadline}
	{
	}

	const TWeakPtr<FAbilityActionBinding> Binding;
	const TWeakPtr<FPlayerInputSession> Session;
	// First Receive's exact H association; never replaced by later Ready or Triggered.
	TWeakPtr<FAbilityInputAssociation> Association;
	// Tag/deadline are fixed at observation; Identity can only be supplied by the first Receive.
	FGGYGOAbilityInputRetryRequest Request;
	bool bActionEnded = false;
	bool bInvalidated = false;
};

struct UGGYGOHeroComponent::FLocalAbilitySystemSubscription
{
	FLocalAbilitySystemSubscription(UGGYGOPawnExtensionComponent* InExtension, APawn* InPawn)
		: Extension(InExtension), Pawn(InPawn)
	{}

	~FLocalAbilitySystemSubscription() { Retire(); }

	void AcceptReturnedHandle(FDelegateHandle ReturnedHandle)
	{
		if (bRetired)
		{
			if (ReturnedHandle.IsValid())
			{
				if (UGGYGOPawnExtensionComponent* OriginalExtension = Extension.Get())
				{
					OriginalExtension->UnregisterLocalAbilitySystemNotice(ReturnedHandle);
				}
			}
			return;
		}
		check(!NoticeHandle.IsValid());
		NoticeHandle = ReturnedHandle;
	}

	void Retire()
	{
		// Seal this original record before native removal; Hero coordinates its owned resources.
		bRetired = true;
		Resource = FGGYGOPawnASCResourceHandle{};
		AssociatedInputSession.Reset();
		const FDelegateHandle OriginalHandle = NoticeHandle;
		NoticeHandle.Reset();
		if (OriginalHandle.IsValid())
		{
			if (UGGYGOPawnExtensionComponent* OriginalExtension = Extension.Get())
			{
				OriginalExtension->UnregisterLocalAbilitySystemNotice(OriginalHandle);
			}
		}
	}

	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> Extension;
	const TWeakObjectPtr<APawn> Pawn;
	FDelegateHandle NoticeHandle{};
	FGGYGOPawnASCResourceHandle Resource{};
	TWeakPtr<FPlayerInputSession> AssociatedInputSession;
	bool bRetired = false;
};

// Exact association between an existing native input session and a consumed H resource.
struct UGGYGOHeroComponent::FAbilityInputAssociation
{
	FAbilityInputAssociation(const TSharedPtr<FLocalAbilitySystemSubscription>& InSubscription,
		const FGGYGOPawnASCResourceHandle& InResource, const TSharedPtr<FPlayerInputSession>& InSession)
		: Subscription(InSubscription), Resource(InResource), Session(InSession)
	{}

	const TWeakPtr<FLocalAbilitySystemSubscription> Subscription;
	const FGGYGOPawnASCResourceHandle Resource;
	const TWeakPtr<FPlayerInputSession> Session;
	bool bRetired = false;
};

namespace
{
	void EndOriginalAbilityInputRequests(const TArray<FGGYGOAbilityInputRequestIdentity>& Identities)
	{
		for (const FGGYGOAbilityInputRequestIdentity& Identity : Identities)
		{
			if (UGGYGOAbilitySystemComponent* OriginalASC = Identity.SourceASC.Get())
			{
				OriginalASC->EndAbilityInputRequest(Identity, EGGYGOAbilityInputRequestEndKind::Invalidated);
			}
		}
	}
}

// Resource identity and diagnostic cache only. No physical facts, request issuer or execution state.
struct FGGYGOHeroMovementInputScope
{
	FGGYGOHeroMovementInputScope(UGGYGOHeroComponent* Hero, APawn* InPawn, UGGYGOInputComponent* InComponent,
		UGGYGOPlayerInput* InSource, UEnhancedInputLocalPlayerSubsystem* InSubsystem,
		UGGYGOCharacterMovementComponent* InConsumer, const UInputAction* InAction,
		uint64 InGeneration, int32 InMappingCount, const UGGYGOInputConfig* Config)
		: Pawn(InPawn), Component(InComponent), Source(InSource), Subsystem(InSubsystem),
		  Consumer(InConsumer), Action(InAction), InputGeneration(InGeneration), ExpectedMappingRegistrationCount(InMappingCount),
		  DiagnosticContext(FString::Printf(
			TEXT("Hero='%s' Pawn='%s' Component='%s' Source='%s' Subsystem='%s' CMC='%s' InputConfig='%s' Action='%s' InputGeneration=%llu"),
			*GetPathNameSafe(Hero), *GetPathNameSafe(InPawn), *GetPathNameSafe(InComponent),
			*GetPathNameSafe(InSource), *GetPathNameSafe(InSubsystem), *GetPathNameSafe(InConsumer),
			*GetPathNameSafe(Config), *GetPathNameSafe(InAction), static_cast<unsigned long long>(InGeneration)))
	{}

	const TWeakObjectPtr<APawn> Pawn;
	const TWeakObjectPtr<UGGYGOInputComponent> Component;
	const TWeakObjectPtr<UGGYGOPlayerInput> Source;
	const TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> Subsystem;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> Consumer;
	const TWeakObjectPtr<const UInputAction> Action;
	const uint64 InputGeneration;
	const int32 ExpectedMappingRegistrationCount;
	const FString DiagnosticContext;
	TWeakObjectPtr<UGGYGOHeroMovementMappingObserver> Observer;
	FGGYGOMovementInputSessionIdentity Session;
	FGGYGOMovementInputConsumerBindingId Binding;
	FGGYGOMovementOwnerSyncObserverId OwnerSyncObserver;
	/** Read-only original CMC notice; never Held or movement admission state. */
	FGGYGOMovementOwnerSyncNotice OwnerSyncNotice;
	TSet<FName> ReportedReasons;
	bool bRetired = false;
};

namespace GGYGOHeroMovementInput
{
	static void Report(const TSharedPtr<FGGYGOHeroMovementInputScope>& Scope, FName Reason, const FString& Detail)
	{
		if (!Scope.IsValid() || Scope->ReportedReasons.Contains(Reason)) { return; }
		Scope->ReportedReasons.Add(Reason);
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Input/Hero Movement] %s Session=%llu Binding=%llu Reason='%s' Detail='%s'."),
			*Scope->DiagnosticContext, static_cast<unsigned long long>(Scope->Session.SessionSerial),
			static_cast<unsigned long long>(Scope->Binding.ConsumerBindingSerial), *Reason.ToString(), *Detail);
	}

	static void ReleaseResources(TWeakObjectPtr<UGGYGOPlayerInput> OriginalSource,
		const FGGYGOMovementInputSessionIdentity& OriginalSession,
		TWeakObjectPtr<UGGYGOCharacterMovementComponent> OriginalConsumer,
		const FGGYGOMovementInputConsumerBindingId& OriginalBinding, FName Reason)
	{
		if (UGGYGOPlayerInput* Source = OriginalSource.Get())
		{
			if (OriginalSession.SessionSerial != 0 && OriginalSession.Producer.Get() == Source)
			{
				Source->EndMovementInputSession(OriginalSession, Reason);
			}
		}
		if (UGGYGOCharacterMovementComponent* Consumer = OriginalConsumer.Get())
		{
			if (OriginalBinding.ConsumerBindingSerial != 0 && OriginalBinding.Consumer.Get() == Consumer)
			{
				FString Error;
				if (!Consumer->InvalidateMovementInputSession(OriginalBinding, Reason, Error))
				{
					// Original cleanup may have installed a successor; never substitute its binding.
					UE_LOG(LogGGYGOAbilitySystem, Verbose, TEXT("[Input/Hero Movement] Original cleanup: %s"), *Error);
				}
			}
		}
	}

	static void ReleaseOwnerSyncSubscription(const TSharedPtr<FGGYGOHeroMovementInputScope>& Scope, FName Reason)
	{
		if (!Scope.IsValid()) { return; }
		const FGGYGOMovementOwnerSyncObserverId OriginalObserver = Scope->OwnerSyncObserver;
		Scope->OwnerSyncObserver = {};
		Scope->OwnerSyncNotice = {};
		if (UGGYGOCharacterMovementComponent* Consumer = Scope->Consumer.Get())
		{
			if (OriginalObserver.IsSet())
			{
				FString Error;
				if (!Consumer->UnsubscribeMovementOwnerSync(OriginalObserver, Reason, Error))
				{
					UE_LOG(LogGGYGOAbilitySystem, Verbose, TEXT("[Input/Hero Movement] Original owner-sync cleanup: %s"), *Error);
				}
			}
		}
		// No writes after precise original unsubscribe; capture destruction may reenter.
	}

	static void ReleaseResources(const TSharedPtr<FGGYGOHeroMovementInputScope>& Scope, FName Reason)
	{
		if (!Scope.IsValid()) { return; }
		const FGGYGOMovementInputSessionIdentity OriginalSession = Scope->Session;
		const FGGYGOMovementInputConsumerBindingId OriginalBinding = Scope->Binding;
		Scope->Session = {};
		Scope->Binding = {};
		ReleaseOwnerSyncSubscription(Scope, Reason);
		ReleaseResources(Scope->Source, OriginalSession, Scope->Consumer, OriginalBinding, Reason);
		// No writes after Source/CMC cleanup; a reentrant successor retains its slots.
	}

	static void Retire(const TSharedPtr<FGGYGOHeroMovementInputScope>& Scope, FName Reason)
	{
		if (!Scope.IsValid()) { return; }
		Scope->bRetired = true;
		const TWeakObjectPtr<UGGYGOHeroMovementMappingObserver> OriginalObserver = Scope->Observer;
		Scope->Observer.Reset();
		if (UGGYGOHeroMovementMappingObserver* Observer = OriginalObserver.Get()) { Observer->Detach(); }
		ReleaseResources(Scope, Reason);
	}
}

void UGGYGOHeroMovementMappingObserver::Initialize(UGGYGOHeroComponent* Hero,
	UEnhancedInputLocalPlayerSubsystem* Subsystem, const TSharedPtr<FGGYGOHeroMovementInputScope>& Scope)
{
	check(Hero && Subsystem && Scope.IsValid() && !OriginalScope.IsValid());
	OriginalHero = Hero;
	OriginalSubsystem = Subsystem;
	OriginalScope = Scope;
	Subsystem->ControlMappingsRebuiltDelegate.AddDynamic(this, &ThisClass::OnMappingsRebuilt);
}

void UGGYGOHeroMovementMappingObserver::Detach()
{
	const TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> Subsystem = OriginalSubsystem;
	OriginalScope.Reset();
	OriginalHero.Reset();
	OriginalSubsystem.Reset();
	if (UEnhancedInputLocalPlayerSubsystem* Source = Subsystem.Get())
	{
		Source->ControlMappingsRebuiltDelegate.RemoveDynamic(this, &ThisClass::OnMappingsRebuilt);
	}
}

void UGGYGOHeroMovementMappingObserver::BeginDestroy()
{
	Detach();
	Super::BeginDestroy();
}

void UGGYGOHeroMovementMappingObserver::OnMappingsRebuilt()
{
	const TSharedPtr<FGGYGOHeroMovementInputScope> Scope = OriginalScope;
	const TWeakObjectPtr<UGGYGOHeroComponent> Hero = OriginalHero;
	if (Scope.IsValid())
	{
		if (UGGYGOHeroComponent* Original = Hero.Get()) { Original->HandleMovementMappingsRebuilt(Scope); }
	}
}

UGGYGOHeroComponent::UGGYGOHeroComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 本组件只在输入事件到达时工作，没有需要每帧推进的状态。
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;
}

void UGGYGOHeroComponent::OnRegister()
{
	Super::OnRegister();

	if (!GetPawn<APawn>())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("UGGYGOHeroComponent 只能挂在 Pawn 上，当前挂在 [%s]。"), *GetNameSafe(GetOwner()));
		return;
	}

	RegisterInitStateFeature();
}

void UGGYGOHeroComponent::BeginPlay()
{
	Super::BeginPlay();

	// 相机由引擎按需拉取；HeroComponent 只负责在每次拉取时给出
	// 已完成能力覆盖仲裁的“当前有效模式”。
	if (APawn* Pawn = GetPawn<APawn>())
	{
		if (UGGYGOCameraComponent* CameraComponent = UGGYGOCameraComponent::FindCameraComponent(Pawn))
		{
			CameraComponent->DetermineCameraModeDelegate.BindUObject(this, &ThisClass::DetermineCameraMode);
		}

		if (UGGYGOPawnExtensionComponent* PawnExtension = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Pawn))
		{
			FString Error;
			if (!PrepareLocalAbilitySystemSubscription(PawnExtension, Error))
			{
				UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("%s"), *Error);
				return;
			}
			if (bEndingPlay || IsBeingDestroyed()) { return; }
		}
	}

	// 只关心 PawnExtension 的状态：输入初始化需要 PawnData，而 PawnData 归它管。
	BindOnActorInitStateChanged(UGGYGOPawnExtensionComponent::NAME_ActorFeatureName, FGameplayTag(), false);

	ensure(TryToChangeInitState(GGYGOGameplayTags::InitState_Spawned));
	CheckDefaultInitialization();
}

void UGGYGOHeroComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPlay = true;
	if (APawn* Pawn = GetPawn<APawn>())
	{
		if (UGGYGOCameraComponent* CameraComponent = UGGYGOCameraComponent::FindCameraComponent(Pawn))
		{
			CameraComponent->DetermineCameraModeDelegate.Unbind();
		}
	}

	// Close notice admission before retiring native input/ASC resources.
	ReleaseLocalAbilitySystemSubscription();
	ReleasePlayerInput();
	AbilityCameraModeOverrides.Reset();
	if (APawn* Pawn = GetPawn<APawn>())
	{
		if (UGGYGOCameraComponent* CameraComponent = UGGYGOCameraComponent::FindCameraComponent(Pawn))
		{
			CameraComponent->ResetCameraRuntimeState();
		}
	}
	UnregisterInitStateFeature();

	Super::EndPlay(EndPlayReason);
}

TSubclassOf<UGGYGOCameraMode> UGGYGOHeroComponent::DetermineCameraMode() const
{
	if (!AbilityCameraModeOverrides.IsEmpty())
	{
		return AbilityCameraModeOverrides.Last().CameraMode;
	}

	const APawn* Pawn = GetPawn<APawn>();
	const UGGYGOPawnExtensionComponent* PawnExtComp = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Pawn);
	const UGGYGOPawnData* PawnData = PawnExtComp ? PawnExtComp->GetPawnData<UGGYGOPawnData>() : nullptr;
	return PawnData ? PawnData->DefaultCameraMode : nullptr;
}

uint64 UGGYGOHeroComponent::SetAbilityCameraMode(TSubclassOf<UGGYGOCameraMode> CameraMode, const FGameplayAbilitySpecHandle& OwningSpecHandle)
{
	if (!CameraMode || !OwningSpecHandle.IsValid())
	{
		return 0;
	}

	// 不回绕复用代次，避免旧能力清理误匹配后续请求。
	if (LastAbilityCameraModeRequestGeneration == MAX_uint64)
	{
		return 0;
	}
	const uint64 RequestGeneration = ++LastAbilityCameraModeRequestGeneration;

	AbilityCameraModeOverrides.RemoveAll([&OwningSpecHandle](const FGGYGOAbilityCameraModeOverride& Override)
	{
		return Override.OwningSpecHandle == OwningSpecHandle;
	});

	FGGYGOAbilityCameraModeOverride& NewOverride = AbilityCameraModeOverrides.AddDefaulted_GetRef();
	NewOverride.CameraMode = CameraMode;
	NewOverride.OwningSpecHandle = OwningSpecHandle;
	NewOverride.RequestGeneration = RequestGeneration;

	return RequestGeneration;
}

bool UGGYGOHeroComponent::ClearAbilityCameraMode(const FGameplayAbilitySpecHandle& OwningSpecHandle, uint64 RequestGeneration)
{
	if (!OwningSpecHandle.IsValid() || RequestGeneration == 0)
	{
		return false;
	}

	const int32 RemovedCount = AbilityCameraModeOverrides.RemoveAll([&OwningSpecHandle, RequestGeneration](const FGGYGOAbilityCameraModeOverride& Override)
	{
		return Override.OwningSpecHandle == OwningSpecHandle && Override.RequestGeneration == RequestGeneration;
	});
	return RemovedCount > 0;
}

bool UGGYGOHeroComponent::CanChangeInitState(UGameFrameworkComponentManager* Manager, FGameplayTag CurrentState, FGameplayTag DesiredState) const
{
	check(Manager);

	APawn* Pawn = GetPawn<APawn>();

	if (!CurrentState.IsValid() && DesiredState == GGYGOGameplayTags::InitState_Spawned)
	{
		return Pawn != nullptr;
	}

	if (CurrentState == GGYGOGameplayTags::InitState_Spawned && DesiredState == GGYGOGameplayTags::InitState_DataAvailable)
	{
		if (!Pawn)
		{
			return false;
		}

		// 本组件只服务被玩家操控的单位。
		//
		// 模拟代理（别人的角色在我的客户端上）永远等不到本地 PlayerController，
		// 若一并要求就会永久卡在 Spawned —— 而 PawnExtension 的
		// DataInitialized 要等**所有** feature 到达 DataAvailable，
		// 于是整个角色的初始化链条都会因此停住，连动画都起不来。
		//
		// 所以这里只对本地控制的角色要求 Controller，其余直接放行。
		const bool bIsLocallyControlled = Pawn->IsLocallyControlled();
		if (bIsLocallyControlled && !GetController<APlayerController>())
		{
			return false;
		}

		return true;
	}

	if (CurrentState == GGYGOGameplayTags::InitState_DataAvailable && DesiredState == GGYGOGameplayTags::InitState_DataInitialized)
	{
		// 等 PawnExtension 拿到 PawnData。输入配置在里面。
		return Manager->HasFeatureReachedInitState(Pawn, UGGYGOPawnExtensionComponent::NAME_ActorFeatureName, GGYGOGameplayTags::InitState_DataAvailable);
	}

	if (CurrentState == GGYGOGameplayTags::InitState_DataInitialized && DesiredState == GGYGOGameplayTags::InitState_GameplayReady)
	{
		return true;
	}

	return false;
}

bool UGGYGOHeroComponent::ConsumeLocalCameraProviderReady(APawn* ExpectedPawn, const TCHAR* NativeEntry)
{
	const TWeakObjectPtr<UGGYGOHeroComponent> OriginalHero(this);
	const TWeakObjectPtr<APawn> OriginalPawn(ExpectedPawn);
	const uint64 OriginalInputGeneration = InputSessionGeneration;
	if (bEndingPlay || IsBeingDestroyed() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !OriginalPawn.IsValid() || ExpectedPawn->IsActorBeingDestroyed() || GetOwner() != ExpectedPawn)
	{
		return false;
	}
	APlayerController* PC = Cast<APlayerController>(ExpectedPawn->GetController());
	if (!ExpectedPawn->IsLocallyControlled() || !PC || !PC->IsLocalController())
	{
		return true; // This native entry has no local player camera provider.
	}
	// BeginPlay installs the existing mode provider before its native init-state continuation.
	// Setup/input may arrive earlier; do not evaluate an uninstalled provider or poll for it.
	if (!HasBegunPlay()) { return true; }
	const TWeakObjectPtr<APlayerController> OriginalPC(PC);
	const TWeakObjectPtr<UWorld> OriginalWorld(GetWorld());
	const TWeakObjectPtr<ULocalPlayer> OriginalPlayer(PC->GetLocalPlayer());
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension =
		UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(ExpectedPawn);
	const UGGYGOPawnData* PawnData = OriginalExtension.IsValid()
		? OriginalExtension->GetPawnData<UGGYGOPawnData>() : nullptr;
	const TWeakObjectPtr<const UGGYGOPawnData> OriginalData(PawnData);
	if (OriginalExtension.IsValid() && !PawnData)
	{
		return true; // Actual PawnData has not arrived; the existing data transition consumes it later.
	}
	const TWeakObjectPtr<UGGYGOCameraComponent> OriginalCamera =
		UGGYGOCameraComponent::FindCameraComponent(ExpectedPawn);
	const TWeakObjectPtr<AGGYGOPlayerCameraManager> OriginalManager =
		Cast<AGGYGOPlayerCameraManager>(PC->PlayerCameraManager);
	const FString Context = FString::Printf(
		TEXT("[Camera/Hero] Entry='%s' Hero='%s' Pawn='%s' Controller='%s' World='%s' LocalPlayer='%s' Extension='%s' PawnData='%s' Manager='%s' Camera='%s'"),
		NativeEntry, *GetPathName(), *GetPathNameSafe(ExpectedPawn), *GetPathNameSafe(PC),
		*GetPathNameSafe(OriginalWorld.Get()), *GetPathNameSafe(OriginalPlayer.Get()),
		*GetPathNameSafe(OriginalExtension.Get()), *GetPathNameSafe(OriginalData.Get()),
		*GetPathNameSafe(OriginalManager.Get()), *GetPathNameSafe(OriginalCamera.Get()));
	const auto Report = [&Context](const TCHAR* Field, const TCHAR* Reason)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("%s Field='%s' Reason='%s'."), *Context, Field, Reason);
	};
	if (!OriginalPC.IsValid() || PC->IsActorBeingDestroyed() || !OriginalWorld.IsValid()
		|| ExpectedPawn->GetWorld() != OriginalWorld.Get() || PC->GetWorld() != OriginalWorld.Get()
		|| PC->GetPawn() != ExpectedPawn || !OriginalPlayer.IsValid()
		|| OriginalPlayer->PlayerController.Get() != PC)
	{
		Report(TEXT("Provider.Context"), TEXT("original-local-pawn-controller-world-player-association-invalid"));
		return false;
	}
	if (!OriginalExtension.IsValid() || OriginalExtension->IsBeingDestroyed()
		|| OriginalExtension->GetOwner() != ExpectedPawn || !OriginalData.IsValid())
	{
		Report(TEXT("Provider.PawnData"), TEXT("original-pawn-extension-data-source-invalid"));
		return true; // Report the camera dependency; keep the original input initialization independent.
	}
	if (!OriginalCamera.IsValid() || OriginalCamera->GetOwner() != ExpectedPawn
		|| OriginalCamera->GetWorld() != OriginalWorld.Get() || !OriginalManager.IsValid()
		|| OriginalManager->PCOwner != PC || OriginalManager->GetWorld() != OriginalWorld.Get())
	{
		Report(TEXT("Provider.CameraManager"), TEXT("original-project-camera-or-manager-association-invalid"));
		return true;
	}
	const FGGYGOCameraEvaluationResult Result =
		OriginalManager->ActivateCameraEvaluation(ExpectedPawn, OriginalCamera.Get());
	UGGYGOHeroComponent* Hero = OriginalHero.Get();
	const bool bOriginalContextCurrent = Hero && !Hero->bEndingPlay && !Hero->IsBeingDestroyed()
		&& !Hero->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		&& Hero->InputSessionGeneration == OriginalInputGeneration
		&& OriginalPawn.IsValid() && !OriginalPawn->IsActorBeingDestroyed() && Hero->GetOwner() == OriginalPawn.Get()
		&& OriginalPC.IsValid() && !OriginalPC->IsActorBeingDestroyed()
		&& OriginalPawn->GetController() == OriginalPC.Get() && OriginalPC->GetPawn() == OriginalPawn.Get()
		&& OriginalPC->IsLocalController() && OriginalPawn->IsLocallyControlled()
		&& OriginalWorld.IsValid() && Hero->GetWorld() == OriginalWorld.Get()
		&& OriginalPawn->GetWorld() == OriginalWorld.Get() && OriginalPC->GetWorld() == OriginalWorld.Get()
		&& OriginalPlayer.IsValid() && OriginalPC->GetLocalPlayer() == OriginalPlayer.Get()
		&& OriginalPlayer->PlayerController.Get() == OriginalPC.Get()
		&& OriginalExtension.IsValid() && !OriginalExtension->IsBeingDestroyed()
		&& OriginalExtension->GetOwner() == OriginalPawn.Get()
		&& UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(OriginalPawn.Get()) == OriginalExtension.Get()
		&& OriginalData.IsValid() && OriginalExtension->GetPawnData<UGGYGOPawnData>() == OriginalData.Get()
		&& OriginalCamera.IsValid() && OriginalCamera->GetOwner() == OriginalPawn.Get()
		&& OriginalCamera->GetWorld() == OriginalWorld.Get()
		&& UGGYGOCameraComponent::FindCameraComponent(OriginalPawn.Get()) == OriginalCamera.Get()
		&& OriginalManager.IsValid() && OriginalPC->PlayerCameraManager == OriginalManager.Get()
		&& OriginalManager->PCOwner == OriginalPC.Get() && OriginalManager->GetWorld() == OriginalWorld.Get()
		&& (!Result.IsSuccess() || OriginalManager->GetViewTarget() == OriginalPawn.Get());
	if (!Result.IsSuccess())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("%s Field='%s' Reason='%s' Mode='%s' ModeClass='%s'."),
			*Context, *Result.Field.ToString(), *Result.Reason, *Result.ModePath, *Result.ModeClassPath);
	}
	if (!bOriginalContextCurrent)
	{
		Report(TEXT("Provider.Context"), TEXT("original-identities-changed-during-activation; old-native-init-tail-stopped"));
	}
	return bOriginalContextCurrent;
}

void UGGYGOHeroComponent::HandleChangeInitState(UGameFrameworkComponentManager* Manager, FGameplayTag CurrentState, FGameplayTag DesiredState)
{
	if (CurrentState != GGYGOGameplayTags::InitState_DataAvailable || DesiredState != GGYGOGameplayTags::InitState_DataInitialized)
	{
		return;
	}

	APawn* Pawn = GetPawn<APawn>();
	if (!ConsumeLocalCameraProviderReady(Pawn, TEXT("DataInitialized"))) { return; }
	if (!Pawn || Pawn->InputComponent == nullptr)
	{
		// 输入组件还没建立。它由引擎在附身流程里创建，随后 Pawn 会调
		// SetupPlayerInputComponent，那里再走一次 InitializePlayerInput。
		return;
	}

	InitializePlayerInputBindings(Pawn->InputComponent);
}

void UGGYGOHeroComponent::OnActorInitStateChanged(const FActorInitStateChangedParams& Params)
{
	if (Params.FeatureName == UGGYGOPawnExtensionComponent::NAME_ActorFeatureName)
	{
		if (Params.FeatureState == GGYGOGameplayTags::InitState_DataAvailable)
		{
			CheckDefaultInitialization();
		}
	}
}

void UGGYGOHeroComponent::CheckDefaultInitialization()
{
	static const TArray<FGameplayTag> StateChain = {
		GGYGOGameplayTags::InitState_Spawned,
		GGYGOGameplayTags::InitState_DataAvailable,
		GGYGOGameplayTags::InitState_DataInitialized,
		GGYGOGameplayTags::InitState_GameplayReady
	};

	ContinueInitStateChain(StateChain);
}

void UGGYGOHeroComponent::InitializePlayerInput(UInputComponent* PlayerInputComponent)
{
	if (bEndingPlay) { return; }
	if (!ConsumeLocalCameraProviderReady(GetPawn<APawn>(), TEXT("InitializePlayerInput"))) { return; }
	InitializePlayerInputBindings(PlayerInputComponent);
}

void UGGYGOHeroComponent::InitializePlayerInputBindings(UInputComponent* PlayerInputComponent)
{
	const uint64 ExpectedGeneration = InputSessionGeneration == MAX_uint64 ? MAX_uint64 : InputSessionGeneration + 1;
	ReleasePlayerInput();
	// 释放 IMC 的同步回调可以建立后继会话；旧初始化不能接着覆盖它。
	if (InputSessionGeneration != ExpectedGeneration || PlayerInputSession.IsValid())
	{
		return;
	}
	if (InputSessionGeneration == MAX_uint64)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("InitializePlayerInput: 输入会话代次已耗尽。"));
		return;
	}

	const APawn* Pawn = GetPawn<APawn>();
	if (!Pawn)
	{
		return;
	}

	const APlayerController* PC = GetController<APlayerController>();
	if (!PC)
	{
		return;
	}

	const ULocalPlayer* LP = PC->GetLocalPlayer();
	if (!LP)
	{
		return;
	}

	UEnhancedInputLocalPlayerSubsystem* Subsystem = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	if (!Subsystem)
	{
		return;
	}

	const UGGYGOPawnExtensionComponent* PawnExtComp = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Pawn);
	const UGGYGOPawnData* PawnData = PawnExtComp ? PawnExtComp->GetPawnData<UGGYGOPawnData>() : nullptr;
	const UGGYGOInputConfig* InputConfig = PawnData ? PawnData->InputConfig : nullptr;

	if (!InputConfig)
	{
		// 没有输入配置时移动与能力输入都不会生效，角色完全不受控。
		// 这几乎总是配置遗漏，所以报错而不是静默返回。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializePlayerInput: [%s] 的 PawnData 没有配置 InputConfig，输入不会生效。"),
			*GetNameSafe(Pawn));
		return;
	}

	UGGYGOInputComponent* GGYGOIC = Cast<UGGYGOInputComponent>(PlayerInputComponent);
	if (!GGYGOIC)
	{
		// 输入组件类型由项目设置里的 DefaultInputComponentClass 决定。
		// 类型不对时批量绑定用不了，这是工程配置错误。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializePlayerInput: 输入组件不是 UGGYGOInputComponent。请在项目设置里把 DefaultInputComponentClass 设为它。"));
		return;
	}

	// 所有关键配置与来源先验证；失败时不能留下部分绑定或部分 IMC 注册。
	const UInputAction* MoveAction = InputConfig->FindNativeInputActionForTag(GGYGOGameplayTags::InputTag_Move, true);
	const UInputAction* MouseLookAction = InputConfig->FindNativeInputActionForTag(GGYGOGameplayTags::InputTag_Look_Mouse, true);
	const UInputAction* ForceWalkAction = InputConfig->FindNativeInputActionForTag(GGYGOGameplayTags::InputTag_ForceWalk, false);
	UEnhancedPlayerInput* EnhancedPlayerInput = Subsystem->GetPlayerInput();
	if (!MoveAction || !MouseLookAction || !EnhancedPlayerInput)
	{
		if (!EnhancedPlayerInput)
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("InitializePlayerInput: [%s] 尚无 EnhancedPlayerInput，未创建输入会话。"), *GetNameSafe(Pawn));
		}
		return;
	}

	UGGYGOPlayerInput* MovementSource = Cast<UGGYGOPlayerInput>(EnhancedPlayerInput);
	const ACharacter* MovementCharacter = GetPawn<ACharacter>();
	UGGYGOCharacterMovementComponent* MovementConsumer = MovementCharacter
		? Cast<UGGYGOCharacterMovementComponent>(MovementCharacter->GetCharacterMovement()) : nullptr;
	if (!MovementSource || !MovementConsumer || MoveAction->ValueType != EInputActionValueType::Axis2D)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Input/Hero Movement] Initialize rejected: Pawn='%s' InputConfig='%s' Action='%s' PlayerInput='%s' CMC='%s' Reason=RequiredSourceConsumerOrAxis2DActionInvalid."),
			*GetPathNameSafe(Pawn), *GetPathNameSafe(InputConfig), *GetPathNameSafe(MoveAction),
			*GetPathNameSafe(EnhancedPlayerInput), *GetPathNameSafe(MovementConsumer));
		return;
	}

	const int32 RegistrationPriority = InputMappingPriority;
	TArray<TWeakObjectPtr<const UInputMappingContext>> MappingsToRegister;
	for (const TObjectPtr<const UInputMappingContext>& Mapping : DefaultInputMappings)
	{
		if (!Mapping)
		{
			continue;
		}
		if (Mapping->GetRegistrationTrackingMode() != EMappingContextRegistrationTrackingMode::CountRegistrations)
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("InitializePlayerInput: IMC [%s] 必须迁移为 CountRegistrations；本次会话未创建，不修改共享资产。"),
				*GetNameSafe(Mapping.Get()));
			return;
		}
		int32 ExistingPriority = INDEX_NONE;
		if (Subsystem->HasMappingContext(Mapping, ExistingPriority) && ExistingPriority != RegistrationPriority)
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("InitializePlayerInput: IMC [%s] 已以优先级 %d 注册，本会话请求 %d；保持原注册，本次会话未创建。"),
				*GetNameSafe(Mapping.Get()), ExistingPriority, RegistrationPriority);
			return;
		}
		MappingsToRegister.AddUnique(Mapping.Get());
	}

	const uint64 SessionGeneration = InputSessionGeneration;
	const TSharedPtr<FPlayerInputSession> OriginalSession = MakeShared<FPlayerInputSession>(
		GGYGOIC, Subsystem, EnhancedPlayerInput, ForceWalkAction ? MovementConsumer : nullptr, SessionGeneration);
	PlayerInputSession = OriginalSession;

	const TSharedPtr<FGGYGOHeroMovementInputScope> OriginalMovementScope = MakeShared<FGGYGOHeroMovementInputScope>(
		this, GetPawn<APawn>(), GGYGOIC, MovementSource, Subsystem, MovementConsumer, MoveAction,
		SessionGeneration, MappingsToRegister.Num(), InputConfig);
	MovementInputScope = OriginalMovementScope;
	MovementMappingObserver = NewObject<UGGYGOHeroMovementMappingObserver>(this);
	OriginalMovementScope->Observer = MovementMappingObserver.Get();
	// Install the original registration before Add. Only actual native rebuilt notification begins Source.
	MovementMappingObserver->Initialize(this, Subsystem, OriginalMovementScope);
	const TWeakObjectPtr<UGGYGOHeroComponent> MovementHero(this);
	OriginalSession->BindHandles.Add(GGYGOIC->BindActionInstanceLambda(MoveAction, ETriggerEvent::Triggered,
		[MovementHero, OriginalMovementScope](const FInputActionInstance& Instance)
		{
			if (UGGYGOHeroComponent* Hero = MovementHero.Get()) { Hero->Input_Move(Instance, OriginalMovementScope); }
		}).GetHandle());
	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_Look_Mouse, ETriggerEvent::Triggered, this, &ThisClass::Input_LookMouse, /*bLogIfNotFound=*/true, &OriginalSession->BindHandles);

	// 手柄视角是可选的：只用键鼠的项目不配它，不该因此报错。
	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_Look_Stick, ETriggerEvent::Triggered, this, &ThisClass::Input_LookStick, /*bLogIfNotFound=*/false, &OriginalSession->BindHandles);
	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_ForceWalk, ETriggerEvent::Triggered, this, &ThisClass::Input_ForceWalkPressed, /*bLogIfNotFound=*/false, &OriginalSession->BindHandles);
	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_ForceWalk, ETriggerEvent::Completed, this, &ThisClass::Input_ForceWalkReleased, /*bLogIfNotFound=*/false, &OriginalSession->BindHandles);
	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_ForceWalk, ETriggerEvent::Canceled, this, &ThisClass::Input_ForceWalkReleased, /*bLogIfNotFound=*/false, &OriginalSession->BindHandles);
	const TWeakObjectPtr<UGGYGOHeroComponent> WeakHero(this);
	for (const FGGYGOInputAction& Action : InputConfig->AbilityInputActions)
	{
		if (!Action.InputAction || !Action.InputTag.IsValid())
		{
			continue;
		}
		const TSharedPtr<FAbilityActionBinding> Binding = MakeShared<FAbilityActionBinding>(
			Action.InputAction.Get(), Action.InputTag, OriginalSession);
		OriginalSession->ActionBindings.Add(Binding);
		OriginalSession->BindHandles.Add(GGYGOIC->BindActionInstanceLambda(Action.InputAction, ETriggerEvent::Triggered,
			[WeakHero, Binding](const FInputActionInstance& Instance)
			{
				if (UGGYGOHeroComponent* Hero = WeakHero.Get()) { Hero->Input_AbilityActionTriggered(Instance, Binding); }
			}).GetHandle());
		OriginalSession->BindHandles.Add(GGYGOIC->BindActionInstanceLambda(Action.InputAction, ETriggerEvent::Completed,
			[WeakHero, Binding](const FInputActionInstance& Instance)
			{
				if (UGGYGOHeroComponent* Hero = WeakHero.Get()) { Hero->Input_AbilityActionReleased(Instance, Binding); }
			}).GetHandle());
	}
	// Preserve original insertion order: Triggered/Completed per Action, then all Canceled.
	for (const TSharedPtr<FAbilityActionBinding>& Binding : OriginalSession->ActionBindings)
	{
		OriginalSession->BindHandles.Add(GGYGOIC->BindActionInstanceLambda(Binding->SourceAction.Get(), ETriggerEvent::Canceled,
			[WeakHero, Binding](const FInputActionInstance& Instance)
			{
				if (UGGYGOHeroComponent* Hero = WeakHero.Get()) { Hero->Input_AbilityActionReleased(Instance, Binding); }
			}).GetHandle());
	}

	for (const TWeakObjectPtr<const UInputMappingContext>& WeakMapping : MappingsToRegister)
	{
		if (PlayerInputSession != OriginalSession || OriginalSession->bRetired) { return; }
		const UInputMappingContext* Mapping = WeakMapping.Get();
		int32 ExistingPriority = INDEX_NONE;
		// 前一项 Add 会同步广播；再验证当前项，不能在来源改变后继续注册。
		if (!Mapping || !OriginalSession->Subsystem.IsValid() || !OriginalSession->PlayerInput.IsValid()
			|| Subsystem->GetPlayerInput() != EnhancedPlayerInput
			|| Mapping->GetRegistrationTrackingMode() != EMappingContextRegistrationTrackingMode::CountRegistrations
			|| (Subsystem->HasMappingContext(Mapping, ExistingPriority) && ExistingPriority != RegistrationPriority))
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("InitializePlayerInput: IMC [%s] 的来源、模式或优先级在注册期间改变，回收本会话。"), *GetNameSafe(Mapping));
			if (PlayerInputSession == OriginalSession) { ReleasePlayerInput(); }
			return;
		}
		// 已验证底层输入对象与模式；引擎先增加计数，再广播 Added。
		// 先登记自己的这一份，以便同步 Release 能与刚完成的 Add 配对。
		FPlayerInputSession::FRegisteredMapping& Registered = OriginalSession->Mappings.AddDefaulted_GetRef();
		Registered.MappingContext = Mapping;
		Registered.RegisteredPriority = RegistrationPriority;
		Subsystem->AddMappingContext(Mapping, RegistrationPriority);
		if (PlayerInputSession != OriginalSession || OriginalSession->bRetired || InputSessionGeneration != SessionGeneration)
		{
			return;
		}
	}

	if (PlayerInputSession != OriginalSession || OriginalSession->bRetired) { return; }
	if (!OriginalSession->Component.IsValid() || !OriginalSession->Subsystem.IsValid() || !OriginalSession->PlayerInput.IsValid()
		|| Subsystem->GetPlayerInput() != EnhancedPlayerInput)
	{
		ReleasePlayerInput();
		return;
	}
	// Input may precede Ready; the exact typed Ready notice later associates this existing session.
	AssociateReadyAbilitySystemWithInput();
}

void UGGYGOHeroComponent::ReleasePlayerInput()
{
	const TSharedPtr<FPlayerInputSession> PreviousSession = MoveTemp(PlayerInputSession);
	if (InputSessionGeneration != MAX_uint64) { ++InputSessionGeneration; }
	if (PreviousSession.IsValid())
	{
		PreviousSession->bRetired = true;
		const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = LocalAbilitySystemSubscription;
		if (OriginalSubscription.IsValid() && OriginalSubscription->AssociatedInputSession.Pin() == PreviousSession)
		{
			OriginalSubscription->AssociatedInputSession.Reset();
		}
		for (const TSharedPtr<FAbilityActionBinding>& Binding : PreviousSession->ActionBindings)
		{
			Binding->bRetired = true;
		}
	}
	// Seal local records before Source/CMC or ASC cleanup can install a successor.
	TArray<FGGYGOAbilityInputRequestIdentity> OriginalIdentities = PreviousSession.IsValid()
		? RetireAbilityInputObservations(PreviousSession->Observations) : TArray<FGGYGOAbilityInputRequestIdentity>{};
	OriginalIdentities.Append(RetireAbilityInputAssociation(PreviousSession));
	const TWeakObjectPtr<UGGYGOInputComponent> PreviousComponent =
		PreviousSession.IsValid() ? PreviousSession->Component : TWeakObjectPtr<UGGYGOInputComponent>{};
	const TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> PreviousSubsystem =
		PreviousSession.IsValid() ? PreviousSession->Subsystem : TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem>{};
	const TWeakObjectPtr<UEnhancedPlayerInput> PreviousPlayerInput =
		PreviousSession.IsValid() ? PreviousSession->PlayerInput : TWeakObjectPtr<UEnhancedPlayerInput>{};
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> PreviousMovement =
		PreviousSession.IsValid() ? PreviousSession->ForceWalkMovement : TWeakObjectPtr<UGGYGOCharacterMovementComponent>{};
	TArray<uint32> PreviousHandles = PreviousSession.IsValid() ? MoveTemp(PreviousSession->BindHandles) : TArray<uint32>{};
	TArray<FPlayerInputSession::FRegisteredMapping> PreviousMappings = PreviousSession.IsValid()
		? MoveTemp(PreviousSession->Mappings) : TArray<FPlayerInputSession::FRegisteredMapping>{};
	const TSharedPtr<FGGYGOHeroMovementInputScope> PreviousMovementScope = MoveTemp(MovementInputScope);
	const TObjectPtr<UGGYGOHeroMovementMappingObserver> PreviousObserver = MovementMappingObserver;
	MovementMappingObserver = nullptr;
	if (PreviousMovementScope.IsValid()) { PreviousMovementScope->bRetired = true; }
	if (PreviousObserver) { PreviousObserver->Detach(); }
	if (UGGYGOCharacterMovementComponent* Movement = PreviousMovement.Get())
	{
		Movement->SetForceWalkRequested(false);
	}
	GGYGOHeroMovementInput::Retire(PreviousMovementScope, TEXT("HeroInputReleased"));
	EndOriginalAbilityInputRequests(OriginalIdentities);

	if (UGGYGOInputComponent* InputComponent = PreviousComponent.Get())
	{
		InputComponent->RemoveBinds(PreviousHandles);
	}
	// 记录已摘除；Remove 的同步回调即使再次退出，也不会重复归还旧注册。
	for (const FPlayerInputSession::FRegisteredMapping& Registered : PreviousMappings)
	{
		UEnhancedInputLocalPlayerSubsystem* Subsystem = PreviousSubsystem.Get();
		const UInputMappingContext* Mapping = Registered.MappingContext.Get();
		if (!Subsystem || !Mapping)
		{
			continue;
		}
		if (!PreviousPlayerInput.IsValid() || Subsystem->GetPlayerInput() != PreviousPlayerInput.Get())
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("ReleasePlayerInput: 原 Subsystem 已失去本会话 PlayerInput，不对新输入对象移除旧 IMC [%s]。"), *GetNameSafe(Mapping));
			break;
		}
		if (Mapping->GetRegistrationTrackingMode() != EMappingContextRegistrationTrackingMode::CountRegistrations)
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("ReleasePlayerInput: IMC [%s] 的注册模式已改变，不用 Untracked 移除其它持有者。"), *GetNameSafe(Mapping));
			continue;
		}
		int32 ExistingPriority = INDEX_NONE;
		if (!Subsystem->HasMappingContext(Mapping, ExistingPriority))
		{
			continue;
		}
		if (ExistingPriority != Registered.RegisteredPriority)
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("ReleasePlayerInput: IMC [%s] 的优先级已从本会话 %d 变为 %d，原注册已被替换，保持现有注册。"),
				*GetNameSafe(Mapping), Registered.RegisteredPriority, ExistingPriority);
			continue;
		}
		Subsystem->RemoveMappingContext(Mapping);
	}
}

bool UGGYGOHeroComponent::IsMovementInputScopeCurrent(const TSharedPtr<FGGYGOHeroMovementInputScope>& Scope) const
{
	if (!Scope.IsValid() || Scope->bRetired || MovementInputScope != Scope
		|| Scope->InputGeneration != InputSessionGeneration || !HasValidPlayerInputSession()
		|| IsBeingDestroyed() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !Scope->Component.HasSameIndexAndSerialNumber(PlayerInputSession->Component)
		|| Scope->Source.Get() != PlayerInputSession->PlayerInput.Get()
		|| !Scope->Subsystem.HasSameIndexAndSerialNumber(PlayerInputSession->Subsystem))
	{
		return false;
	}
	const ACharacter* Pawn = Cast<ACharacter>(Scope->Pawn.Get());
	return Pawn && GetOwner() == Pawn && !Pawn->IsActorBeingDestroyed()
		&& Pawn->InputComponent == Scope->Component.Get() && Scope->Action.IsValid()
		&& Scope->Consumer.IsValid() && Scope->Consumer->GetOwner() == Pawn
		&& Pawn->GetCharacterMovement() == Scope->Consumer.Get();
}

void UGGYGOHeroComponent::HandleMovementMappingsRebuilt(const TSharedPtr<FGGYGOHeroMovementInputScope>& OriginalScope)
{
	if (!IsMovementInputScopeCurrent(OriginalScope)) { return; }
	// A real rebuild can occur inside an Added callback; wait for all of this scope's actual Add registrations.
	if (PlayerInputSession->Mappings.Num() != OriginalScope->ExpectedMappingRegistrationCount) { return; }
	const TWeakObjectPtr<UGGYGOHeroComponent> OriginalHero(this);
	GGYGOHeroMovementInput::ReleaseResources(OriginalScope, TEXT("NativeMappingsRebuilt"));
	UGGYGOHeroComponent* Hero = OriginalHero.Get();
	if (!Hero || !Hero->IsMovementInputScopeCurrent(OriginalScope)
		|| OriginalScope->Session.SessionSerial != 0 || OriginalScope->Binding.ConsumerBindingSerial != 0) { return; }
	OriginalScope->ReportedReasons.Reset();
	const TWeakObjectPtr<UGGYGOPlayerInput> Source = OriginalScope->Source;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> Consumer = OriginalScope->Consumer;
	const uint64 ExpectedConsumerSerial = Consumer->GetMovementInputBindingSerial();
	FGGYGOMovementInputSessionIdentity Session;
	FString Error;
	if (!Source->BeginMovementInputSession(OriginalScope->Pawn.Get(), OriginalScope->Component.Get(),
		OriginalScope->Action.Get(), Session, Error))
	{
		Hero = OriginalHero.Get();
		if (Hero && Hero->IsMovementInputScopeCurrent(OriginalScope)
			&& OriginalScope->Session.SessionSerial == 0 && OriginalScope->Binding.ConsumerBindingSerial == 0)
		{
			GGYGOHeroMovementInput::Report(OriginalScope, TEXT("BeginRejected"), Error);
			GGYGOHeroMovementInput::Retire(OriginalScope, TEXT("BeginRejected"));
		}
		else
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("[Input/Hero Movement] Original Begin rejected after reentry: %s"), *Error);
		}
		return;
	}
	Hero = OriginalHero.Get();
	if (!Hero || !Hero->IsMovementInputScopeCurrent(OriginalScope)
		|| OriginalScope->Session.SessionSerial != 0 || OriginalScope->Binding.ConsumerBindingSerial != 0)
	{
		GGYGOHeroMovementInput::ReleaseResources(Source, Session, Consumer, {}, TEXT("BeginInterrupted"));
		return;
	}
	// Own the prepared token immediately, including reentry from CMC Bind; no receiver exists yet.
	OriginalScope->Session = Session;
	FGGYGOMovementInputConsumerBindingId Binding;
	if (!Consumer.IsValid() || !Consumer->BindMovementInputSession(Session, ExpectedConsumerSerial, Binding, Error))
	{
		if (OriginalScope->Session == Session && OriginalScope->Binding.ConsumerBindingSerial == 0)
		{
			GGYGOHeroMovementInput::Report(OriginalScope, TEXT("BindRejected"),
				Error.IsEmpty() ? TEXT("Original CMC became unavailable before Bind.") : Error);
			GGYGOHeroMovementInput::Retire(OriginalScope, TEXT("BindRejected"));
		}
		else
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("[Input/Hero Movement] Original Bind rejected after reentry: %s"), *Error);
			GGYGOHeroMovementInput::ReleaseResources(Source, Session, Consumer, {}, TEXT("BindInterrupted"));
		}
		return;
	}
	Hero = OriginalHero.Get();
	if (!Hero || !Hero->IsMovementInputScopeCurrent(OriginalScope) || OriginalScope->Session != Session
		|| OriginalScope->Binding.ConsumerBindingSerial != 0
		|| Consumer->GetMovementInputBindingSerial() != Binding.ConsumerBindingSerial)
	{
		GGYGOHeroMovementInput::ReleaseResources(Source, Session, Consumer, Binding, TEXT("BindInterrupted"));
		return;
	}
	OriginalScope->Binding = Binding; // Both original resources precede Attach's synchronous replay.
	const TWeakPtr<FGGYGOHeroMovementInputScope> WeakScope(OriginalScope);
	const auto IsOriginalAssemblyCurrent = [OriginalHero, WeakScope, Session, Binding]()
	{
		const TSharedPtr<FGGYGOHeroMovementInputScope> Scope = WeakScope.Pin();
		const UGGYGOHeroComponent* CurrentHero = OriginalHero.Get();
		return CurrentHero && CurrentHero->IsMovementInputScopeCurrent(Scope)
			&& Scope->Session == Session && Scope->Binding == Binding
			&& Scope->Consumer->GetMovementInputBindingSerial() == Binding.ConsumerBindingSerial;
	};
	FGGYGOMovementOwnerSyncScopeId OwnerScope;
	if (!Consumer->GetMovementOwnerSyncScope(OwnerScope, Error)
		|| !OwnerScope.IsSet() || !OwnerScope.GetConsumer().HasSameIndexAndSerialNumber(Consumer)
		|| !OwnerScope.GetOriginalPawn().HasSameIndexAndSerialNumber(OriginalScope->Pawn)
		|| OwnerScope.GetOriginalPlayerController().Get() != OriginalScope->Pawn->GetController())
	{
		if (IsOriginalAssemblyCurrent())
		{
			GGYGOHeroMovementInput::Report(OriginalScope, TEXT("OwnerSyncScopeUnavailable"),
				Error.IsEmpty() ? TEXT("Native owner scope does not identify this original Pawn/CMC/Controller.") : Error);
			GGYGOHeroMovementInput::Retire(OriginalScope, TEXT("OwnerSyncScopeUnavailable"));
		}
		else
		{
			GGYGOHeroMovementInput::ReleaseResources(Source, Session, Consumer, Binding, TEXT("OwnerSyncScopeInterrupted"));
		}
		return;
	}
	if (!IsOriginalAssemblyCurrent())
	{
		GGYGOHeroMovementInput::ReleaseResources(Source, Session, Consumer, Binding, TEXT("OwnerSyncScopeInterrupted"));
		return;
	}
	const auto TryAdmitHeldRequest = [WeakScope, Source, Consumer, Session, Binding, IsOriginalAssemblyCurrent]
		(const FGGYGOMovementInputRequestIdentity* ExpectedRequest)
	{
		const TSharedPtr<FGGYGOHeroMovementInputScope> Scope = WeakScope.Pin();
		if (!IsOriginalAssemblyCurrent() || !Scope->OwnerSyncObserver.IsSet()
			|| !Scope->OwnerSyncNotice.IsSet()
			|| Scope->OwnerSyncNotice.GetState() != EGGYGOMovementOwnerSyncState::Ready
			|| !Scope->OwnerSyncNotice.IsInitialSynchronizationEligible()) { return; }
		const FGGYGOMovementOwnerSyncObserverId OriginalObserver = Scope->OwnerSyncObserver;
		const FGGYGOMovementOwnerSyncNotice OriginalReady = Scope->OwnerSyncNotice;
		FGGYGOMovementInputRequestIdentity Request;
		FString AdmissionError;
		// False includes ordinary real release/no request. It never authorizes admission.
		if (!Source->GetMovementInputRequest(Session, Request, AdmissionError)) { return; }
		if (!IsOriginalAssemblyCurrent()
			|| Scope->OwnerSyncObserver.GetScope() != OriginalObserver.GetScope()
			|| Scope->OwnerSyncObserver.GetObserverSerial() != OriginalObserver.GetObserverSerial()
			|| Scope->OwnerSyncNotice.GetNoticeSerial() != OriginalReady.GetNoticeSerial()) { return; }
		if (Request.Session != Session || Request.RequestSerial == 0)
		{
			GGYGOHeroMovementInput::Report(Scope, TEXT("InitialRequestOriginMismatch"),
				TEXT("Source Held lookup did not identify the original session's allocated request."));
			GGYGOHeroMovementInput::Retire(Scope, TEXT("InitialRequestOriginMismatch"));
			return;
		}
		// A Started callback may have released/reentered before Consume returned.
		if (ExpectedRequest && Request != *ExpectedRequest) { return; }
		const EGGYGOMovementInitialRequestAdmissionResult Admission = Consumer->TryAdmitInitialMovementInputRequest(
			OriginalObserver, Binding, Request, OriginalReady, AdmissionError);
		if (!IsOriginalAssemblyCurrent()
			|| Scope->OwnerSyncObserver.GetScope() != OriginalObserver.GetScope()
			|| Scope->OwnerSyncObserver.GetObserverSerial() != OriginalObserver.GetObserverSerial()
			|| Scope->OwnerSyncNotice.GetNoticeSerial() != OriginalReady.GetNoticeSerial()) { return; }
		if (Admission == EGGYGOMovementInitialRequestAdmissionResult::ExecutionFailed)
		{
			GGYGOHeroMovementInput::Report(Scope, TEXT("InitialRequestExecutionFailed"), AdmissionError);
			// CMC retains the same FAILED request and requires real Release then Press.
		}
		else if (Admission == EGGYGOMovementInitialRequestAdmissionResult::Stale
			|| Admission == EGGYGOMovementInitialRequestAdmissionResult::Rejected)
		{
			GGYGOHeroMovementInput::Report(Scope, TEXT("InitialRequestAdmissionRejected"), AdmissionError);
			GGYGOHeroMovementInput::Retire(Scope, TEXT("InitialRequestAdmissionRejected"));
		}
		// Admitted/AlreadyAdmitted do not write a second admission state or replay Started.
	};
	const FGGYGOMovementOwnerSyncDelegate OwnerSyncReceiver = FGGYGOMovementOwnerSyncDelegate::CreateLambda(
		[WeakScope, Session, Binding, OwnerScope, IsOriginalAssemblyCurrent, TryAdmitHeldRequest]
		(const FGGYGOMovementOwnerSyncObserverId& ReceivedObserver, const FGGYGOMovementOwnerSyncNotice& Notice)
		{
			const TSharedPtr<FGGYGOHeroMovementInputScope> Scope = WeakScope.Pin();
			if (!Scope.IsValid() || Scope->bRetired || Scope->Session != Session || Scope->Binding != Binding) { return; }
			if (!ReceivedObserver.IsSet() || !Scope->OwnerSyncObserver.IsSet()
				|| ReceivedObserver.GetScope() != OwnerScope || Scope->OwnerSyncObserver.GetScope() != OwnerScope
				|| ReceivedObserver.GetObserverSerial() != Scope->OwnerSyncObserver.GetObserverSerial()
				|| !Notice.IsSet() || Notice.GetScope() != OwnerScope)
			{
				GGYGOHeroMovementInput::Report(Scope, TEXT("OwnerSyncNoticeOriginMismatch"),
					TEXT("Owner-sync callback did not match its original subscription and native owner scope."));
				GGYGOHeroMovementInput::Retire(Scope, TEXT("OwnerSyncNoticeOriginMismatch"));
				return;
			}
			// Terminal ownership is matched before live Pawn/Controller checks; they may already be gone.
			if (Notice.GetState() == EGGYGOMovementOwnerSyncState::Invalidated)
			{
				GGYGOHeroMovementInput::Report(Scope, TEXT("OwnerSyncInvalidated"), Notice.GetReason().ToString());
				// Return this assembly; a later real mapping rebuild still belongs to the outer input lifecycle.
				GGYGOHeroMovementInput::ReleaseResources(Scope, TEXT("OwnerSyncInvalidated"));
				return;
			}
			if (!IsOriginalAssemblyCurrent())
			{
				GGYGOHeroMovementInput::Retire(Scope, TEXT("OwnerSyncInputUnavailable"));
				return;
			}
			if (Scope->OwnerSyncNotice.IsSet() && Notice.GetNoticeSerial() <= Scope->OwnerSyncNotice.GetNoticeSerial()) { return; }
			Scope->OwnerSyncNotice = Notice;
			if (Notice.GetState() == EGGYGOMovementOwnerSyncState::Ready) { TryAdmitHeldRequest(nullptr); }
		});
	// Direct member output is installed before synchronous replay; never save it after return.
	if (!Consumer->SubscribeMovementOwnerSync(OwnerScope, OwnerSyncReceiver, OriginalScope->OwnerSyncObserver, Error))
	{
		if (IsOriginalAssemblyCurrent())
		{
			GGYGOHeroMovementInput::Report(OriginalScope, TEXT("OwnerSyncSubscribeRejected"), Error);
			GGYGOHeroMovementInput::Retire(OriginalScope, TEXT("OwnerSyncSubscribeRejected"));
		}
		else
		{
			GGYGOHeroMovementInput::ReleaseResources(Source, Session, Consumer, Binding, TEXT("OwnerSyncSubscribeInterrupted"));
		}
		return;
	}
	if (!IsOriginalAssemblyCurrent() || !OriginalScope->OwnerSyncObserver.IsSet()
		|| OriginalScope->OwnerSyncObserver.GetScope() != OwnerScope)
	{
		if (OriginalScope->Session == Session && OriginalScope->Binding == Binding)
		{
			GGYGOHeroMovementInput::ReleaseResources(OriginalScope, TEXT("OwnerSyncReplayInterrupted"));
		}
		else
		{
			GGYGOHeroMovementInput::ReleaseResources(Source, Session, Consumer, Binding, TEXT("OwnerSyncReplayInterrupted"));
		}
		return;
	}
	const FGGYGOMovementInputFactDelegate Receiver = FGGYGOMovementInputFactDelegate::CreateLambda(
		[Consumer, Binding, Session, WeakScope, TryAdmitHeldRequest](const FGGYGOMovementInputConsumerBindingId& ReceivedBinding,
			const FGGYGOMovementInputFact& Fact)
		{
			const TSharedPtr<FGGYGOHeroMovementInputScope> Scope = WeakScope.Pin();
			if (ReceivedBinding != Binding || Fact.Request.Session != Session)
			{
				GGYGOHeroMovementInput::Report(Scope, TEXT("FactOriginMismatch"), TEXT("Receiver retained a different original Binding/Session."));
				if (Scope.IsValid() && Scope->Binding == Binding && Scope->Session == Session)
				{
					GGYGOHeroMovementInput::Retire(Scope, TEXT("FactOriginMismatch"));
				}
				return;
			}
			UGGYGOCharacterMovementComponent* CMC = Consumer.Get();
			if (!CMC)
			{
				GGYGOHeroMovementInput::Report(Scope, TEXT("ConsumerUnavailable"), TEXT("Original fact consumer no longer exists."));
				if (Scope.IsValid() && Scope->Binding == Binding && Scope->Session == Session)
				{
					GGYGOHeroMovementInput::Retire(Scope, TEXT("ConsumerUnavailable"));
				}
				return;
			}
			FString ConsumeError;
			const EGGYGOMovementInputConsumeResult Result = CMC->ConsumeMovementInputFact(Binding, Fact, ConsumeError);
			// Recorded with execution failure remains CMC's original FAILED request, awaiting real release.
			if (Scope.IsValid() && Scope->Binding == Binding && Scope->Session == Session)
			{
				if (Fact.Kind == EGGYGOMovementInputFactKind::SessionInvalidated)
				{
					Scope->Session = {};
					Scope->Binding = {};
					GGYGOHeroMovementInput::ReleaseOwnerSyncSubscription(Scope, Fact.Reason);
				}
				else if (Result == EGGYGOMovementInputConsumeResult::Rejected || Result == EGGYGOMovementInputConsumeResult::Stale)
				{
					GGYGOHeroMovementInput::Report(Scope, TEXT("FactConsumeRejected"), ConsumeError);
					GGYGOHeroMovementInput::Retire(Scope, TEXT("FactConsumeRejected"));
				}
				else if (Result == EGGYGOMovementInputConsumeResult::Recorded
					&& Fact.Kind == EGGYGOMovementInputFactKind::RequestStarted)
				{
					if (!ConsumeError.IsEmpty())
					{
						GGYGOHeroMovementInput::Report(Scope, TEXT("StartedExecutionFailed"), ConsumeError);
					}
					else
					{
						TryAdmitHeldRequest(&Fact.Request);
					}
				}
			}
		});
	if (!Source.IsValid() || !Source->AttachMovementInputReceiver(Session, Binding, Receiver, Error))
	{
		GGYGOHeroMovementInput::Report(OriginalScope, TEXT("AttachRejected"),
			Error.IsEmpty() ? TEXT("Original Source became unavailable before Attach.") : Error);
		// Returned resources belong to this stack even if replay already replaced the Hero scope.
		if (OriginalScope->Session == Session && OriginalScope->Binding == Binding)
		{
			GGYGOHeroMovementInput::Retire(OriginalScope, TEXT("AttachRejected"));
		}
		else
		{
			GGYGOHeroMovementInput::ReleaseResources(Source, Session, Consumer, Binding, TEXT("AttachInterrupted"));
		}
	}
}

void UGGYGOHeroComponent::Input_Move(const FInputActionInstance& Instance,
	const TSharedPtr<FGGYGOHeroMovementInputScope>& OriginalScope)
{
	if (!IsMovementInputScopeCurrent(OriginalScope)) { return; }
	if (Instance.GetSourceAction().Get() != OriginalScope->Action.Get())
	{
		GGYGOHeroMovementInput::Report(OriginalScope, TEXT("MoveActionMismatch"), TEXT("Triggered is not the original Move Action."));
		GGYGOHeroMovementInput::Retire(OriginalScope, TEXT("MoveActionMismatch"));
		return;
	}
	const FVector2D Value = Instance.GetValue().Get<FVector2D>();
	if (Value.IsNearlyZero()) { return; } // Value projection only; never release/neutral/request inference.
	const FGGYGOMovementInputSessionIdentity Session = OriginalScope->Session;
	const FGGYGOMovementInputConsumerBindingId Binding = OriginalScope->Binding;
	if (Session.SessionSerial == 0 || Binding.ConsumerBindingSerial == 0 || Binding.SourceSession != Session
		|| OriginalScope->Consumer->GetMovementInputBindingSerial() != Binding.ConsumerBindingSerial)
	{
		GGYGOHeroMovementInput::Report(OriginalScope, TEXT("MovementSessionUnavailable"),
			TEXT("Original Source/CMC assembly is not current; only actual native rebuild or a new input lifecycle can assemble it."));
		return;
	}
	FGGYGOMovementInputRequestIdentity Request;
	FString Error;
	if (!OriginalScope->Source->GetMovementInputRequest(Session, Request, Error))
	{
		GGYGOHeroMovementInput::Report(OriginalScope, TEXT("MovementRequestUnavailable"), Error);
		return;
	}
	if (Request.Session != Session || Request.RequestSerial == 0)
	{
		GGYGOHeroMovementInput::Report(OriginalScope, TEXT("MovementRequestOriginMismatch"), TEXT("Source lookup did not return its original session's allocated request."));
		GGYGOHeroMovementInput::Retire(OriginalScope, TEXT("MovementRequestOriginMismatch"));
		return;
	}
	APawn* Pawn = OriginalScope->Pawn.Get();
	AController* Controller = Pawn ? Pawn->GetController() : nullptr;
	if (!Controller) { return; }
	// Same native camera-relative value projection; Source facts already identify the CMC request.
	const FRotator YawOnlyRotation(0.0f, Controller->GetControlRotation().Yaw, 0.0f);
	const FRotationMatrix RotationBasis(YawOnlyRotation);
	Pawn->AddMovementInput(RotationBasis.GetUnitAxis(EAxis::X) * Value.Y);
	Pawn->AddMovementInput(RotationBasis.GetUnitAxis(EAxis::Y) * Value.X);
}

void UGGYGOHeroComponent::Input_ForceWalkPressed()
{
	if (const ACharacter* Character = GetPawn<ACharacter>())
	{
		if (UGGYGOCharacterMovementComponent* MoveComp = Character->IsLocallyControlled() && PlayerInputSession.IsValid()
			? PlayerInputSession->ForceWalkMovement.Get() : nullptr)
		{
			MoveComp->SetForceWalkRequested(true);
		}
	}
}

void UGGYGOHeroComponent::Input_ForceWalkReleased()
{
	if (UGGYGOCharacterMovementComponent* MoveComp = PlayerInputSession.IsValid() ? PlayerInputSession->ForceWalkMovement.Get() : nullptr)
	{
		MoveComp->SetForceWalkRequested(false);
	}
}

void UGGYGOHeroComponent::Input_LookMouse(const FInputActionValue& InputActionValue)
{
	APawn* Pawn = GetPawn<APawn>();
	if (!Pawn)
	{
		return;
	}

	const FVector2D Value = InputActionValue.Get<FVector2D>();

	// 鼠标增量已经是"移动了多少像素"，与帧长无关，所以不乘 DeltaTime。
	Pawn->AddControllerYawInput(Value.X);
	Pawn->AddControllerPitchInput(Value.Y);
}

void UGGYGOHeroComponent::Input_LookStick(const FInputActionValue& InputActionValue)
{
	APawn* Pawn = GetPawn<APawn>();
	const UWorld* World = GetWorld();
	if (!Pawn || !World)
	{
		return;
	}

	const FVector2D Value = InputActionValue.Get<FVector2D>();

	// 摇杆是持续的偏移量而非增量，必须乘 DeltaTime，否则转视角的速度会随帧率变化。
	const float DeltaSeconds = World->GetDeltaSeconds();

	Pawn->AddControllerYawInput(Value.X * DeltaSeconds);
	Pawn->AddControllerPitchInput(Value.Y * DeltaSeconds);
}

bool UGGYGOHeroComponent::IsAbilityActionBindingCurrent(const TSharedPtr<FAbilityActionBinding>& Binding) const
{
	const TSharedPtr<FPlayerInputSession> Session = Binding ? Binding->Session.Pin() : nullptr;
	return Binding && !Binding->bRetired && HasValidPlayerInputSession() && Session == PlayerInputSession
		&& Binding->SourceAction.IsValid() && Session->ActionBindings.Contains(Binding);
}

TArray<FGGYGOAbilityInputRequestIdentity> UGGYGOHeroComponent::RetireAbilityInputObservations(
	const TArray<TSharedPtr<FAbilityInputObservation>>& Observations)
{
	const TArray<TSharedPtr<FAbilityInputObservation>> OriginalObservations = Observations;
	TArray<FGGYGOAbilityInputRequestIdentity> OriginalIdentities;
	for (const TSharedPtr<FAbilityInputObservation>& Observation : OriginalObservations)
	{
		if (Observation && !Observation->bInvalidated)
		{
			Observation->bInvalidated = true;
			if (Observation->Request.Identity.IsAssigned()) { OriginalIdentities.AddUnique(Observation->Request.Identity); }
		}
	}
	for (const TSharedPtr<FAbilityInputObservation>& Observation : OriginalObservations)
	{
		if (Observation)
		{
			if (const TSharedPtr<FPlayerInputSession> Session = Observation->Session.Pin())
			{
				Session->Observations.Remove(Observation);
			}
		}
	}
	// Active binding latches retain these retired observations until their Action ends.
	return OriginalIdentities;
}

void UGGYGOHeroComponent::InvalidateAbilityInputObservations(
	const TArray<TSharedPtr<FAbilityInputObservation>>& Observations)
{
	EndOriginalAbilityInputRequests(RetireAbilityInputObservations(Observations));
	// No Hero fields are touched after cleanup, which may install a successor.
}

void UGGYGOHeroComponent::InvalidateAbilityActionBinding(const TSharedPtr<FAbilityActionBinding>& Binding)
{
	if (!Binding || Binding->bRetired) { return; }
	Binding->bRetired = true;
	TArray<TSharedPtr<FAbilityInputObservation>> OriginalObservations;
	if (const TSharedPtr<FPlayerInputSession> Session = Binding->Session.Pin())
	{
		for (const TSharedPtr<FAbilityInputObservation>& Observation : Session->Observations)
		{
			if (Observation->Binding.Pin() == Binding) { OriginalObservations.Add(Observation); }
		}
	}
	if (Binding->Observation) { OriginalObservations.AddUnique(Binding->Observation); }
	InvalidateAbilityInputObservations(OriginalObservations);
}

void UGGYGOHeroComponent::Input_AbilityActionTriggered(const FInputActionInstance& ActionInstance,
	const TSharedPtr<FAbilityActionBinding>& Binding)
{
	if (!IsAbilityActionBindingCurrent(Binding))
	{
		InvalidateAbilityActionBinding(Binding);
		return;
	}
	if (ActionInstance.GetSourceAction().Get() != Binding->SourceAction.Get())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("Input/Hero Trigger rejected: Hero=%s Action=%s Tag=%s Reason=SourceActionMismatch"),
			*GetPathName(), *Binding->SourceActionPath, *Binding->InputTag.ToString());
		InvalidateAbilityActionBinding(Binding);
		return;
	}
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : -1.0;
	if (World && FMath::IsFinite(Now)) { PruneEndedAbilityInputObservations(Now); }
	const bool bFirstTrigger = !Binding->Observation;
	if (bFirstTrigger)
	{
		const double Window = static_cast<double>(InputBufferWindow);
		const double Deadline = Now + Window;
		const bool bValidDeadline = World && FMath::IsFinite(Now) && Now >= 0.0
			&& FMath::IsFinite(Window) && Window >= 0.0 && FMath::IsFinite(Deadline);
		const TSharedPtr<FAbilityInputObservation> Observation = MakeShared<FAbilityInputObservation>(
			Binding, bValidDeadline ? Deadline : UGGYGOAbilitySystemComponent::NoAbilityInputRetryDeadline);
		Binding->Observation = Observation;
		Binding->Session.Pin()->Observations.Add(Observation);
		// The original observation is installed even on failure; later Ready/Triggered cannot issue it.
		if (!bValidDeadline)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("Input/Hero Trigger rejected: Hero=%s Action=%s Tag=%s Window=%g Reason=InvalidWorldOrBufferWindow"),
				*GetPathName(), *Binding->SourceActionPath, *Binding->InputTag.ToString(), Window);
			return;
		}
	}
	const TSharedPtr<FAbilityInputObservation> Original = Binding->Observation;
	if (Original->bInvalidated || Original->bActionEnded || (!bFirstTrigger && !Original->Request.Identity.IsAssigned()))
	{
		return;
	}
	const TSharedPtr<FPlayerInputSession> OriginalSession = Binding->Session.Pin();
	const TSharedPtr<FAbilityInputAssociation> OriginalAssociation = OriginalSession->AbilityAssociation;
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> ExpectedASC(GetInputSessionAbilitySystem());
	if (!ExpectedASC.IsValid())
	{
		if (bFirstTrigger)
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("Input/Hero Trigger rejected: Hero=%s Action=%s Tag=%s Reason=ASCNotReady; original observation latched"),
				*GetPathName(), *Binding->SourceActionPath, *Binding->InputTag.ToString());
		}
		else
		{
			InvalidateAbilityInputObservations({Original});
		}
		return;
	}
	if (!bFirstTrigger && (Original->Association.Pin() != OriginalAssociation
		|| !Original->Request.Identity.SourceASC.HasSameIndexAndSerialNumber(ExpectedASC)))
	{
		InvalidateAbilityInputObservations({Original});
		return;
	}
	if (bFirstTrigger) { Original->Association = OriginalAssociation; }
	const FGGYGOAbilityInputRequestIdentity PreviousIdentity = Original->Request.Identity;
	const TWeakObjectPtr<UGGYGOHeroComponent> WeakHero(this);
	const FGGYGOAbilityInputRequestResult Result = ExpectedASC->ReceiveAbilityInputRequest(
		Original->Request.InputTag, PreviousIdentity, Original->Request.OriginalDeadline);
	UGGYGOHeroComponent* OriginalHero = WeakHero.Get();
	if (!OriginalHero || !OriginalHero->IsAbilityActionBindingCurrent(Binding)
		|| Binding->Observation != Original || Original->bInvalidated || Original->bActionEnded
		|| !ExpectedASC.IsValid() || OriginalHero->PlayerInputSession != OriginalSession
		|| OriginalSession->AbilityAssociation != OriginalAssociation
		|| !OriginalHero->IsAbilityInputAssociationCurrent(OriginalAssociation))
	{
		if (OriginalHero) { OriginalHero->InvalidateAbilityInputObservations({Original}); }
		else
		{
			Original->bInvalidated = true;
			if (UGGYGOAbilitySystemComponent* OriginalASC = PreviousIdentity.SourceASC.Get())
			{
				OriginalASC->EndAbilityInputRequest(PreviousIdentity, EGGYGOAbilityInputRequestEndKind::Invalidated);
			}
		}
		// A first Receive may return an ID after its association was already retired.
		if (bFirstTrigger && Result.Identity.IsAssigned()
			&& Result.Identity.SourceASC.HasSameIndexAndSerialNumber(ExpectedASC))
		{
			if (UGGYGOAbilitySystemComponent* OriginalASC = ExpectedASC.Get())
			{
				OriginalASC->EndAbilityInputRequest(Result.Identity, EGGYGOAbilityInputRequestEndKind::Invalidated);
			}
		}
		return;
	}
	const bool bAccepted = Result.Outcome == EGGYGOAbilityInputRequestOutcome::Accepted
		|| Result.Outcome == EGGYGOAbilityInputRequestOutcome::AlreadyApplied;
	if (bFirstTrigger)
	{
		if (bAccepted && Result.Identity.IsAssigned()
			&& Result.Identity.SourceASC.HasSameIndexAndSerialNumber(ExpectedASC))
		{
			Original->Request.Identity = Result.Identity;
		}
		// Rejection leaves the original empty ID latched, never retries with an empty Previous.
	}
	else if (!bAccepted || Result.Identity != PreviousIdentity)
	{
		OriginalHero->InvalidateAbilityInputObservations({Original});
	}
}

void UGGYGOHeroComponent::Input_AbilityActionReleased(const FInputActionInstance& ActionInstance,
	const TSharedPtr<FAbilityActionBinding>& Binding)
{
	if (!IsAbilityActionBindingCurrent(Binding))
	{
		InvalidateAbilityActionBinding(Binding);
		return;
	}
	if (ActionInstance.GetSourceAction().Get() != Binding->SourceAction.Get())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("Input/Hero Action end rejected: Hero=%s Action=%s Tag=%s Reason=SourceActionMismatch"),
			*GetPathName(), *Binding->SourceActionPath, *Binding->InputTag.ToString());
		InvalidateAbilityActionBinding(Binding);
		return;
	}
	const TSharedPtr<FAbilityInputObservation> Original = Binding->Observation;
	if (!Original) { return; }
	Binding->Observation.Reset();
	Original->bActionEnded = true;
	if (Original->bInvalidated || !Original->Request.Identity.IsAssigned())
	{
		if (const TSharedPtr<FPlayerInputSession> Session = Original->Session.Pin()) { Session->Observations.Remove(Original); }
		return;
	}
	const FGGYGOAbilityInputRequestIdentity Identity = Original->Request.Identity;
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC = Identity.SourceASC;
	if (!OriginalASC.IsValid() || !PlayerInputSession.IsValid()
		|| Original->Association.Pin() != PlayerInputSession->AbilityAssociation
		|| GetInputSessionAbilitySystem() != OriginalASC.Get())
	{
		InvalidateAbilityInputObservations({Original});
		return;
	}
	const TWeakObjectPtr<UGGYGOHeroComponent> WeakHero(this);
	const FGGYGOAbilityInputRequestResult Result = OriginalASC->EndAbilityInputRequest(
		Identity, EGGYGOAbilityInputRequestEndKind::Released);
	if (Result.Outcome != EGGYGOAbilityInputRequestOutcome::Accepted
		&& Result.Outcome != EGGYGOAbilityInputRequestOutcome::AlreadyApplied)
	{
		// Failed release remains a failure; exact retirement only returns this association's resource.
		if (UGGYGOHeroComponent* OriginalHero = WeakHero.Get())
		{
			OriginalHero->InvalidateAbilityInputObservations({Original});
		}
		else
		{
			Original->bInvalidated = true;
			if (UGGYGOAbilitySystemComponent* ASC = OriginalASC.Get())
			{
				ASC->EndAbilityInputRequest(Identity, EGGYGOAbilityInputRequestEndKind::Invalidated);
			}
		}
	}
}

bool UGGYGOHeroComponent::HasValidPlayerInputSession() const
{
	const TSharedPtr<FPlayerInputSession> Session = PlayerInputSession;
	if (!Session.IsValid() || Session->bRetired || Session->Generation != InputSessionGeneration) { return false; }
	const APawn* Pawn = GetPawn<APawn>();
	const APlayerController* PC = GetController<APlayerController>();
	const ULocalPlayer* LocalPlayer = PC ? PC->GetLocalPlayer() : nullptr;
	const UEnhancedInputLocalPlayerSubsystem* Subsystem = Session->Subsystem.Get();
	return !bEndingPlay && Pawn && Pawn->IsLocallyControlled() && LocalPlayer
		&& Session->Component.IsValid() && Pawn->InputComponent == Session->Component.Get()
		&& Subsystem && Session->PlayerInput.IsValid()
		&& LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() == Subsystem
		&& Subsystem->GetPlayerInput() == Session->PlayerInput.Get();
}

bool UGGYGOHeroComponent::IsAbilityInputAssociationCurrent(const TSharedPtr<FAbilityInputAssociation>& Association) const
{
	if (!Association.IsValid() || Association->bRetired || !HasValidPlayerInputSession()) { return false; }
	const TSharedPtr<FPlayerInputSession> Session = Association->Session.Pin();
	const TSharedPtr<FLocalAbilitySystemSubscription> Subscription = Association->Subscription.Pin();
	if (Session != PlayerInputSession || Session->AbilityAssociation != Association
		|| !Subscription.IsValid() || LocalAbilitySystemSubscription != Subscription || Subscription->bRetired
		|| !Subscription->Resource.HasSameResource(Association->Resource)
		|| Subscription->AssociatedInputSession.Pin() != Session)
	{
		return false;
	}
	const FGGYGOPawnASCResourceIdentity Identity = Association->Resource.GetIdentity();
	return Identity.ASC.IsValid() && GetReadyLocalAbilitySystemComponent() == Identity.ASC.Get();
}

UGGYGOAbilitySystemComponent* UGGYGOHeroComponent::GetInputSessionAbilitySystem() const
{
	const TSharedPtr<FAbilityInputAssociation> Association =
		PlayerInputSession.IsValid() ? PlayerInputSession->AbilityAssociation : nullptr;
	return IsAbilityInputAssociationCurrent(Association) ? Association->Resource.GetIdentity().ASC.Get() : nullptr;
}

TArray<FGGYGOAbilityInputRequestIdentity> UGGYGOHeroComponent::RetireAbilityInputAssociation(
	const TSharedPtr<FPlayerInputSession>& Session)
{
	if (!Session.IsValid()) { return {}; }
	const TSharedPtr<FAbilityInputAssociation> Association = MoveTemp(Session->AbilityAssociation);
	if (!Association.IsValid()) { return {}; }
	Association->bRetired = true;
	// Closing retires only this H association. Keep the subscription's original native
	// session provenance until Released or explicit native session retirement returns it.
	TArray<TSharedPtr<FAbilityInputObservation>> OriginalObservations;
	for (const TSharedPtr<FAbilityInputObservation>& Observation : Session->Observations)
	{
		if (Observation->Association.Pin() == Association) { OriginalObservations.Add(Observation); }
	}
	return RetireAbilityInputObservations(OriginalObservations);
}

void UGGYGOHeroComponent::AssociateReadyAbilitySystemWithInput()
{
	if (!HasValidPlayerInputSession()) { return; }
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = LocalAbilitySystemSubscription;
	if (!OriginalSubscription.IsValid() || !GetReadyLocalAbilitySystemComponent()) { return; }
	const TSharedPtr<FPlayerInputSession> OriginalSession = PlayerInputSession;
	const FGGYGOPawnASCResourceHandle OriginalResource = OriginalSubscription->Resource;
	const FGGYGOPawnASCResourceIdentity Identity = OriginalResource.GetIdentity();
	const TSharedPtr<FPlayerInputSession> AssociatedSession = OriginalSubscription->AssociatedInputSession.Pin();
	if (AssociatedSession.IsValid() && AssociatedSession != OriginalSession)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Input/Hero] Association rejected: Hero='%s' Extension='%s' ASC='%s' InputGeneration=%llu Reason=OriginalResourceNamesDifferentLiveInputSession."),
			*GetPathNameSafe(this), *GetPathNameSafe(OriginalSubscription->Extension.Get()), *GetPathNameSafe(Identity.ASC.Get()),
			static_cast<unsigned long long>(OriginalSession->Generation));
		return;
	}
	const TSharedPtr<FAbilityInputAssociation> Association = OriginalSession->AbilityAssociation;
	if (Association.IsValid() && !Association->bRetired && Association->Session.Pin() == OriginalSession
		&& Association->Subscription.Pin() == OriginalSubscription && Association->Resource.HasSameResource(OriginalResource)
		&& AssociatedSession == OriginalSession)
	{
		return;
	}
	const TArray<FGGYGOAbilityInputRequestIdentity> OriginalIdentities = RetireAbilityInputAssociation(OriginalSession);
	if (!OriginalIdentities.IsEmpty())
	{
		const TWeakObjectPtr<UGGYGOHeroComponent> WeakHero(this);
		EndOriginalAbilityInputRequests(OriginalIdentities);
		UGGYGOHeroComponent* Hero = WeakHero.Get();
		// Exact End may call out through failure diagnostics; only this original H/session can continue.
		if (!Hero || Hero->PlayerInputSession != OriginalSession || OriginalSession->bRetired
			|| OriginalSession->AbilityAssociation.IsValid()
			|| Hero->LocalAbilitySystemSubscription != OriginalSubscription || OriginalSubscription->bRetired
			|| !OriginalSubscription->Resource.HasSameResource(OriginalResource)
			|| !Hero->HasValidPlayerInputSession() || Hero->GetReadyLocalAbilitySystemComponent() != Identity.ASC.Get())
		{
			return;
		}
	}
	OriginalSession->AbilityAssociation = MakeShared<FAbilityInputAssociation>(
		OriginalSubscription, OriginalResource, OriginalSession);
	OriginalSubscription->AssociatedInputSession = OriginalSession;
}

void UGGYGOHeroComponent::PruneEndedAbilityInputObservations(double Now)
{
	if (!PlayerInputSession.IsValid()) { return; }
	PlayerInputSession->Observations.RemoveAll([Now](const TSharedPtr<FAbilityInputObservation>& Observation)
	{
		return Observation->bInvalidated || (Observation->bActionEnded
			&& (!Observation->Request.Identity.IsAssigned()
				|| !FMath::IsFinite(Observation->Request.OriginalDeadline) || Now >= Observation->Request.OriginalDeadline));
	});
	// Active Action latches survive deadline expiry. ASC alone prunes finite retry work.
}

bool UGGYGOHeroComponent::PrepareLocalAbilitySystemSubscription(
	UGGYGOPawnExtensionComponent* Extension, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	const FString ErrorContext = FString::Printf(
		TEXT("[Input/Hero] LocalASC prepare: Hero='%s', Owner='%s', Extension='%s'"),
		*GetPathNameSafe(this), *GetPathNameSafe(GetOwner()), *GetPathNameSafe(Extension));
	const auto Reject = [&OutError, &ErrorContext](const TCHAR* Reason)
	{
		OutError = FString::Printf(TEXT("%s, Reason='%s'."), *ErrorContext, Reason);
		return false;
	};
	const TWeakObjectPtr<UGGYGOHeroComponent> OriginalHero(this);
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(Extension);
	const TWeakObjectPtr<APawn> OriginalPawn(Cast<APawn>(GetOwner()));
	const auto IsOriginalTargetLive = [&]()
	{
		const UGGYGOHeroComponent* Hero = OriginalHero.Get();
		const UGGYGOPawnExtensionComponent* Source = OriginalExtension.Get();
		const APawn* Pawn = OriginalPawn.Get();
		return Hero && !Hero->bEndingPlay && !Hero->IsBeingDestroyed()
			&& !Hero->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& Source && !Source->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& Pawn && !Pawn->IsActorBeingDestroyed()
			&& Hero->GetOwner() == Pawn && Source->GetOwner() == Pawn;
	};
	if (!IsOriginalTargetLive())
	{
		return Reject(TEXT("requires a live Hero and its original Pawn Extension"));
	}
	if (LocalAbilitySystemSubscription.IsValid() && !LocalAbilitySystemSubscription->bRetired
		&& LocalAbilitySystemSubscription->Extension.HasSameIndexAndSerialNumber(OriginalExtension)
		&& LocalAbilitySystemSubscription->Pawn.HasSameIndexAndSerialNumber(OriginalPawn))
	{
		return true; // Original registration only, never a claim of Ready.
	}
	ReleaseLocalAbilitySystemSubscription();
	UGGYGOHeroComponent* Hero = OriginalHero.Get();
	if (!IsOriginalTargetLive() || !Hero || Hero->LocalAbilitySystemSubscription.IsValid())
	{
		return Reject(TEXT("original target or subscription changed while retiring the old record"));
	}

	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription =
		MakeShared<FLocalAbilitySystemSubscription>(OriginalExtension.Get(), OriginalPawn.Get());
	Hero->LocalAbilitySystemSubscription = OriginalSubscription;
	const TWeakPtr<FLocalAbilitySystemSubscription> WeakSubscription(OriginalSubscription);
	const FDelegateHandle ReturnedHandle = OriginalExtension.Get()->RegisterLocalAbilitySystemNoticeAndCall(
		FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateWeakLambda(Hero,
			[OriginalHero, WeakSubscription](const FGGYGOPawnASCLocalNotice& Notice)
			{
				const TSharedPtr<FLocalAbilitySystemSubscription> Subscription = WeakSubscription.Pin();
				if (UGGYGOHeroComponent* LiveHero = OriginalHero.Get())
				{
					if (Subscription.IsValid())
					{
						LiveHero->ConsumeLocalAbilitySystemNotice(Subscription, Notice);
					}
				}
			}));
	// The original stack record owns this return even if synchronous Ready retired/replaced it.
	OriginalSubscription->AcceptReturnedHandle(ReturnedHandle);
	Hero = OriginalHero.Get();
	if (!ReturnedHandle.IsValid() || !IsOriginalTargetLive() || !Hero
		|| Hero->LocalAbilitySystemSubscription != OriginalSubscription || OriginalSubscription->bRetired)
	{
		if (Hero && Hero->LocalAbilitySystemSubscription == OriginalSubscription)
		{
			Hero->ReleaseLocalAbilitySystemSubscription();
		}
		else
		{
			OriginalSubscription->Retire();
		}
		return Reject(ReturnedHandle.IsValid()
			? TEXT("original record changed during immediate replay; only its returned handle was retired")
			: TEXT("Extension returned no valid notice handle"));
	}
	return true; // A valid subscription can still have no Ready resource.
}

void UGGYGOHeroComponent::ReleaseLocalAbilitySystemSubscription()
{
	check(IsInGameThread());
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = MoveTemp(LocalAbilitySystemSubscription);
	if (!OriginalSubscription.IsValid()) { return; }
	const TSharedPtr<FPlayerInputSession> Session = OriginalSubscription->AssociatedInputSession.Pin();
	TArray<FGGYGOAbilityInputRequestIdentity> OriginalIdentities;
	if (Session.IsValid() && Session->AbilityAssociation.IsValid()
		&& Session->AbilityAssociation->Subscription.Pin() == OriginalSubscription)
	{
		OriginalIdentities = RetireAbilityInputAssociation(Session);
	}
	OriginalSubscription->Retire();
	EndOriginalAbilityInputRequests(OriginalIdentities);
	// Exact cleanup uses captured records only, even if notice removal installed a successor.
}

void UGGYGOHeroComponent::ConsumeLocalAbilitySystemNotice(
	const TSharedPtr<FLocalAbilitySystemSubscription>& ExpectedSubscription,
	const FGGYGOPawnASCLocalNotice& Notice)
{
	check(IsInGameThread());
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = ExpectedSubscription;
	if (!OriginalSubscription.IsValid() || OriginalSubscription->bRetired
		|| LocalAbilitySystemSubscription != OriginalSubscription) { return; }
	const FGGYGOPawnASCLocalNotice OriginalNotice = Notice;
	const FGGYGOPawnASCResourceHandle OriginalResource = OriginalNotice.Resource;
	const FGGYGOPawnASCResourceIdentity Identity = OriginalResource.GetIdentity();
	const auto Reject = [&](const TCHAR* Reason)
	{
		UE_LOG(LogGGYGOAbilitySystem, Verbose,
			TEXT("[Input/Hero] LocalASC notice rejected: Hero='%s', Extension='%s', Pawn='%s', ASC='%s', Kind=%u, Reason='%s'."),
			*GetPathNameSafe(this), *GetPathNameSafe(OriginalSubscription->Extension.Get()),
			*GetPathNameSafe(Identity.Pawn.Get()), *GetPathNameSafe(Identity.ASC.Get()),
			static_cast<uint32>(OriginalNotice.Kind), Reason);
	};
	if (!OriginalResource.HasResource())
	{
		Reject(TEXT("notice has no original opaque resource"));
		return;
	}
	if (OriginalNotice.Kind == EGGYGOPawnASCLocalNoticeKind::Closing)
	{
		if (!OriginalSubscription->Resource.HasSameResource(OriginalResource))
		{
			Reject(TEXT("Closing does not name the consumed original resource; successor is retained"));
			return;
		}
		const TSharedPtr<FPlayerInputSession> OriginalSession = OriginalSubscription->AssociatedInputSession.Pin();
		const TSharedPtr<FAbilityInputAssociation> OriginalAssociation =
			OriginalSession.IsValid() ? OriginalSession->AbilityAssociation : nullptr;
		if (OriginalAssociation.IsValid() && OriginalAssociation->Subscription.Pin() == OriginalSubscription
			&& OriginalAssociation->Resource.HasSameResource(OriginalResource))
		{
			// Original H loss of admission needs no Ready/ASC/Pawn qualification. Seal
			// local observations first; exact cleanup cannot touch a reentrant successor.
			EndOriginalAbilityInputRequests(RetireAbilityInputAssociation(OriginalSession));
		}
		// Native bindings and the active Action's first observation survive this H boundary.
		return;
	}
	if (OriginalNotice.Kind == EGGYGOPawnASCLocalNoticeKind::Released)
	{
		// Exact withdrawal may outlive Pawn/ASC validity; capture cleanup provenance before revoking it.
		if (!OriginalSubscription->Resource.HasSameResource(OriginalResource))
		{
			Reject(TEXT("Released does not name the consumed original resource; successor is retained"));
			return;
		}
		const TSharedPtr<FPlayerInputSession> AssociatedSession = OriginalSubscription->AssociatedInputSession.Pin();
		const TSharedPtr<FPlayerInputSession> OriginalSession = PlayerInputSession;
		const TSharedPtr<FAbilityInputAssociation> OriginalAssociation =
			OriginalSession.IsValid() ? OriginalSession->AbilityAssociation : nullptr;
		const bool bOwnsInput = AssociatedSession.IsValid() && AssociatedSession == OriginalSession;
		const bool bOwnsAssociation = OriginalAssociation.IsValid()
			&& OriginalAssociation->Subscription.Pin() == OriginalSubscription
			&& OriginalAssociation->Resource.HasSameResource(OriginalResource);
		const uint64 OriginalInputGeneration = InputSessionGeneration;
		const uint64 ExpectedInputGeneration = bOwnsInput && OriginalInputGeneration != MAX_uint64
			? OriginalInputGeneration + 1 : OriginalInputGeneration;
		const TWeakObjectPtr<UGGYGOHeroComponent> WeakHero(this);
		const uint64 OriginalCameraGeneration = LastAbilityCameraModeRequestGeneration;
		const TWeakObjectPtr<APawn> OriginalPawn = OriginalSubscription->Pawn;
		const TWeakObjectPtr<UGGYGOCameraComponent> OriginalCamera =
			UGGYGOCameraComponent::FindCameraComponent(OriginalPawn.Get());
		OriginalSubscription->Resource = FGGYGOPawnASCResourceHandle{};
		OriginalSubscription->AssociatedInputSession.Reset();
		if (bOwnsInput)
		{
			ReleasePlayerInput();
		}
		else if (bOwnsAssociation)
		{
			EndOriginalAbilityInputRequests(RetireAbilityInputAssociation(OriginalSession));
		}
		// Camera withdrawal follows this matching H. Input cleanup is external, so a
		// new H, native session or camera request must stop this original withdrawal.
		UGGYGOHeroComponent* Hero = WeakHero.Get();
		if (!Hero || Hero->bEndingPlay || Hero->IsBeingDestroyed()
			|| Hero->LocalAbilitySystemSubscription != OriginalSubscription || OriginalSubscription->bRetired
			|| OriginalSubscription->Resource.HasResource()
			|| Hero->InputSessionGeneration != ExpectedInputGeneration
			|| Hero->LastAbilityCameraModeRequestGeneration != OriginalCameraGeneration
			|| (bOwnsInput ? Hero->PlayerInputSession.IsValid() : Hero->PlayerInputSession != OriginalSession))
		{
			return;
		}
		Hero->AbilityCameraModeOverrides.Reset();
		if (UGGYGOCameraComponent* Camera = OriginalCamera.Get())
		{
			if (OriginalPawn.IsValid() && Camera->GetOwner() == OriginalPawn.Get())
			{
				Camera->ResetCameraRuntimeState();
			}
		}
		return;
	}
	if (OriginalNotice.Kind != EGGYGOPawnASCLocalNoticeKind::Ready
		&& OriginalNotice.Kind != EGGYGOPawnASCLocalNoticeKind::Refreshed)
	{
		Reject(TEXT("kind is not Ready, Released, Refreshed or Closing"));
		return;
	}
	UGGYGOPawnExtensionComponent* Source = OriginalSubscription->Extension.Get();
	APawn* Pawn = OriginalSubscription->Pawn.Get();
	UGGYGOAbilitySystemComponent* ASC = Identity.ASC.Get();
	if (bEndingPlay || IsBeingDestroyed() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !Source || Source->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !Pawn || Pawn->IsActorBeingDestroyed() || GetOwner() != Pawn || Source->GetOwner() != Pawn
		|| !Identity.Pawn.HasSameIndexAndSerialNumber(OriginalSubscription->Pawn) || !ASC
		|| !Source->IsLocalAbilitySystemResourceReady(OriginalResource)
		|| !Identity.Binding.HasSameIdentity(OriginalNotice.PublishedContext.Binding)
		|| !ASC->IsAvatarBindingPublicationContextCurrent(OriginalNotice.PublishedContext))
	{
		Reject(TEXT("original owner/resource or actual publication Context is no longer Ready"));
		return;
	}
	if (OriginalSubscription->bRetired || LocalAbilitySystemSubscription != OriginalSubscription) { return; }
	const bool bSameResource = OriginalSubscription->Resource.HasSameResource(OriginalResource);
	if (OriginalSubscription->Resource.HasResource() && !bSameResource)
	{
		Reject(TEXT("different opaque resource cannot be adopted, even with the same ASC/Pawn"));
		return;
	}
	if (OriginalNotice.Kind == EGGYGOPawnASCLocalNoticeKind::Refreshed)
	{
		if (!bSameResource)
		{
			Reject(TEXT("Refreshed requires the already consumed original opaque resource"));
		}
		return; // No input association, generation, binding or mapping changes.
	}
	if (!bSameResource) { OriginalSubscription->Resource = OriginalResource; }
	// Ready never builds input. Associate only if the existing input session is live.
	AssociateReadyAbilitySystemWithInput();
}

UGGYGOAbilitySystemComponent* UGGYGOHeroComponent::GetReadyLocalAbilitySystemComponent() const
{
	check(IsInGameThread());
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = LocalAbilitySystemSubscription;
	if (!OriginalSubscription.IsValid() || OriginalSubscription->bRetired
		|| bEndingPlay || IsBeingDestroyed() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return nullptr;
	}
	const FGGYGOPawnASCResourceHandle OriginalResource = OriginalSubscription->Resource;
	const FGGYGOPawnASCResourceIdentity Identity = OriginalResource.GetIdentity();
	const UGGYGOPawnExtensionComponent* Source = OriginalSubscription->Extension.Get();
	const APawn* Pawn = OriginalSubscription->Pawn.Get();
	UGGYGOAbilitySystemComponent* ASC = Identity.ASC.Get();
	if (!OriginalResource.HasResource() || !Source
		|| Source->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !Pawn || Pawn->IsActorBeingDestroyed() || GetOwner() != Pawn || Source->GetOwner() != Pawn
		|| !Identity.Pawn.HasSameIndexAndSerialNumber(OriginalSubscription->Pawn) || !ASC
		|| !Source->IsLocalAbilitySystemResourceReady(OriginalResource))
	{
		return nullptr;
	}
	return LocalAbilitySystemSubscription == OriginalSubscription && !OriginalSubscription->bRetired
		&& OriginalSubscription->Resource.HasSameResource(OriginalResource) ? ASC : nullptr;
}
