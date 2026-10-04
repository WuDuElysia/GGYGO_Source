/**
 * @file GGYGOHeroComponent.cpp
 * @brief 玩家操控单位的输入组件实现
 */
#include "Character/Components/GGYGOHeroComponent.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Camera/GGYGOCameraComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Components/GameFrameworkComponentManager.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Input/GGYGOInputComponent.h"
#include "Input/GGYGOPlayerInput.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "System/GGYGOGameplayTags.h"
// Input-Hero-LocalIdentity includes begin.
#include "Misc/Optional.h"
// Input-Hero-LocalIdentity includes end.

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHeroComponent)

class UActorComponent;

const FName UGGYGOHeroComponent::NAME_ActorFeatureName("Hero");

// These records own Action associations, never physical held state or ASC execution.
struct UGGYGOHeroComponent::FAbilityActionBinding
{
	FAbilityActionBinding(const UInputAction* InAction, const FGameplayTag& InTag,
		UGGYGOInputComponent* InComponent, UEnhancedPlayerInput* InPlayerInput, uint64 InGeneration)
		: SourceAction(InAction), InputTag(InTag), Component(InComponent), PlayerInput(InPlayerInput),
		  InputGeneration(InGeneration), SourceActionPath(GetPathNameSafe(InAction))
	{
	}

	const TWeakObjectPtr<const UInputAction> SourceAction;
	const FGameplayTag InputTag;
	const TWeakObjectPtr<UGGYGOInputComponent> Component;
	const TWeakObjectPtr<UEnhancedPlayerInput> PlayerInput;
	const uint64 InputGeneration;
	const FString SourceActionPath;
	bool bRetired = false;
	TSharedPtr<FAbilityInputObservation> Observation;
};

struct UGGYGOHeroComponent::FAbilityInputObservation
{
	FAbilityInputObservation(const TSharedPtr<FAbilityActionBinding>& InBinding, double InDeadline)
		: Binding(InBinding), Request{InBinding->InputTag, {}, InDeadline}
	{
	}

	const TWeakPtr<FAbilityActionBinding> Binding;
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
		AssociatedInputSessionGeneration.Reset();
		AssociatedInputComponent.Reset();
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
	TOptional<uint64> AssociatedInputSessionGeneration;
	TWeakObjectPtr<UGGYGOInputComponent> AssociatedInputComponent;
	bool bRetired = false;
};

// Derived origin of the existing two native ASC subscriptions, never another execution state.
struct UGGYGOHeroComponent::FAbilityRetryBinding
{
	FAbilityRetryBinding(const TSharedPtr<FLocalAbilitySystemSubscription>& InSubscription,
		const FGGYGOPawnASCResourceHandle& InResource, TWeakObjectPtr<UGGYGOInputComponent> InComponent,
		uint64 InInputGeneration, uint64 InSubscriptionGeneration)
		: Subscription(InSubscription), Resource(InResource), Component(InComponent),
		  InputGeneration(InInputGeneration), SubscriptionGeneration(InSubscriptionGeneration)
	{}

	const TWeakPtr<FLocalAbilitySystemSubscription> Subscription;
	const FGGYGOPawnASCResourceHandle Resource;
	const TWeakObjectPtr<UGGYGOInputComponent> Component;
	const uint64 InputGeneration;
	const uint64 SubscriptionGeneration;
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

	static void ReleaseResources(const TSharedPtr<FGGYGOHeroMovementInputScope>& Scope, FName Reason)
	{
		if (!Scope.IsValid()) { return; }
		const FGGYGOMovementInputSessionIdentity OriginalSession = Scope->Session;
		const FGGYGOMovementInputConsumerBindingId OriginalBinding = Scope->Binding;
		Scope->Session = {};
		Scope->Binding = {};
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

void UGGYGOHeroComponent::HandleChangeInitState(UGameFrameworkComponentManager* Manager, FGameplayTag CurrentState, FGameplayTag DesiredState)
{
	if (CurrentState != GGYGOGameplayTags::InitState_DataAvailable || DesiredState != GGYGOGameplayTags::InitState_DataInitialized)
	{
		return;
	}

	APawn* Pawn = GetPawn<APawn>();
	if (!Pawn || Pawn->InputComponent == nullptr)
	{
		// 输入组件还没建立。它由引擎在附身流程里创建，随后 Pawn 会调
		// SetupPlayerInputComponent，那里再走一次 InitializePlayerInput。
		return;
	}

	InitializePlayerInput(Pawn->InputComponent);
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
	if (bEndingPlay)
	{
		return;
	}
	const uint64 ExpectedGeneration = InputSessionGeneration == MAX_uint64 ? MAX_uint64 : InputSessionGeneration + 1;
	ReleasePlayerInput();
	// 释放 IMC 的同步回调可以建立后继会话；旧初始化不能接着覆盖它。
	if (InputSessionGeneration != ExpectedGeneration || InputSessionComponent.IsValid())
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
	InputSessionComponent = GGYGOIC;
	InputSessionSubsystem = Subsystem;
	InputSessionPlayerInput = EnhancedPlayerInput;
	if (ForceWalkAction)
	{
		if (const ACharacter* Character = GetPawn<ACharacter>())
		{
			InputSessionMovementComponent = Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement());
		}
	}

	const TSharedPtr<FGGYGOHeroMovementInputScope> OriginalMovementScope = MakeShared<FGGYGOHeroMovementInputScope>(
		this, GetPawn<APawn>(), GGYGOIC, MovementSource, Subsystem, MovementConsumer, MoveAction,
		SessionGeneration, MappingsToRegister.Num(), InputConfig);
	MovementInputScope = OriginalMovementScope;
	MovementMappingObserver = NewObject<UGGYGOHeroMovementMappingObserver>(this);
	OriginalMovementScope->Observer = MovementMappingObserver.Get();
	// Install the original registration before Add. Only actual native rebuilt notification begins Source.
	MovementMappingObserver->Initialize(this, Subsystem, OriginalMovementScope);
	const TWeakObjectPtr<UGGYGOHeroComponent> MovementHero(this);
	InputSessionBindHandles.Add(GGYGOIC->BindActionInstanceLambda(MoveAction, ETriggerEvent::Triggered,
		[MovementHero, OriginalMovementScope](const FInputActionInstance& Instance)
		{
			if (UGGYGOHeroComponent* Hero = MovementHero.Get()) { Hero->Input_Move(Instance, OriginalMovementScope); }
		}).GetHandle());
	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_Look_Mouse, ETriggerEvent::Triggered, this, &ThisClass::Input_LookMouse, /*bLogIfNotFound=*/true, &InputSessionBindHandles);

