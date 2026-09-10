/**
 * @file GGYGOHeroComponent.cpp
 * @brief 玩家操控单位的输入组件实现
 */
#include "Character/Components/GGYGOHeroComponent.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Components/GameFrameworkComponentManager.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Input/GGYGOInputComponent.h"
#include "InputMappingContext.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHeroComponent)

class UActorComponent;

const FName UGGYGOHeroComponent::NAME_ActorFeatureName("Hero");

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

	// 只关心 PawnExtension 的状态：输入初始化需要 PawnData，而 PawnData 归它管。
	BindOnActorInitStateChanged(UGGYGOPawnExtensionComponent::NAME_ActorFeatureName, FGameplayTag(), false);

	ensure(TryToChangeInitState(GGYGOGameplayTags::InitState_Spawned));
	CheckDefaultInitialization();
}

void UGGYGOHeroComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterInitStateFeature();

	Super::EndPlay(EndPlayReason);
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
	check(PlayerInputComponent);

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

	// 先清空再注册。本函数可能被调用两次（InitState 推进时与 SetupPlayerInputComponent 时），
	// 不清空会让同一个 IMC 叠加注册，输入被处理多次。
	Subsystem->ClearAllMappings();

	for (const TObjectPtr<const UInputMappingContext>& Mapping : DefaultInputMappings)
	{
		if (Mapping)
		{
			Subsystem->AddMappingContext(Mapping, InputMappingPriority);
		}
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
		// 输入组件类型由项目设置里的 DefaultPlayerInputClass 决定。
		// 类型不对时批量绑定用不了，这是工程配置错误。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializePlayerInput: 输入组件不是 UGGYGOInputComponent。请在项目设置里把 DefaultPlayerInputClass 设为它。"));
		return;
	}

	// 解绑上一次的能力输入。换角色时若不解绑，旧角色的能力仍会响应按键。
	GGYGOIC->RemoveBinds(AbilityInputBindHandles);

	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_Move, ETriggerEvent::Triggered, this, &ThisClass::Input_Move, /*bLogIfNotFound=*/true);
	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_Look_Mouse, ETriggerEvent::Triggered, this, &ThisClass::Input_LookMouse, /*bLogIfNotFound=*/true);

	// 手柄视角是可选的：只用键鼠的项目不配它，不该因此报错。
	GGYGOIC->BindNativeAction(InputConfig, GGYGOGameplayTags::InputTag_Look_Stick, ETriggerEvent::Triggered, this, &ThisClass::Input_LookStick, /*bLogIfNotFound=*/false);

	GGYGOIC->BindAbilityActions(InputConfig, this, &ThisClass::Input_AbilityInputTagPressed, &ThisClass::Input_AbilityInputTagReleased, AbilityInputBindHandles);

	// 输入缓冲依赖 ASC 的通知。放在这里而不是 BeginPlay：
	// ASC 就绪的时机晚于 BeginPlay，而走到这里 PawnData 已经就位，ASC 必然也在。
	BindAbilityRetryDelegates();
}

