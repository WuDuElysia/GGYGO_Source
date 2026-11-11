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
#include "Player/GGYGOLocalPlayer.h"
#include "System/GGYGOGameplayTags.h"

#include "Components/GameFrameworkComponentManager.h"
#include "CoreGlobals.h"
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
	FPlayerInputSession(UGGYGOInputComponent* InComponent, UGGYGOLocalPlayer* InLocalPlayer,
		UEnhancedInputLocalPlayerSubsystem* InSubsystem,
		UEnhancedPlayerInput* InPlayerInput, UGGYGOCharacterMovementComponent* InForceWalkMovement, uint64 InGeneration)
		: Component(InComponent), LocalPlayer(InLocalPlayer), Subsystem(InSubsystem), PlayerInput(InPlayerInput),
		  ForceWalkMovement(InForceWalkMovement), Generation(InGeneration)
	{}

	struct FRegisteredMapping
	{
		TWeakObjectPtr<const UInputMappingContext> MappingContext;
		int32 RegisteredPriority = 0;
	};

	const TWeakObjectPtr<UGGYGOInputComponent> Component;
	const TWeakObjectPtr<UGGYGOLocalPlayer> LocalPlayer;
	const TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> Subsystem;
	const TWeakObjectPtr<UEnhancedPlayerInput> PlayerInput;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> ForceWalkMovement;
	const uint64 Generation;
	FDelegateHandle MappingRemovalHandle;
	TArray<uint32> BindHandles;
	TArray<FRegisteredMapping> Mappings;
	TArray<TSharedPtr<FAbilityActionBinding>> ActionBindings;
	/** 本会话当前关联的 ASC 绑定资源；未关联时为空。资源变化或会话退役时，
	 *  按 SourceKey 让原 ASC 一次结束本会话签发的全部请求（含已释放、仍在缓冲期的请求）。 */
	FGGYGOPawnASCResourceHandle AssociatedResource;
	bool bRetired = false;

	FGGYGOAbilityInputSourceKey MakeSourceKey(const UGGYGOHeroComponent* Hero) const
	{
		return FGGYGOAbilityInputSourceKey{Hero, Generation};
	}
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
	/** 本次按住已开始（首个 Triggered 到 Completed/Canceled）；按住期间不产生新请求。 */
	bool bPressed = false;
	/** 本次按下的请求：Tag、截止与移动评价 Token 在首个 Triggered 固定；Identity 只由首次 Receive 写入。
	 *  按住期间 Identity 未分配（首次被拒、ASC 未就绪或关联已变化）时，本次按住不再请求。 */
	FGGYGOAbilityInputRetryRequest CurrentRequest;
	/** 已诊断过的 Hero 侧拒绝原因；每个动作每种原因只报一次。 */
	TSet<FName> ReportedReasons;
};


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
	// 只观察现有边界；不登记状态、不请求重建，也不驱动 Source／CMC 装配。
	// OwnedMappings 是本次归还凭据数；MappingApplied 不读取引擎内部注册计数。
	static void TraceLifecycle(const TSharedPtr<FGGYGOHeroMovementInputScope>& Scope, const TCHAR* Phase,
		int32 OwnedMappings = INDEX_NONE, const UInputMappingContext* Mapping = nullptr, FName Reason = NAME_None)
	{
		if (!Scope.IsValid() || !UE_LOG_ACTIVE(LogGGYGOAbilitySystem, Verbose)) { return; }
		UEnhancedInputLocalPlayerSubsystem* Subsystem = Scope->Subsystem.Get();
		const int32 MappingApplied = Mapping && Subsystem ? static_cast<int32>(Subsystem->HasMappingContext(Mapping)) : INDEX_NONE;
		UE_LOG(LogGGYGOAbilitySystem, Verbose,
			TEXT("[Input/Hero MovementLifecycle] Frame=%llu Phase=%s %s Retired=%d OwnedMappings=%d ExpectedOwnedMappings=%d Session=%llu Binding=%llu Mapping='%s' MappingApplied=%d Reason=%s"),
			static_cast<unsigned long long>(GFrameCounter), Phase, *Scope->DiagnosticContext, Scope->bRetired,
			OwnedMappings, Scope->ExpectedMappingRegistrationCount,
			static_cast<unsigned long long>(Scope->Session.SessionSerial),
			static_cast<unsigned long long>(Scope->Binding.ConsumerBindingSerial),
			*GetPathNameSafe(Mapping), MappingApplied, *Reason.ToString());
	}

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
		TraceLifecycle(Scope, TEXT("AssemblyResourcesReleased"), INDEX_NONE, nullptr, Reason);
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
	UE_LOG(LogGGYGOAbilitySystem, Verbose,
		TEXT("[Input/Hero MovementLifecycle] Frame=%llu Phase=NativeRebuildNotification Observer='%s' Hero='%s' Subsystem='%s' ScopeAlive=%d"),
		static_cast<unsigned long long>(GFrameCounter), *GetPathNameSafe(this),
		*GetPathNameSafe(Hero.Get()), *GetPathNameSafe(OriginalSubsystem.Get()), Scope.IsValid());
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
	// 本地 Controller 附身时，原生 OnPossess 先调 Pawn::PossessedBy（在这里推进到 DataInitialized），
	// 之后才 SetPawn；此刻 Controller 还没指向本 Pawn，不是关联错误。同一次附身随后的
	// PawnClientRestart → InitializePlayerInput 会再次进入这里完成镜头 Ready。
	if (FCString::Strcmp(NativeEntry, TEXT("DataInitialized")) == 0 && PC->GetPawn() != ExpectedPawn) { return true; }
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
	if (!PC || PC->IsActorBeingDestroyed())
	{
		return;
	}

	ULocalPlayer* LocalPlayer = PC->GetLocalPlayer();
	if (!LocalPlayer)
	{
		return;
	}
	UGGYGOLocalPlayer* LP = Cast<UGGYGOLocalPlayer>(LocalPlayer);
	if (!LP)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializePlayerInput: [%s] 的 LocalPlayer [%s] 缺少项目输入映射退役入口。"),
			*GetPathNameSafe(Pawn), *GetPathNameSafe(LocalPlayer));
		return;
	}
	if (LP->IsInputMappingRemovalStarted()) { return; }

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
		GGYGOIC, LP, Subsystem, EnhancedPlayerInput, ForceWalkAction ? MovementConsumer : nullptr, SessionGeneration);
	PlayerInputSession = OriginalSession;
	// 在首次注册 IMC 前订阅原 LocalPlayer 的退役通知。Controller 销毁时只归还其原 PlayerInput 的计数。
	const TWeakPtr<FPlayerInputSession> WeakSession(OriginalSession);
	OriginalSession->MappingRemovalHandle = LP->OnInputMappingsRemoving().AddWeakLambda(this, [this, WeakSession](UEnhancedPlayerInput* RemovingInput)
	{
		const TSharedPtr<FPlayerInputSession> Session = WeakSession.Pin();
		if (Session.IsValid() && PlayerInputSession == Session
			&& (!RemovingInput || Session->PlayerInput.Get() == RemovingInput))
		{
			ReleasePlayerInput();
		}
	});

	const TSharedPtr<FGGYGOHeroMovementInputScope> OriginalMovementScope = MakeShared<FGGYGOHeroMovementInputScope>(
		this, GetPawn<APawn>(), GGYGOIC, MovementSource, Subsystem, MovementConsumer, MoveAction,
		SessionGeneration, MappingsToRegister.Num(), InputConfig);
	MovementInputScope = OriginalMovementScope;
	MovementMappingObserver = NewObject<UGGYGOHeroMovementMappingObserver>(this);
	OriginalMovementScope->Observer = MovementMappingObserver.Get();
	// Install the original registration before Add. Only actual native rebuilt notification begins Source.
	MovementMappingObserver->Initialize(this, Subsystem, OriginalMovementScope);
	GGYGOHeroMovementInput::TraceLifecycle(OriginalMovementScope, TEXT("ObserverInstalled"), OriginalSession->Mappings.Num());
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
		GGYGOHeroMovementInput::TraceLifecycle(OriginalMovementScope, TEXT("BeforeMappingAdd"), OriginalSession->Mappings.Num(), Mapping);
		Subsystem->AddMappingContext(Mapping, RegistrationPriority);
		GGYGOHeroMovementInput::TraceLifecycle(OriginalMovementScope, TEXT("AfterMappingAdd"), OriginalSession->Mappings.Num(), WeakMapping.Get());
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
	// 已有 CountRegistrations 项的 Add 只增加引用；完整登记后统一请求本代正常延迟编译。
	// 保留默认按键忽略规则，由引擎真实通知继续唯一的 Source／CMC 装配。
	Subsystem->RequestRebuildControlMappings();
	GGYGOHeroMovementInput::TraceLifecycle(OriginalMovementScope, TEXT("RebuildRequested"), OriginalSession->Mappings.Num());
	// Input may precede Ready; the exact typed Ready notice later associates this existing session.
	GGYGOHeroMovementInput::TraceLifecycle(OriginalMovementScope, TEXT("InstallationComplete"), OriginalSession->Mappings.Num());
	AssociateReadyAbilitySystemWithInput();
}