	// 手柄视角是可选的：只用键鼠的项目不配它，不该因此报错。
	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_Look_Stick, ETriggerEvent::Triggered, this, &ThisClass::Input_LookStick, /*bLogIfNotFound=*/false, &InputSessionBindHandles);
	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_ForceWalk, ETriggerEvent::Triggered, this, &ThisClass::Input_ForceWalkPressed, /*bLogIfNotFound=*/false, &InputSessionBindHandles);
	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_ForceWalk, ETriggerEvent::Completed, this, &ThisClass::Input_ForceWalkReleased, /*bLogIfNotFound=*/false, &InputSessionBindHandles);
	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_ForceWalk, ETriggerEvent::Canceled, this, &ThisClass::Input_ForceWalkReleased, /*bLogIfNotFound=*/false, &InputSessionBindHandles);
	const TWeakObjectPtr<UGGYGOHeroComponent> WeakHero(this);
	for (const FGGYGOInputAction& Action : InputConfig->AbilityInputActions)
	{
		if (!Action.InputAction || !Action.InputTag.IsValid())
		{
			continue;
		}
		const TSharedPtr<FAbilityActionBinding> Binding = MakeShared<FAbilityActionBinding>(
			Action.InputAction.Get(), Action.InputTag, GGYGOIC, EnhancedPlayerInput, SessionGeneration);
		AbilityActionBindings.Add(Binding);
		InputSessionBindHandles.Add(GGYGOIC->BindActionInstanceLambda(Action.InputAction, ETriggerEvent::Triggered,
			[WeakHero, Binding](const FInputActionInstance& Instance)
			{
				if (UGGYGOHeroComponent* Hero = WeakHero.Get()) { Hero->Input_AbilityActionTriggered(Instance, Binding); }
			}).GetHandle());
		InputSessionBindHandles.Add(GGYGOIC->BindActionInstanceLambda(Action.InputAction, ETriggerEvent::Completed,
			[WeakHero, Binding](const FInputActionInstance& Instance)
			{
				if (UGGYGOHeroComponent* Hero = WeakHero.Get()) { Hero->Input_AbilityActionReleased(Instance, Binding); }
			}).GetHandle());
	}
	// Preserve original insertion order: Triggered/Completed per Action, then all Canceled.
	for (const TSharedPtr<FAbilityActionBinding>& Binding : AbilityActionBindings)
	{
		InputSessionBindHandles.Add(GGYGOIC->BindActionInstanceLambda(Binding->SourceAction.Get(), ETriggerEvent::Canceled,
			[WeakHero, Binding](const FInputActionInstance& Instance)
			{
				if (UGGYGOHeroComponent* Hero = WeakHero.Get()) { Hero->Input_AbilityActionReleased(Instance, Binding); }
			}).GetHandle());
	}

	for (const TWeakObjectPtr<const UInputMappingContext>& WeakMapping : MappingsToRegister)
	{
		const UInputMappingContext* Mapping = WeakMapping.Get();
		int32 ExistingPriority = INDEX_NONE;
		// 前一项 Add 会同步广播；再验证当前项，不能在来源改变后继续注册。
		if (!Mapping || !InputSessionSubsystem.IsValid() || !InputSessionPlayerInput.IsValid()
			|| Subsystem->GetPlayerInput() != EnhancedPlayerInput
			|| Mapping->GetRegistrationTrackingMode() != EMappingContextRegistrationTrackingMode::CountRegistrations
			|| (Subsystem->HasMappingContext(Mapping, ExistingPriority) && ExistingPriority != RegistrationPriority))
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("InitializePlayerInput: IMC [%s] 的来源、模式或优先级在注册期间改变，回收本会话。"), *GetNameSafe(Mapping));
			ReleasePlayerInput();
			return;
		}
		// 已验证底层输入对象与模式；引擎先增加计数，再广播 Added。
		// 先登记自己的这一份，以便同步 Release 能与刚完成的 Add 配对。
		FRegisteredInputMapping& Registered = InputSessionMappings.AddDefaulted_GetRef();
		Registered.MappingContext = Mapping;
		Registered.RegisteredPriority = RegistrationPriority;
		Subsystem->AddMappingContext(Mapping, RegistrationPriority);
		if (InputSessionGeneration != SessionGeneration)
		{
			return;
		}
	}

	if (!InputSessionComponent.IsValid() || !InputSessionSubsystem.IsValid() || !InputSessionPlayerInput.IsValid()
		|| Subsystem->GetPlayerInput() != EnhancedPlayerInput)
	{
		ReleasePlayerInput();
		return;
	}
	// Input may precede Ready; the exact typed Ready notice later associates this existing session.
	BindAbilityRetryDelegates();
}