void UGGYGOHeroComponent::Input_Move(const FInputActionValue& InputActionValue)
{
	APawn* Pawn = GetPawn<APawn>();
	AController* Controller = Pawn ? Pawn->GetController() : nullptr;
	if (!Controller)
	{
		return;
	}

	const FVector2D Value = InputActionValue.Get<FVector2D>();
	if (Value.IsNearlyZero())
	{
		return;
	}

	// 摄像机相对移动：摇杆的"上"是摄像机朝向的前方，不是角色的前方。
	// 只取 Yaw —— 带上 Pitch 会让俯视时的前向有向下分量，角色会试图往地里走。
	const FRotator YawOnlyRotation(0.0f, Controller->GetControlRotation().Yaw, 0.0f);
	const FRotationMatrix RotationBasis(YawOnlyRotation);

	// 走 AddMovementInput 而不是自己存一份输入：它累加进 CMC 的 Acceleration，
	// 因此被 SavedMove 保存并获得网络预测。CMC 看不见的输入无法参与预测。
	Pawn->AddMovementInput(RotationBasis.GetUnitAxis(EAxis::X) * Value.Y);
	Pawn->AddMovementInput(RotationBasis.GetUnitAxis(EAxis::Y) * Value.X);
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

void UGGYGOHeroComponent::Input_AbilityInputTagPressed(FGameplayTag InputTag)
{
	const UGGYGOPawnExtensionComponent* PawnExtComp = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(GetPawn<APawn>());
	if (UGGYGOAbilitySystemComponent* GGYGOASC = PawnExtComp ? PawnExtComp->GetGGYGOAbilitySystemComponent() : nullptr)
	{
		// 只入缓存，不在这里激活。激活统一由 PlayerController 在 PostProcessInput
		// 里触发，那时本帧所有输入事件都已到达，先后顺序才是确定的。
		GGYGOASC->AbilityInputTagPressed(InputTag);
	}
}

void UGGYGOHeroComponent::Input_AbilityInputTagReleased(FGameplayTag InputTag)
{
	const UGGYGOPawnExtensionComponent* PawnExtComp = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(GetPawn<APawn>());
	if (UGGYGOAbilitySystemComponent* GGYGOASC = PawnExtComp ? PawnExtComp->GetGGYGOAbilitySystemComponent() : nullptr)
	{
		GGYGOASC->AbilityInputTagReleased(InputTag);
	}
}

void UGGYGOHeroComponent::BindAbilityRetryDelegates()
{
	if (bAbilityRetryDelegatesBound)
	{
		return;
	}

	const UGGYGOPawnExtensionComponent* PawnExtComp = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(GetPawn<APawn>());
	UGGYGOAbilitySystemComponent* GGYGOASC = PawnExtComp ? PawnExtComp->GetGGYGOAbilitySystemComponent() : nullptr;
	if (!GGYGOASC)
	{
		return;
	}

	GGYGOASC->OnAbilityInputRetryable.AddUObject(this, &ThisClass::BufferAbilityInput);
	GGYGOASC->OnAbilityGroupFreed.AddUObject(this, &ThisClass::HandleAbilityGroupFreed);

	bAbilityRetryDelegatesBound = true;
}

void UGGYGOHeroComponent::BufferAbilityInput(FGameplayTag InputTag)
{
	const UWorld* World = GetWorld();
	if (!World || !InputTag.IsValid())
	{
		return;
	}

	const float Now = World->GetTimeSeconds();

	// 同一个 InputTag 已在缓冲里就只刷新时间戳。
	// 追加一条会让玩家连按三次时产生三次重试，一次组空出就连出三段。
	for (FBufferedInput& Buffered : BufferedInputs)
	{
		if (Buffered.InputTag.MatchesTagExact(InputTag))
		{
			Buffered.BufferedAtTime = Now;
			return;
		}
	}

	BufferedInputs.Add(FBufferedInput{ InputTag, Now });
}

void UGGYGOHeroComponent::HandleAbilityGroupFreed(FGameplayTag GroupTag)
{
	const UWorld* World = GetWorld();
	if (!World || BufferedInputs.Num() == 0)
	{
		return;
	}

	const UGGYGOPawnExtensionComponent* PawnExtComp = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(GetPawn<APawn>());
	UGGYGOAbilitySystemComponent* GGYGOASC = PawnExtComp ? PawnExtComp->GetGGYGOAbilitySystemComponent() : nullptr;
	if (!GGYGOASC)
	{
		return;
	}

	const float Now = World->GetTimeSeconds();

	// 先剔除过期项。
	//
	// 过期清理放在组空出这个时机而不是每帧 Tick：缓冲项很少，
	// 而组空出本身就是唯一需要读取缓冲的时刻，为它单独开一个 Tick 不划算。
	BufferedInputs.RemoveAll([this, Now](const FBufferedInput& Buffered)
	{
		return (Now - Buffered.BufferedAtTime) > InputBufferWindow;
	});

	// 重试时不按 GroupTag 筛选缓冲项。
	//
	// 缓冲的是 InputTag，而一个 InputTag 可能对应多个能力、分属不同组，
	// 意图层无从判断它该等哪个组。直接全部重试一遍，由 ASC 的仲裁决定
	// 哪些能通过 —— 那本来就是它的职责，重复判断只会引入不一致。
	//
	// 复制一份再遍历：重试会激活能力，能力可能同帧结束并再次触发本函数，
	// 直接遍历原数组会在迭代中被修改。
	TArray<FBufferedInput> Pending = BufferedInputs;
	BufferedInputs.Reset();

	for (const FBufferedInput& Buffered : Pending)
	{
		GGYGOASC->AbilityInputTagPressed(Buffered.InputTag);
	}
}