void UGGYGOHeroComponent::ReleasePlayerInput()
{
	const TSharedPtr<FPlayerInputSession> PreviousSession = MoveTemp(PlayerInputSession);
	if (InputSessionGeneration != MAX_uint64) { ++InputSessionGeneration; }
	if (PreviousSession.IsValid())
	{
		PreviousSession->bRetired = true;
		if (UGGYGOLocalPlayer* LocalPlayer = PreviousSession->LocalPlayer.Get())
		{
			LocalPlayer->OnInputMappingsRemoving().Remove(PreviousSession->MappingRemovalHandle);
		}
		for (const TSharedPtr<FAbilityActionBinding>& Binding : PreviousSession->ActionBindings)
		{
			Binding->bRetired = true;
		}
	}
	// 先结束本会话签发的全部 ASC 请求，再做可能外调的 Source/CMC 清理。
	RetireSessionAbilityRequests(PreviousSession);
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
	if (!IsMovementInputScopeCurrent(OriginalScope))
	{
		GGYGOHeroMovementInput::TraceLifecycle(OriginalScope, TEXT("RebuildScopeNotCurrent"),
			PlayerInputSession.IsValid() ? PlayerInputSession->Mappings.Num() : INDEX_NONE);
		return;
	}
	// A real rebuild can occur inside an Added callback; wait for all of this scope's actual Add registrations.
	if (PlayerInputSession->Mappings.Num() != OriginalScope->ExpectedMappingRegistrationCount)
	{
		GGYGOHeroMovementInput::TraceLifecycle(OriginalScope, TEXT("RebuildMappingsPending"), PlayerInputSession->Mappings.Num());
		return;
	}
	const TWeakObjectPtr<UGGYGOHeroComponent> OriginalHero(this);
	GGYGOHeroMovementInput::ReleaseResources(OriginalScope, TEXT("NativeMappingsRebuilt"));
	UGGYGOHeroComponent* Hero = OriginalHero.Get();
	if (!Hero || !Hero->IsMovementInputScopeCurrent(OriginalScope)
		|| OriginalScope->Session.SessionSerial != 0 || OriginalScope->Binding.ConsumerBindingSerial != 0)
	{
		GGYGOHeroMovementInput::TraceLifecycle(OriginalScope, TEXT("RebuildCleanupChangedAssembly"));
		return;
	}
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
	GGYGOHeroMovementInput::TraceLifecycle(OriginalScope, TEXT("SourceBeginAccepted"));
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
	GGYGOHeroMovementInput::TraceLifecycle(OriginalScope, TEXT("ConsumerBindAccepted"));
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
		// 只有已证明的 Held 请求可申请首次准入；NotHeld／Awaiting／Unavailable 都不授权，也不在此诊断。
		if (Source->QueryMovementInputRequest(Session, Request, AdmissionError)
			!= EGGYGOMovementInputRequestQueryResult::Held) { return; }
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
					GGYGOHeroMovementInput::TraceLifecycle(Scope, TEXT("FactSessionInvalidated"), INDEX_NONE, nullptr, Fact.Reason);
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
	else
	{
		GGYGOHeroMovementInput::TraceLifecycle(OriginalScope, TEXT("AttachReturned"));
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
	const EGGYGOMovementInputRequestQueryResult Query =
		OriginalScope->Source->QueryMovementInputRequest(Session, Request, Error);
	if (Query != EGGYGOMovementInputRequestQueryResult::Held)
	{
		// NotHeld 是 Source 确认的正常中立，Query 不附错误；非零 Action 值与之矛盾，单独说明后拒绝投影。
		// Unavailable／AwaitingPhysicalProof 直接沿用 Query 的原错误。
		GGYGOHeroMovementInput::Report(OriginalScope, TEXT("MovementRequestUnavailable"),
			Query == EGGYGOMovementInputRequestQueryResult::NotHeld
				? FString(TEXT("原 Source 查询为 NotHeld（已确认中立且没有活动请求）；拒绝本次非零 Move Action 值的移动投影。"))
				: Error);
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
	FGGYGOMovementInputEvaluationToken ProjectionEvaluation;
	FString ProjectionSourceError;
	const bool bProjectionSourceCaptured = OriginalScope->Source->CaptureMovementInputEvaluationToken(
		Session, ProjectionEvaluation, ProjectionSourceError);
	const FRotationMatrix RotationBasis(YawOnlyRotation);
	Pawn->AddMovementInput(RotationBasis.GetUnitAxis(EAxis::X) * Value.Y);
	Pawn->AddMovementInput(RotationBasis.GetUnitAxis(EAxis::Y) * Value.X);
	// 只交这次真实投影的来源与 Yaw；记录不代表移动已执行，也不改变原 Add 的许可。
	const bool bProjectionRecorded = OriginalScope->Consumer->RecordLocomotionInputProjection(
		Binding, Request, ProjectionEvaluation, static_cast<float>(YawOnlyRotation.Yaw), Error);
	if (!bProjectionSourceCaptured || !bProjectionRecorded)
	{
		GGYGOHeroMovementInput::Report(OriginalScope, TEXT("LocomotionInputProjectionUnavailable"),
			bProjectionSourceCaptured ? Error : ProjectionSourceError);
	}
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

void UGGYGOHeroComponent::InvalidateCurrentAbilityInputRequest(FAbilityActionBinding& Binding)
{
	const FGGYGOAbilityInputRequestIdentity Identity = Binding.CurrentRequest.Identity;
	Binding.CurrentRequest.Identity = FGGYGOAbilityInputRequestIdentity{};
	if (UGGYGOAbilitySystemComponent* OriginalASC = Identity.IsAssigned() ? Identity.SourceASC.Get() : nullptr)
	{
		OriginalASC->EndAbilityInputRequest(Identity, EGGYGOAbilityInputRequestEndKind::Invalidated);
	}
}

void UGGYGOHeroComponent::ReportAbilityInputOnce(FAbilityActionBinding& Binding, FName Reason, const FString& Detail) const
{
	if (Binding.ReportedReasons.Contains(Reason)) { return; }
	Binding.ReportedReasons.Add(Reason);
	UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("Input/Hero Trigger rejected: Hero=%s Action=%s Tag=%s Reason=%s %s"),
		*GetPathName(), *Binding.SourceActionPath, *Binding.InputTag.ToString(), *Reason.ToString(), *Detail);
}

void UGGYGOHeroComponent::InvalidateAbilityActionBinding(const TSharedPtr<FAbilityActionBinding>& Binding)
{
	if (!Binding || Binding->bRetired) { return; }
	Binding->bRetired = true;
	Binding->bPressed = false;
	InvalidateCurrentAbilityInputRequest(*Binding);
}

void UGGYGOHeroComponent::RetireSessionAbilityRequests(const TSharedPtr<FPlayerInputSession>& Session)
{
	if (!Session.IsValid()) { return; }
	// 先在本地摘掉关联与各动作的请求身份；按住中的动作保持 bPressed，释放前不再请求。
	UGGYGOAbilitySystemComponent* const OriginalASC = Session->AssociatedResource.GetIdentity().ASC.Get();
	Session->AssociatedResource = FGGYGOPawnASCResourceHandle{};
	for (const TSharedPtr<FAbilityActionBinding>& Binding : Session->ActionBindings)
	{
		Binding->CurrentRequest.Identity = FGGYGOAbilityInputRequestIdentity{};
	}
	// 原 ASC 按来源结束本会话签发的全部请求（含已释放、仍在缓冲期的）；该调用不外调。
	if (OriginalASC) { OriginalASC->EndAbilityInputRequestsFromSource(Session->MakeSourceKey(this)); }
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
	FGGYGOAbilityInputRetryRequest& Request = Binding->CurrentRequest;
	const bool bFirstTrigger = !Binding->bPressed;
	if (bFirstTrigger)
	{
		// 首个 Triggered 固定本次按下的 Tag、截止与移动评价 Token；按住期间不再采样。
		Binding->bPressed = true;
		Request = FGGYGOAbilityInputRetryRequest{Binding->InputTag, {},
			UGGYGOAbilitySystemComponent::NoAbilityInputRetryDeadline, {}};
		const TSharedPtr<FGGYGOHeroMovementInputScope> MovementScope = MovementInputScope;
		if (IsMovementInputScopeCurrent(MovementScope))
		{
			// 缺源保留未分配 Token；需要方向的消费者明确拒绝，普通能力不新增移动前置。
			FString EvaluationError;
			MovementScope->Source->CaptureMovementInputEvaluationToken(
				MovementScope->Session, Request.OriginalEvaluationToken, EvaluationError);
		}
		const UWorld* World = GetWorld();
		const double Now = World ? World->GetTimeSeconds() : -1.0;
		const double Window = static_cast<double>(InputBufferWindow);
		const double Deadline = Now + Window;
		if (!World || !FMath::IsFinite(Now) || Now < 0.0 || !FMath::IsFinite(Window) || Window < 0.0
			|| !FMath::IsFinite(Deadline))
		{
			ReportAbilityInputOnce(*Binding, TEXT("InvalidWorldOrBufferWindow"), FString::Printf(TEXT("Window=%g"), Window));
			return; // 本次按住以空身份锁存，释放前不再请求。
		}
		Request.OriginalDeadline = Deadline;
	}
	else if (!Request.Identity.IsAssigned())
	{
		return; // 首次被拒、ASC 未就绪或关联已变化：本次按住不重试。
	}
	// 两个输入安装入口都落到此处。先保留原按下锁存，再消费 Pawn 持有的实际准备请求；
	// Pending/Failed 不签发 ASC 身份，Ready 后也不把这次按住变成新的按下。
	const UGGYGOPawnExtensionComponent* PawnExtension = LocalASCSubscription.GetExtension();
	FString PreparationReason;
	if (!PawnExtension || !PawnExtension->AreAbilityInputResourcesReady(Request.InputTag, PreparationReason))
	{
		// 撤销身份后后续 Held 从上方返回；每次新按下保留最新原因，避免先前 Pending 吞掉真实 Failed。
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("Input/Hero Trigger rejected: Hero=%s Action=%s Tag=%s Reason=RenderResourcesNotReady %s"),
			*GetPathName(), *Binding->SourceActionPath, *Request.InputTag.ToString(),
			PawnExtension ? *PreparationReason : TEXT("PawnExtension missing"));
		InvalidateCurrentAbilityInputRequest(*Binding);
		return;
	}
	UGGYGOAbilitySystemComponent* ASC = GetInputSessionAbilitySystem();
	if (!ASC || (!bFirstTrigger && !Request.Identity.SourceASC.HasSameIndexAndSerialNumber(ASC)))
	{
		if (bFirstTrigger) { ReportAbilityInputOnce(*Binding, TEXT("ASCNotReady"), TEXT("original press latched")); }
		else { InvalidateCurrentAbilityInputRequest(*Binding); }
		return;
	}
	// Receive 只改 ASC 输入表与 Spec.InputPressed，不外调；返回后无需重验 Hero/Binding/Session。
	const TSharedPtr<FPlayerInputSession> Session = Binding->Session.Pin();
	const FGGYGOAbilityInputRequestResult Result = ASC->ReceiveAbilityInputRequest(Request.InputTag, Request.Identity,
		Request.OriginalDeadline, Request.OriginalEvaluationToken, Session->MakeSourceKey(this));
	const bool bAccepted = Result.Outcome == EGGYGOAbilityInputRequestOutcome::Accepted
		|| Result.Outcome == EGGYGOAbilityInputRequestOutcome::AlreadyApplied;
	if (bFirstTrigger)
	{
		// 被拒时保持空身份锁存；ASC 已按原因记录诊断。
		if (bAccepted && Result.Identity.IsAssigned()) { Request.Identity = Result.Identity; }
	}
	else if (!bAccepted || Result.Identity != Request.Identity)
	{
		InvalidateCurrentAbilityInputRequest(*Binding);
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
	if (!Binding->bPressed) { return; }
	const FGGYGOAbilityInputRequestIdentity Identity = Binding->CurrentRequest.Identity;
	Binding->bPressed = false;
	Binding->CurrentRequest = FGGYGOAbilityInputRetryRequest{};
	UGGYGOAbilitySystemComponent* const OriginalASC = Identity.IsAssigned() ? Identity.SourceASC.Get() : nullptr;
	if (!OriginalASC) { return; }
	// 关联仍是同一 ASC 时以 Released 结束（保留缓冲期内的单击）；被拒时 ASC 已诊断，身份已在本地结束。
	OriginalASC->EndAbilityInputRequest(Identity, GetInputSessionAbilitySystem() == OriginalASC
		? EGGYGOAbilityInputRequestEndKind::Released : EGGYGOAbilityInputRequestEndKind::Invalidated);
}

bool UGGYGOHeroComponent::HasValidPlayerInputSession() const
{
	const TSharedPtr<FPlayerInputSession> Session = PlayerInputSession;
	if (!Session.IsValid() || Session->bRetired || Session->Generation != InputSessionGeneration) { return false; }
	const APawn* Pawn = GetPawn<APawn>();
	const APlayerController* PC = GetController<APlayerController>();
	const UGGYGOLocalPlayer* LocalPlayer = Session->LocalPlayer.Get();
	const UEnhancedInputLocalPlayerSubsystem* Subsystem = Session->Subsystem.Get();
	return !bEndingPlay && Pawn && Pawn->IsLocallyControlled() && LocalPlayer && PC && !PC->IsActorBeingDestroyed()
		&& PC->GetLocalPlayer() == LocalPlayer && !LocalPlayer->IsInputMappingRemovalStarted()
		&& Session->Component.IsValid() && Pawn->InputComponent == Session->Component.Get()
		&& Subsystem && Session->PlayerInput.IsValid()
		&& LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() == Subsystem
		&& Subsystem->GetPlayerInput() == Session->PlayerInput.Get();
}

bool UGGYGOHeroComponent::IsSessionAssociationCurrent(const TSharedPtr<FPlayerInputSession>& Session) const
{
	if (!Session.IsValid() || Session != PlayerInputSession || !HasValidPlayerInputSession()
		|| !Session->AssociatedResource.HasSameResource(LocalASCSubscription.GetConsumedResource()))
	{
		return false;
	}
	const UGGYGOAbilitySystemComponent* ASC = Session->AssociatedResource.GetIdentity().ASC.Get();
	return ASC && GetReadyLocalAbilitySystemComponent() == ASC;
}

UGGYGOAbilitySystemComponent* UGGYGOHeroComponent::GetInputSessionAbilitySystem() const
{
	return IsSessionAssociationCurrent(PlayerInputSession)
		? PlayerInputSession->AssociatedResource.GetIdentity().ASC.Get() : nullptr;
}

void UGGYGOHeroComponent::AssociateReadyAbilitySystemWithInput()
{
	if (!HasValidPlayerInputSession() || !GetReadyLocalAbilitySystemComponent()) { return; }
	const TSharedPtr<FPlayerInputSession> Session = PlayerInputSession;
	const FGGYGOPawnASCResourceHandle Resource = LocalASCSubscription.GetConsumedResource();
	if (Session->AssociatedResource.HasSameResource(Resource)) { return; }
	// 资源变化：原 ASC 先按来源结束本会话的全部请求（不外调），再关联新资源。
	RetireSessionAbilityRequests(Session);
	Session->AssociatedResource = Resource;
}

bool UGGYGOHeroComponent::PrepareLocalAbilitySystemSubscription(
	UGGYGOPawnExtensionComponent* Extension, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	if (bEndingPlay || !IsValid(Extension) || Extension->GetOwner() != GetOwner())
	{
		OutError = FString::Printf(TEXT("[Input/Hero] LocalASC subscribe rejected: Hero='%s', Owner='%s', Extension='%s'."),
			*GetPathNameSafe(this), *GetPathNameSafe(GetOwner()), *GetPathNameSafe(Extension));
		return false;
	}
	if (LocalASCSubscription.IsSubscribed() && LocalASCSubscription.GetExtension() == Extension) { return true; }
	ReleaseLocalAbilitySystemSubscription();
	const TWeakObjectPtr<UGGYGOHeroComponent> WeakHero(this);
	// 已就绪时立即回放 Ready；Ready 不建立输入，只关联已存在的输入会话。
	LocalASCSubscription.Subscribe(Extension, [WeakHero](const FGGYGOPawnASCNotice& Notice)
	{
		if (UGGYGOHeroComponent* Hero = WeakHero.Get()) { Hero->ConsumeLocalAbilitySystemNotice(Notice); }
	});
	return true;
}

void UGGYGOHeroComponent::ReleaseLocalAbilitySystemSubscription()
{
	check(IsInGameThread());
	const TSharedPtr<FPlayerInputSession> Session = PlayerInputSession;
	if (Session.IsValid() && Session->AssociatedResource.HasResource())
	{
		RetireSessionAbilityRequests(Session);
	}
	LocalASCSubscription.Unsubscribe();
}

void UGGYGOHeroComponent::ConsumeLocalAbilitySystemNotice(const FGGYGOPawnASCNotice& Notice)
{
	check(IsInGameThread());
	if (Notice.Kind == EGGYGOPawnASCNoticeKind::Ready)
	{
		AssociateReadyAbilitySystemWithInput();
		return;
	}
	// Released：本会话关联的是这次绑定时退出输入；相机覆盖随原 Avatar 一起复位。
	const TSharedPtr<FPlayerInputSession> Session = PlayerInputSession;
	const bool bOwnsInput = Session.IsValid() && Session->AssociatedResource.HasSameResource(Notice.Resource);
	const TWeakObjectPtr<UGGYGOHeroComponent> WeakHero(this);
	const uint64 OriginalCameraGeneration = LastAbilityCameraModeRequestGeneration;
	if (bOwnsInput)
	{
		ReleasePlayerInput();
	}
	// ReleasePlayerInput 会外调 Enhanced Input；回调里出现新绑定、新输入会话或新相机请求时不再复位。
	UGGYGOHeroComponent* Hero = WeakHero.Get();
	if (!Hero || Hero->bEndingPlay || Hero->LocalASCSubscription.GetConsumedResource().HasResource()
		|| Hero->LastAbilityCameraModeRequestGeneration != OriginalCameraGeneration
		|| (bOwnsInput ? Hero->PlayerInputSession.IsValid() : Hero->PlayerInputSession != Session))
	{
		return;
	}
	Hero->AbilityCameraModeOverrides.Reset();
	if (UGGYGOCameraComponent* Camera = UGGYGOCameraComponent::FindCameraComponent(Hero->GetOwner()))
	{
		Camera->ResetCameraRuntimeState();
	}
}

UGGYGOAbilitySystemComponent* UGGYGOHeroComponent::GetReadyLocalAbilitySystemComponent() const
{
	check(IsInGameThread());
	if (bEndingPlay || IsBeingDestroyed() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return nullptr; }
	return LocalASCSubscription.GetReadyASC();
}