void UGGYGOHeroComponent::ReleasePlayerInput()
{
	// Derived association only. Retire it before any original cleanup can install a successor.
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = LocalAbilitySystemSubscription;
	if (OriginalSubscription.IsValid() && OriginalSubscription->AssociatedInputSessionGeneration.IsSet()
		&& OriginalSubscription->AssociatedInputSessionGeneration.GetValue() == InputSessionGeneration
		&& OriginalSubscription->AssociatedInputComponent.HasSameIndexAndSerialNumber(InputSessionComponent))
	{
		OriginalSubscription->AssociatedInputSessionGeneration.Reset();
		OriginalSubscription->AssociatedInputComponent.Reset();
	}
	if (InputSessionGeneration != MAX_uint64)
	{
		++InputSessionGeneration;
	}
	const TWeakObjectPtr<UGGYGOInputComponent> PreviousComponent = InputSessionComponent;
	const TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> PreviousSubsystem = InputSessionSubsystem;
	const TWeakObjectPtr<UEnhancedPlayerInput> PreviousPlayerInput = InputSessionPlayerInput;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> PreviousMovement = InputSessionMovementComponent;
	TArray<uint32> PreviousHandles = MoveTemp(InputSessionBindHandles);
	TArray<FRegisteredInputMapping> PreviousMappings = MoveTemp(InputSessionMappings);
	InputSessionComponent.Reset();
	InputSessionSubsystem.Reset();
	InputSessionPlayerInput.Reset();
	InputSessionMovementComponent.Reset();
	InputSessionBindHandles.Reset();
	InputSessionMappings.Reset();
	const TSharedPtr<FGGYGOHeroMovementInputScope> PreviousMovementScope = MovementInputScope;
	MovementInputScope.Reset();
	const TObjectPtr<UGGYGOHeroMovementMappingObserver> PreviousObserver = MovementMappingObserver;
	MovementMappingObserver = nullptr;
	if (PreviousObserver) { PreviousObserver->Detach(); }
	if (PreviousMovementScope.IsValid()) { PreviousMovementScope->bRetired = true; }
	// Retire original bindings before any ASC cleanup can reenter and install successors.
	const TArray<TSharedPtr<FAbilityActionBinding>> PreviousActionBindings = MoveTemp(AbilityActionBindings);
	AbilityActionBindings.Reset();
	for (const TSharedPtr<FAbilityActionBinding>& Binding : PreviousActionBindings)
	{
		Binding->bRetired = true;
	}
	// The existing setter is a local CMC state write. Return this original request before ASC cleanup can reenter.
	if (UGGYGOCharacterMovementComponent* Movement = PreviousMovement.Get())
	{
		Movement->SetForceWalkRequested(false);
	}
	const TWeakObjectPtr<UGGYGOHeroComponent> ReleaseHero(this);
	const uint64 ReleasedGeneration = InputSessionGeneration;
	const uint64 OriginalSubscriptionGeneration = AbilityInputSubscriptionGeneration;
	const TSharedPtr<FAbilityRetryBinding> OriginalAbilityBinding = AbilityRetryBinding;
	GGYGOHeroMovementInput::Retire(PreviousMovementScope, TEXT("HeroInputReleased"));
	// Source/CMC cleanup is also external: do not unsubscribe an ASC successor installed by that cleanup.
	if (UGGYGOHeroComponent* Hero = ReleaseHero.Get())
	{
		if (Hero->InputSessionGeneration == ReleasedGeneration
			&& Hero->AbilityInputSubscriptionGeneration == OriginalSubscriptionGeneration
			&& Hero->AbilityRetryBinding == OriginalAbilityBinding)
		{
			Hero->UnbindAbilityRetryDelegates();
		}
	}

	if (UGGYGOInputComponent* InputComponent = PreviousComponent.Get())
	{
		InputComponent->RemoveBinds(PreviousHandles);
	}
	// 记录已摘除；Remove 的同步回调即使再次退出，也不会重复归还旧注册。
	for (const FRegisteredInputMapping& Registered : PreviousMappings)
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
		|| !Scope->Component.HasSameIndexAndSerialNumber(InputSessionComponent)
		|| Scope->Source.Get() != InputSessionPlayerInput.Get()
		|| !Scope->Subsystem.HasSameIndexAndSerialNumber(InputSessionSubsystem))
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
	if (InputSessionMappings.Num() != OriginalScope->ExpectedMappingRegistrationCount) { return; }
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
		|| OriginalScope->Binding.ConsumerBindingSerial != 0)
	{
		GGYGOHeroMovementInput::ReleaseResources(Source, Session, Consumer, Binding, TEXT("BindInterrupted"));
		return;
	}
	OriginalScope->Binding = Binding; // Both original resources precede Attach's synchronous replay.
	const TWeakPtr<FGGYGOHeroMovementInputScope> WeakScope(OriginalScope);
	const FGGYGOMovementInputFactDelegate Receiver = FGGYGOMovementInputFactDelegate::CreateLambda(
		[Consumer, Binding, Session, WeakScope](const FGGYGOMovementInputConsumerBindingId& ReceivedBinding,
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
				}
				else if (Result == EGGYGOMovementInputConsumeResult::Rejected || Result == EGGYGOMovementInputConsumeResult::Stale)
				{
					GGYGOHeroMovementInput::Report(Scope, TEXT("FactConsumeRejected"), ConsumeError);
					GGYGOHeroMovementInput::Retire(Scope, TEXT("FactConsumeRejected"));
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
		if (UGGYGOCharacterMovementComponent* MoveComp = Character->IsLocallyControlled() ? InputSessionMovementComponent.Get() : nullptr)
		{
			MoveComp->SetForceWalkRequested(true);
		}
	}
}

void UGGYGOHeroComponent::Input_ForceWalkReleased()
{
	if (UGGYGOCharacterMovementComponent* MoveComp = InputSessionMovementComponent.Get())
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
	return Binding && !Binding->bRetired && HasValidPlayerInputSession()
		&& Binding->InputGeneration == InputSessionGeneration && Binding->SourceAction.IsValid()
		&& Binding->Component.HasSameIndexAndSerialNumber(InputSessionComponent)
		&& Binding->PlayerInput.HasSameIndexAndSerialNumber(InputSessionPlayerInput)
		&& AbilityActionBindings.Contains(Binding);
}

void UGGYGOHeroComponent::InvalidateAbilityInputObservations(
	const TArray<TSharedPtr<FAbilityInputObservation>>& Observations)
{
	TArray<FGGYGOAbilityInputRequestIdentity> OriginalIdentities;
	for (const TSharedPtr<FAbilityInputObservation>& Observation : Observations)
	{
		if (Observation && !Observation->bInvalidated)
		{
			Observation->bInvalidated = true;
			if (Observation->Request.Identity.IsAssigned()) { OriginalIdentities.Add(Observation->Request.Identity); }
		}
	}
	// All local retirement precedes external cleanup. Active binding latches remain until Action end.
	AbilityInputObservations.RemoveAll([&Observations](const TSharedPtr<FAbilityInputObservation>& Observation)
		{ return Observations.Contains(Observation); });
	BufferedInputs.RemoveAll([&OriginalIdentities](const FGGYGOAbilityInputRetryRequest& Request)
		{ return OriginalIdentities.Contains(Request.Identity); });
	for (const FGGYGOAbilityInputRequestIdentity& Identity : OriginalIdentities)
	{
		if (UGGYGOAbilitySystemComponent* OriginalASC = Identity.SourceASC.Get())
		{
			OriginalASC->EndAbilityInputRequest(Identity, EGGYGOAbilityInputRequestEndKind::Invalidated);
		}
	}
	// No Hero fields are touched after cleanup, which may retire this Hero or install a successor.
}

void UGGYGOHeroComponent::InvalidateAbilityActionBinding(const TSharedPtr<FAbilityActionBinding>& Binding)
{
	if (!Binding || Binding->bRetired) { return; }
	Binding->bRetired = true;
	TArray<TSharedPtr<FAbilityInputObservation>> OriginalObservations;
	for (const TSharedPtr<FAbilityInputObservation>& Observation : AbilityInputObservations)
	{
		if (Observation->Binding.Pin() == Binding) { OriginalObservations.Add(Observation); }
	}
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
	if (World && FMath::IsFinite(Now)) { PruneExpiredInputRequests(Now); }
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
		AbilityInputObservations.Add(Observation);
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
	if (!bFirstTrigger && !Original->Request.Identity.SourceASC.HasSameIndexAndSerialNumber(ExpectedASC))
	{
		InvalidateAbilityInputObservations({Original});
		return;
	}
	const FGGYGOAbilityInputRequestIdentity PreviousIdentity = Original->Request.Identity;
	const TWeakObjectPtr<UGGYGOHeroComponent> WeakHero(this);
	const FGGYGOAbilityInputRequestResult Result = ExpectedASC->ReceiveAbilityInputRequest(
		Original->Request.InputTag, PreviousIdentity, Original->Request.OriginalDeadline);
	UGGYGOHeroComponent* OriginalHero = WeakHero.Get();
	if (!OriginalHero || !OriginalHero->IsAbilityActionBindingCurrent(Binding)
		|| Binding->Observation != Original || Original->bInvalidated || Original->bActionEnded
		|| !ExpectedASC.IsValid() || OriginalHero->GetInputSessionAbilitySystem() != ExpectedASC.Get())
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
		AbilityInputObservations.Remove(Original);
		return;
	}
	const FGGYGOAbilityInputRequestIdentity Identity = Original->Request.Identity;
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC = Identity.SourceASC;
	if (!OriginalASC.IsValid() || GetInputSessionAbilitySystem() != OriginalASC.Get())
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

TSharedPtr<UGGYGOHeroComponent::FAbilityInputObservation> UGGYGOHeroComponent::FindAbilityInputObservation(
	const FGGYGOAbilityInputRequestIdentity& Identity) const
{
	if (!Identity.IsAssigned()) { return nullptr; }
	for (const TSharedPtr<FAbilityInputObservation>& Observation : AbilityInputObservations)
	{
		if (!Observation->bInvalidated && Observation->Request.Identity == Identity) { return Observation; }
	}
	return nullptr;
}

bool UGGYGOHeroComponent::HasValidPlayerInputSession() const
{
	const APawn* Pawn = GetPawn<APawn>();
	const APlayerController* PC = GetController<APlayerController>();
	const ULocalPlayer* LocalPlayer = PC ? PC->GetLocalPlayer() : nullptr;
	const UEnhancedInputLocalPlayerSubsystem* Subsystem = InputSessionSubsystem.Get();
	return !bEndingPlay && Pawn && Pawn->IsLocallyControlled() && LocalPlayer
		&& InputSessionComponent.IsValid() && Subsystem && InputSessionPlayerInput.IsValid()
		&& LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() == Subsystem
		&& Subsystem->GetPlayerInput() == InputSessionPlayerInput.Get();
}

bool UGGYGOHeroComponent::IsAbilityRetryBindingCurrent(const TSharedPtr<FAbilityRetryBinding>& Binding) const
{
	if (!Binding.IsValid() || AbilityRetryBinding != Binding
		|| Binding->InputGeneration != InputSessionGeneration
		|| Binding->SubscriptionGeneration != AbilityInputSubscriptionGeneration
		|| !Binding->Component.HasSameIndexAndSerialNumber(InputSessionComponent)
		|| !HasValidPlayerInputSession()) { return false; }
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = Binding->Subscription.Pin();
	if (!OriginalSubscription.IsValid() || LocalAbilitySystemSubscription != OriginalSubscription
		|| OriginalSubscription->bRetired || !OriginalSubscription->Resource.HasSameResource(Binding->Resource)
		|| !OriginalSubscription->AssociatedInputSessionGeneration.IsSet()
		|| OriginalSubscription->AssociatedInputSessionGeneration.GetValue() != Binding->InputGeneration
		|| !OriginalSubscription->AssociatedInputComponent.HasSameIndexAndSerialNumber(Binding->Component))
	{
		return false;
	}
	const FGGYGOPawnASCResourceIdentity Identity = Binding->Resource.GetIdentity();
	return Identity.ASC.IsValid() && Identity.ASC.HasSameIndexAndSerialNumber(InputSessionAbilitySystem)
		&& GetReadyLocalAbilitySystemComponent() == Identity.ASC.Get();
}

UGGYGOAbilitySystemComponent* UGGYGOHeroComponent::GetInputSessionAbilitySystem() const
{
	return IsAbilityRetryBindingCurrent(AbilityRetryBinding) ? InputSessionAbilitySystem.Get() : nullptr;
}

bool UGGYGOHeroComponent::IsInputSessionAbilitySystemCurrent(TWeakObjectPtr<UGGYGOAbilitySystemComponent> ExpectedASC, uint64 ExpectedGeneration, uint64 ExpectedSubscriptionGeneration) const
{
	return ExpectedGeneration == InputSessionGeneration && ExpectedSubscriptionGeneration == AbilityInputSubscriptionGeneration && ExpectedASC.IsValid()
		&& GetInputSessionAbilitySystem() == ExpectedASC.Get();
}

void UGGYGOHeroComponent::UnbindAbilityRetryDelegates()
{
	if (AbilityInputSubscriptionGeneration != MAX_uint64) { ++AbilityInputSubscriptionGeneration; }
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> PreviousASC = InputSessionAbilitySystem;
	const FDelegateHandle RetryHandle = AbilityInputRetryableDelegateHandle;
	const FDelegateHandle GroupFreedHandle = AbilityGroupFreedDelegateHandle;
	TArray<TSharedPtr<FAbilityInputObservation>> OriginalObservations;
	for (const TSharedPtr<FAbilityInputObservation>& Observation : AbilityInputObservations)
	{
		const TSharedPtr<FAbilityActionBinding> Binding = Observation->Binding.Pin();
		if (!Binding || Binding->bRetired
			|| (Observation->Request.Identity.IsAssigned()
				&& Observation->Request.Identity.SourceASC.HasSameIndexAndSerialNumber(PreviousASC)))
		{
			OriginalObservations.Add(Observation);
		}
	}
	InputSessionAbilitySystem.Reset();
	AbilityRetryBinding.Reset();
	AbilityInputRetryableDelegateHandle.Reset();
	AbilityGroupFreedDelegateHandle.Reset();
	if (UGGYGOAbilitySystemComponent* ASC = PreviousASC.Get())
	{
		ASC->OnAbilityInputRetryable.Remove(RetryHandle);
		ASC->OnAbilityGroupFreed.Remove(GroupFreedHandle);
	}
	// The binding's first-observation latch survives an ASC-only unsubscribe/rebind.
	InvalidateAbilityInputObservations(OriginalObservations);
}

void UGGYGOHeroComponent::BindAbilityRetryDelegates()
{
	if (!HasValidPlayerInputSession()) { return; }
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = LocalAbilitySystemSubscription;
	UGGYGOAbilitySystemComponent* ASC = GetReadyLocalAbilitySystemComponent();
	if (!OriginalSubscription.IsValid() || !ASC) { return; }
	const FGGYGOPawnASCResourceHandle OriginalResource = OriginalSubscription->Resource;
	const uint64 Generation = InputSessionGeneration;
	FString Error;
	if (!AssociateInputSessionWithLocalResource(OriginalResource, Generation, Error))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("%s"), *Error);
		return;
	}
	if (IsAbilityRetryBindingCurrent(AbilityRetryBinding) && InputSessionAbilitySystem.Get() == ASC
		&& AbilityInputRetryableDelegateHandle.IsValid() && AbilityGroupFreedDelegateHandle.IsValid())
	{
		return;
	}
	uint64 ExpectedSubscriptionGeneration = AbilityInputSubscriptionGeneration;
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> ExpectedASC = ASC;
	const TWeakObjectPtr<UGGYGOHeroComponent> WeakHero(this);
	if (!InputSessionAbilitySystem.IsExplicitlyNull() || AbilityRetryBinding.IsValid()
		|| AbilityInputRetryableDelegateHandle.IsValid() || AbilityGroupFreedDelegateHandle.IsValid())
	{
		if (ExpectedSubscriptionGeneration != MAX_uint64) { ++ExpectedSubscriptionGeneration; }
		UnbindAbilityRetryDelegates();
	}
	UGGYGOHeroComponent* OriginalHero = WeakHero.Get();
	if (!OriginalHero) { return; }
	// Original ID cleanup may install a successor, including on the same ASC and input generation.
	if (OriginalHero->InputSessionGeneration != Generation || !OriginalHero->HasValidPlayerInputSession()
		|| OriginalHero->AbilityInputSubscriptionGeneration != ExpectedSubscriptionGeneration
		|| !OriginalHero->InputSessionAbilitySystem.IsExplicitlyNull() || OriginalHero->AbilityRetryBinding.IsValid()
		|| OriginalHero->AbilityInputRetryableDelegateHandle.IsValid() || OriginalHero->AbilityGroupFreedDelegateHandle.IsValid()
		|| !ExpectedASC.IsValid() || OriginalHero->LocalAbilitySystemSubscription != OriginalSubscription
		|| OriginalSubscription->bRetired || !OriginalSubscription->Resource.HasSameResource(OriginalResource)
		|| OriginalHero->GetReadyLocalAbilitySystemComponent() != ExpectedASC.Get())
	{
		return;
	}
	if (ExpectedSubscriptionGeneration == MAX_uint64)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("Input/Hero ASC subscription rejected: Hero=%s ASC=%s Reason=SubscriptionGenerationExhausted"),
			*OriginalHero->GetPathName(), *GetPathNameSafe(ExpectedASC.Get()));
		return;
	}
	if (!OriginalHero->AssociateInputSessionWithLocalResource(OriginalResource, Generation, Error))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("%s"), *Error);
		return;
	}
	const TSharedPtr<FAbilityRetryBinding> OriginalBinding = MakeShared<FAbilityRetryBinding>(
		OriginalSubscription, OriginalResource, OriginalHero->InputSessionComponent,
		Generation, ExpectedSubscriptionGeneration);
	OriginalHero->InputSessionAbilitySystem = ExpectedASC;
	OriginalHero->AbilityRetryBinding = OriginalBinding;
	// Native multicast Add does not replay; both handles capture this exact immutable H/record origin.
	OriginalHero->AbilityInputRetryableDelegateHandle = ExpectedASC->OnAbilityInputRetryable.AddWeakLambda(OriginalHero,
		[WeakHero, OriginalBinding](const FGGYGOAbilityInputRetryRequest& OriginalRequest)
		{
			if (UGGYGOHeroComponent* Hero = WeakHero.Get())
			{
				if (Hero->IsAbilityRetryBindingCurrent(OriginalBinding))
				{
					Hero->BufferAbilityInput(OriginalRequest);
				}
			}
		});
	OriginalHero->AbilityGroupFreedDelegateHandle = ExpectedASC->OnAbilityGroupFreed.AddWeakLambda(OriginalHero,
		[WeakHero, OriginalBinding](FGameplayTag GroupTag)
		{
			if (UGGYGOHeroComponent* Hero = WeakHero.Get())
			{
				if (Hero->IsAbilityRetryBindingCurrent(OriginalBinding))
				{
					Hero->HandleAbilityGroupFreed(GroupTag);
				}
			}
		});
}

void UGGYGOHeroComponent::PruneExpiredInputRequests(double Now)
{
	AbilityInputObservations.RemoveAll([Now](const TSharedPtr<FAbilityInputObservation>& Observation)
	{
		return Observation->bInvalidated || (Observation->bActionEnded
			&& (!Observation->Request.Identity.IsAssigned()
				|| !FMath::IsFinite(Observation->Request.OriginalDeadline) || Now >= Observation->Request.OriginalDeadline));
	});
	BufferedInputs.RemoveAll([Now](const FGGYGOAbilityInputRetryRequest& Request)
		{ return !FMath::IsFinite(Request.OriginalDeadline) || Now >= Request.OriginalDeadline; });
	// Active Action observations remain latched and held is still solely owned by ASC.
}

void UGGYGOHeroComponent::BufferAbilityInput(const FGGYGOAbilityInputRetryRequest& OriginalRequest)
{
	const FGGYGOAbilityInputRetryRequest Request = OriginalRequest;
	const UWorld* World = GetWorld();
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> ExpectedASC(GetInputSessionAbilitySystem());
	if (!World || !ExpectedASC.IsValid()) { return; }
	const double Now = World->GetTimeSeconds();
	PruneExpiredInputRequests(Now);
	if (!Request.Identity.IsAssigned() || !Request.InputTag.IsValid()
		|| !FMath::IsFinite(Request.OriginalDeadline) || Request.OriginalDeadline < 0.0)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("Input/Hero Retry rejected: Hero=%s Tag=%s Serial=%llu Deadline=%g Reason=InvalidOriginalRequest"),
			*GetPathName(), *Request.InputTag.ToString(),
			static_cast<unsigned long long>(Request.Identity.RequestSerial), Request.OriginalDeadline);
		return;
	}
	if (!Request.Identity.SourceASC.HasSameIndexAndSerialNumber(ExpectedASC)
		|| Now >= Request.OriginalDeadline || ExpectedASC->HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked))
	{
		return;
	}
	const TSharedPtr<FAbilityInputObservation> Original = FindAbilityInputObservation(Request.Identity);
	// Notifications belonging to another source are not this Hero's association.
	if (!Original || Original->Request.InputTag != Request.InputTag
		|| Original->Request.OriginalDeadline != Request.OriginalDeadline)
	{
		return;
	}
	const TSharedPtr<FAbilityActionBinding> Binding = Original->Binding.Pin();
	if (!IsAbilityActionBindingCurrent(Binding))
	{
		if (Binding) { InvalidateAbilityActionBinding(Binding); }
		else { InvalidateAbilityInputObservations({Original}); }
		return;
	}
	if (!BufferedInputs.ContainsByPredicate([&Request](const FGGYGOAbilityInputRetryRequest& Buffered)
		{ return Buffered.Identity == Request.Identity; }))
	{
		BufferedInputs.Add(Request);
	}
}

void UGGYGOHeroComponent::HandleAbilityGroupFreed(FGameplayTag GroupTag)
{
	const UWorld* World = GetWorld();
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> ExpectedASC(GetInputSessionAbilitySystem());
	const uint64 Generation = InputSessionGeneration;
	const uint64 SubscriptionGeneration = AbilityInputSubscriptionGeneration;
	if (!World || !ExpectedASC.IsValid()) { return; }
	PruneExpiredInputRequests(World->GetTimeSeconds());
	if (ExpectedASC->HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked))
	{
		BufferedInputs.Reset();
		return;
	}
	// Group selection remains in ASC. Move before Queue so synchronous new failures survive.
	const TArray<FGGYGOAbilityInputRetryRequest> Pending = MoveTemp(BufferedInputs);
	BufferedInputs.Reset();
	const TWeakObjectPtr<UGGYGOHeroComponent> WeakHero(this);
	for (const FGGYGOAbilityInputRetryRequest& Request : Pending)
	{
		UGGYGOHeroComponent* OriginalHero = WeakHero.Get();
		if (!OriginalHero || !OriginalHero->IsInputSessionAbilitySystemCurrent(ExpectedASC, Generation, SubscriptionGeneration))
		{
			return;
		}
		const TSharedPtr<FAbilityInputObservation> Original = OriginalHero->FindAbilityInputObservation(Request.Identity);
		if (!Original || Original->Request.InputTag != Request.InputTag
			|| Original->Request.OriginalDeadline != Request.OriginalDeadline)
		{
			continue;
		}
		const TSharedPtr<FAbilityActionBinding> Binding = Original->Binding.Pin();
		if (!OriginalHero->IsAbilityActionBindingCurrent(Binding))
		{
			if (Binding) { OriginalHero->InvalidateAbilityActionBinding(Binding); }
			else { OriginalHero->InvalidateAbilityInputObservations({Original}); }
			continue;
		}
		const UWorld* OriginalWorld = OriginalHero->GetWorld();
		if (!OriginalWorld || OriginalWorld->GetTimeSeconds() >= Request.OriginalDeadline) { continue; }
		ExpectedASC->QueueAbilityInputRetry(Request);
		// Next iteration reacquires the original weak Hero and both subscription identities.
	}
}

// Input-Hero-LocalIdentity implementation begin.
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
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = LocalAbilitySystemSubscription;
	if (!OriginalSubscription.IsValid()) { return; }
	LocalAbilitySystemSubscription.Reset();
	const TSharedPtr<FAbilityRetryBinding> OriginalBinding = AbilityRetryBinding;
	OriginalSubscription->Retire();
	if (OriginalBinding.IsValid() && OriginalBinding->Subscription.Pin() == OriginalSubscription
		&& AbilityRetryBinding == OriginalBinding)
	{
		UnbindAbilityRetryDelegates();
	}
	// No writes after original ASC cleanup; a reentrant successor is untouched.
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
	if (OriginalNotice.Kind == EGGYGOPawnASCLocalNoticeKind::Released)
	{
		// Exact withdrawal may outlive Pawn/ASC validity; capture cleanup provenance before revoking it.
		if (!OriginalSubscription->Resource.HasSameResource(OriginalResource))
		{
			Reject(TEXT("Released does not name the consumed original resource; successor is retained"));
			return;
		}
		const TOptional<uint64> OriginalGeneration = OriginalSubscription->AssociatedInputSessionGeneration;
		const TWeakObjectPtr<UGGYGOInputComponent> OriginalComponent = OriginalSubscription->AssociatedInputComponent;
		const TSharedPtr<FAbilityRetryBinding> OriginalBinding = AbilityRetryBinding;
		const TWeakObjectPtr<UGGYGOHeroComponent> WeakHero(this);
		const uint64 OriginalCameraGeneration = LastAbilityCameraModeRequestGeneration;
		const TWeakObjectPtr<APawn> OriginalPawn = OriginalSubscription->Pawn;
		const TWeakObjectPtr<UGGYGOCameraComponent> OriginalCamera =
			UGGYGOCameraComponent::FindCameraComponent(OriginalPawn.Get());
		const uint64 OriginalInputGeneration = InputSessionGeneration;
		const TWeakObjectPtr<UGGYGOInputComponent> OriginalSessionComponent = InputSessionComponent;
		const uint64 OriginalRetryGeneration = AbilityInputSubscriptionGeneration;
		const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC = InputSessionAbilitySystem;
		const FDelegateHandle OriginalRetryHandle = AbilityInputRetryableDelegateHandle;
		const FDelegateHandle OriginalGroupFreedHandle = AbilityGroupFreedDelegateHandle;
		const bool bOwnsInput = OriginalGeneration.IsSet()
			&& OriginalGeneration.GetValue() == OriginalInputGeneration
			&& OriginalComponent.HasSameIndexAndSerialNumber(OriginalSessionComponent);
		const bool bOwnsRetry = OriginalBinding.IsValid()
			&& OriginalBinding->Subscription.Pin() == OriginalSubscription
			&& OriginalBinding->Resource.HasSameResource(OriginalResource);
		const bool bRetiresRetry = bOwnsInput || bOwnsRetry;
		const uint64 ExpectedInputGeneration = bOwnsInput && OriginalInputGeneration != MAX_uint64
			? OriginalInputGeneration + 1 : OriginalInputGeneration;
		const uint64 ExpectedRetryGeneration = bRetiresRetry && OriginalRetryGeneration != MAX_uint64
			? OriginalRetryGeneration + 1 : OriginalRetryGeneration;
		OriginalSubscription->Resource = FGGYGOPawnASCResourceHandle{};
		OriginalSubscription->AssociatedInputSessionGeneration.Reset();
		OriginalSubscription->AssociatedInputComponent.Reset();
		if (bOwnsInput)
		{
			ReleasePlayerInput();
		}
		else if (bOwnsRetry && AbilityRetryBinding == OriginalBinding)
		{
			UnbindAbilityRetryDelegates(); // Original retry subscription only, never an unrelated input session.
		}
		// Camera withdrawal belongs to the matching H, including when it has no input session.
		// Original input/ASC cleanup may reenter. Do not clear any replacement resource or camera request.
		UGGYGOHeroComponent* Hero = WeakHero.Get();
		if (!Hero || Hero->bEndingPlay || Hero->IsBeingDestroyed()
			|| Hero->LocalAbilitySystemSubscription != OriginalSubscription || OriginalSubscription->bRetired
			|| OriginalSubscription->Resource.HasResource()
			|| Hero->InputSessionGeneration != ExpectedInputGeneration
			|| Hero->AbilityInputSubscriptionGeneration != ExpectedRetryGeneration
			|| Hero->LastAbilityCameraModeRequestGeneration != OriginalCameraGeneration
			|| (bOwnsInput ? !Hero->InputSessionComponent.IsExplicitlyNull()
				: !Hero->InputSessionComponent.HasSameIndexAndSerialNumber(OriginalSessionComponent)))
		{
			return;
		}
		if (bRetiresRetry)
		{
			if (!Hero->InputSessionAbilitySystem.IsExplicitlyNull() || Hero->AbilityRetryBinding.IsValid()
				|| Hero->AbilityInputRetryableDelegateHandle.IsValid() || Hero->AbilityGroupFreedDelegateHandle.IsValid())
			{
				return;
			}
		}
		else if (!Hero->InputSessionAbilitySystem.HasSameIndexAndSerialNumber(OriginalASC)
			|| Hero->AbilityRetryBinding != OriginalBinding
			|| Hero->AbilityInputRetryableDelegateHandle != OriginalRetryHandle
			|| Hero->AbilityGroupFreedDelegateHandle != OriginalGroupFreedHandle)
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
		Reject(TEXT("kind is not Ready, Released or Refreshed"));
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
	// Ready never builds input. Associate and subscribe only if the existing input session is live.
	BindAbilityRetryDelegates();
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

bool UGGYGOHeroComponent::AssociateInputSessionWithLocalResource(
	const FGGYGOPawnASCResourceHandle& ExpectedResource, uint64 ExpectedInputSessionGeneration, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = LocalAbilitySystemSubscription;
	const FGGYGOPawnASCResourceHandle OriginalResource = ExpectedResource;
	const FGGYGOPawnASCResourceIdentity Identity = OriginalResource.GetIdentity();
	const FString ErrorContext = FString::Printf(
		TEXT("[Input/Hero] LocalASC input association: Hero='%s', Extension='%s', ASC='%s', Pawn='%s', InputGeneration=%llu"),
		*GetPathNameSafe(this),
		*GetPathNameSafe(OriginalSubscription.IsValid() ? OriginalSubscription->Extension.Get() : nullptr),
		*GetPathNameSafe(Identity.ASC.Get()), *GetPathNameSafe(Identity.Pawn.Get()),
		static_cast<unsigned long long>(ExpectedInputSessionGeneration));
	const auto Reject = [&OutError, &ErrorContext](const TCHAR* Reason)
	{
		OutError = FString::Printf(TEXT("%s, Reason='%s'."), *ErrorContext, Reason);
		return false;
	};
	if (!OriginalSubscription.IsValid() || OriginalSubscription->bRetired || !OriginalResource.HasResource()
		|| !OriginalSubscription->Resource.HasSameResource(OriginalResource))
	{
		return Reject(TEXT("requires the exact consumed original opaque resource and active record"));
	}
	UGGYGOAbilitySystemComponent* ASC = GetReadyLocalAbilitySystemComponent();
	if (!ASC || ASC != Identity.ASC.Get())
	{
		return Reject(TEXT("original local resource is not currently Ready"));
	}
	const APawn* Pawn = OriginalSubscription->Pawn.Get();
	if (ExpectedInputSessionGeneration != InputSessionGeneration || !HasValidPlayerInputSession()
		|| !Pawn || InputSessionComponent.Get() != Pawn->InputComponent)
	{
		return Reject(TEXT("input generation/component does not name the existing original Hero input session"));
	}
	if (OriginalSubscription->AssociatedInputSessionGeneration.IsSet()
		&& OriginalSubscription->AssociatedInputSessionGeneration.GetValue() == ExpectedInputSessionGeneration
		&& !OriginalSubscription->AssociatedInputComponent.HasSameIndexAndSerialNumber(InputSessionComponent))
	{
		return Reject(TEXT("one input generation cannot be reassociated with a different component"));
	}
	if (LocalAbilitySystemSubscription != OriginalSubscription || OriginalSubscription->bRetired
		|| !OriginalSubscription->Resource.HasSameResource(OriginalResource))
	{
		return Reject(TEXT("original record changed before the derived association was stored"));
	}
	OriginalSubscription->AssociatedInputSessionGeneration = ExpectedInputSessionGeneration;
	OriginalSubscription->AssociatedInputComponent = InputSessionComponent;
	return true; // Derived identity association, not input/movement assembly or admission.
}
// Input-Hero-LocalIdentity implementation end.
