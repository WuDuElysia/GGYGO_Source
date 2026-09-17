/**
 * @file GGYGOPawnExtensionComponent.cpp
 * @brief Pawn 初始化协调者实现
 */
#include "Character/Components/GGYGOPawnExtensionComponent.h"

#include "AbilitySystem/Cues/GGYGOGameplayCueManager.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Components/GameFrameworkComponentManager.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPawnExtensionComponent)

class FLifetimeProperty;
class UActorComponent;

const FName UGGYGOPawnExtensionComponent::NAME_ActorFeatureName("PawnExtension");

UGGYGOPawnExtensionComponent::UGGYGOPawnExtensionComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 本组件是纯协调者，没有任何需要每帧推进的状态。
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	// PawnData 需要复制到客户端，客户端才能推进 DataAvailable。
	SetIsReplicatedByDefault(true);

	PawnData = nullptr;
	AbilitySystemComponent = nullptr;
}

void UGGYGOPawnExtensionComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UGGYGOPawnExtensionComponent, PawnData);
}

void UGGYGOPawnExtensionComponent::OnRegister()
{
	Super::OnRegister();

	const APawn* Pawn = GetPawn<APawn>();
	ensureAlwaysMsgf((Pawn != nullptr),
		TEXT("UGGYGOPawnExtensionComponent 只能挂在 Pawn 上，当前挂在 [%s]。"), *GetNameSafe(GetOwner()));

	// 一个 Pawn 上出现两个协调者会导致 feature 名冲突，
	// Manager 无法区分两者的状态，HaveAllFeaturesReachedInitState 的结果就不可信了。
	TArray<UActorComponent*> PawnExtensionComponents;
	Pawn->GetComponents(UGGYGOPawnExtensionComponent::StaticClass(), PawnExtensionComponents);
	ensureAlwaysMsgf((PawnExtensionComponents.Num() == 1),
		TEXT("[%s] 上只能有一个 UGGYGOPawnExtensionComponent，当前有 %d 个。"),
		*GetNameSafe(GetOwner()), PawnExtensionComponents.Num());

	// 尽早注册。只在 game world 里生效，编辑器预览世界里是空操作。
	RegisterInitStateFeature();
}

void UGGYGOPawnExtensionComponent::BeginPlay()
{
	Super::BeginPlay();

	// 监听**所有** feature 的状态变化（第一个参数 NAME_None 表示不筛选 feature，
	// 第二个参数空 Tag 表示不筛选状态）。因为 DataInitialized 的条件是
	// "所有 feature 都到 DataAvailable"，任何一个 feature 的推进都可能让条件成立。
	BindOnActorInitStateChanged(NAME_None, FGameplayTag(), false);

	// Spawned 只在这里设置一次，之后的推进全靠条件驱动。
	ensure(TryToChangeInitState(GGYGOGameplayTags::InitState_Spawned));
	CheckDefaultInitialization();
}

void UGGYGOPawnExtensionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 先解除 ASC 关联（会回收已授予的能力），再注销 feature。
	// 顺序不能反：注销 feature 后其它组件收不到状态变化，
	// 它们绑在 OnAbilitySystemUninitialized 上的清理逻辑就没机会跑。
	UninitializeAbilitySystem();
	UnregisterInitStateFeature();

	Super::EndPlay(EndPlayReason);
}

void UGGYGOPawnExtensionComponent::SetPawnData(const UGGYGOPawnData* InPawnData)
{
	check(InPawnData);

	APawn* Pawn = GetPawnChecked<APawn>();

	// 客户端不能自己设，否则会和复制过来的值打架。
	if (Pawn->GetLocalRole() != ROLE_Authority)
	{
		return;
	}

	if (PawnData)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SetPawnData: Pawn [%s] 已有 PawnData [%s]，拒绝改为 [%s]。换角色请换 Pawn。"),
			*GetNameSafe(Pawn), *GetNameSafe(PawnData), *GetNameSafe(InPawnData));
		return;
	}

	PawnData = InPawnData;

	// 强制立即同步。默认的复制节流可能让客户端晚几帧才拿到 PawnData，
	// 那期间客户端卡在 Spawned，本地控制的角色会有可感知的输入延迟。
	Pawn->ForceNetUpdate();

	CheckDefaultInitialization();
}

void UGGYGOPawnExtensionComponent::OnRep_PawnData()
{
	CheckDefaultInitialization();
}

void UGGYGOPawnExtensionComponent::InitializeAbilitySystem(UGGYGOAbilitySystemComponent* InASC, AActor* InOwnerActor)
{
	check(InASC);
	check(InOwnerActor);

	APawn* Pawn = GetPawnChecked<APawn>();
	if (AbilitySystemComponent == InASC &&
		InASC->GetOwnerActor() == InOwnerActor &&
		InASC->GetAvatarActor() == Pawn)
	{
		// 只有 ASC、Owner、Avatar 三者都一致才算幂等。
		// 仅比较 ASC 会掩盖外部宿主已经清空/替换 Avatar 的情况。
		return;
	}

	if (AbilitySystemComponent)
	{
		// 换 ASC 前先清理旧的，否则旧 ASC 会一直把本 Pawn 当 Avatar。
		UninitializeAbilitySystem();
	}

	AActor* ExistingAvatar = InASC->GetAvatarActor();

	UE_LOG(LogGGYGOAbilitySystem, Verbose,
		TEXT("InitializeAbilitySystem: ASC [%s] → Pawn [%s]，Owner [%s]，原 Avatar [%s]。"),
		*GetNameSafe(InASC), *GetNameSafe(Pawn), *GetNameSafe(InOwnerActor), *GetNameSafe(ExistingAvatar));

	if ((ExistingAvatar != nullptr) && (ExistingAvatar != Pawn))
	{
		// 该 ASC 已经有别的 Avatar。这在客户端延迟时会发生：
		// 新 Pawn 生成并被附身了，而旧 Pawn 的销毁复制还没到。
		// 服务器上不该出现这种情况，所以用 ensure 把它标出来。
		ensure(!ExistingAvatar->HasAuthority());

		if (UGGYGOPawnExtensionComponent* OtherExtensionComponent = FindPawnExtensionComponent(ExistingAvatar))
		{
			OtherExtensionComponent->UninitializeAbilitySystem();
		}
	}

	AbilitySystemComponent = InASC;
	AbilitySystemComponent->InitAbilityActorInfo(InOwnerActor, Pawn);

	// PawnData 的配置分发**不在这里做**，见 ApplyPawnDataToConsumers 的说明。
	// 这里只建立 ASC 与 Avatar 的关系，让订阅方（HealthComponent、CMC）能拿到 ASC。
	//
	// 但如果 PawnData 已经就绪（关卡里放置的实例在 PostInitializeComponents 前就有值），
	// 顺手分发一次没有坏处，而且能覆盖"InitState 因故没走完"的退化情况。
	ApplyPawnDataToConsumers();

	OnAbilitySystemInitialized.Broadcast();
}

void UGGYGOPawnExtensionComponent::UninitializeAbilitySystem()
{
	if (!AbilitySystemComponent)
	{
		return;
	}

	// 只在"本 Pawn 仍是 Avatar"时清理。
	// 如果 Avatar 已经换成别的 Pawn，说明那个 Pawn 在成为 Avatar 时已经清理过了，
	// 这里再清一遍会把新 Avatar 的能力误取消。
	if (AbilitySystemComponent->GetAvatarActor() == GetOwner())
	{
		// 这里**不**回收 AbilitySet：能力由队伍位置授予，与位置同生命周期。
		// 本 Pawn 只是 Avatar，它下场或销毁不该带走位置上的能力 ——
		// 那正是"待命角色冷却继续走"依赖的前提。

		// 死亡相关能力要能跨过 Avatar 更替继续跑（死亡演出、掉落物结算）。
		FGameplayTagContainer AbilityTypesToIgnore;
		AbilityTypesToIgnore.AddTag(GGYGOGameplayTags::Ability_Behavior_SurvivesDeath);

		AbilitySystemComponent->CancelAbilities(nullptr, &AbilityTypesToIgnore);
		AbilitySystemComponent->ClearAbilityInput();
		AbilitySystemComponent->RemoveAllGameplayCues();

		if (AbilitySystemComponent->GetOwnerActor() != nullptr)
		{
			// Owner 还在，只解除 Avatar 绑定，ASC 本身继续可用
			// （队伍换角色时队伍级 ASC 走这条路）。
			AbilitySystemComponent->SetAvatarActor(nullptr);
		}
		else
		{
			// Owner 都没了，整个 ActorInfo 都得清，只清 Avatar 会留下悬空的 Owner 引用。
			AbilitySystemComponent->ClearActorInfo();
		}

		OnAbilitySystemUninitialized.Broadcast();
	}

	AbilitySystemComponent = nullptr;
}

void UGGYGOPawnExtensionComponent::HandleControllerChanged()
{
	if (AbilitySystemComponent && (AbilitySystemComponent->GetAvatarActor() == GetPawnChecked<APawn>()))
	{
		ensure(AbilitySystemComponent->AbilityActorInfo->OwnerActor == AbilitySystemComponent->GetOwnerActor());

		if (AbilitySystemComponent->GetOwnerActor() == nullptr)
		{
			// Owner 没了（PlayerState 级 ASC 场景下玩家离开），整体反初始化。
			UninitializeAbilitySystem();
		}
		else
		{
			// ActorInfo 里缓存了 Controller、AnimInstance、MovementComponent 等，
			// Controller 换了必须刷新，否则能力里拿到的还是旧 Controller。
			AbilitySystemComponent->RefreshAbilityActorInfo();
		}
	}

	CheckDefaultInitialization();
}

void UGGYGOPawnExtensionComponent::HandlePlayerStateReplicated()
{
	CheckDefaultInitialization();
}

void UGGYGOPawnExtensionComponent::SetupPlayerInputComponent()
{
	CheckDefaultInitialization();
}

void UGGYGOPawnExtensionComponent::ApplyPawnDataToConsumers()
{
	if (!PawnData)
	{
		return;
	}

	// ASC 侧的配置（组规则表、Tag 关系表）不在这里做：它们属于 ASC，
	// 而 ASC 归队伍位置持有，由 `AGGYGOCharacterSlot::InitializeForPawnData` 注入。
	// 在两处都写会让"当前生效的是哪一份配置"取决于两个初始化流程的先后。

	// 移动层。用 FindComponentByClass 而不是要求 Owner 是 ACharacter ——
	// 载具、飞行单位将来可能不是 Character，那时它们没有这个组件，跳过即可。
	if (AActor* Owner = GetOwner())
	{
		if (UGGYGOCharacterMovementComponent* MoveComp = Owner->FindComponentByClass<UGGYGOCharacterMovementComponent>())
		{
			MoveComp->SetMovementSet(PawnData->MovementSet);
		}
	}

	// 预热该角色的特效。Cue 是按需异步加载的，不预热则第一次触发时
	// 资产还没就位，表现为"第一刀没有火花"。
	if (UGGYGOGameplayCueManager* CueManager = UGGYGOGameplayCueManager::Get())
	{
		CueManager->PreloadCuesForTags(PawnData->CuesToPreload);
	}
}

void UGGYGOPawnExtensionComponent::CheckDefaultInitialization()
{
	// 先推进别人再推进自己。
	// 本组件的 DataInitialized 依赖所有 feature 到达 DataAvailable，
	// 如果不先给它们一次推进机会，第一次调用时它们可能还卡在 Spawned，
	// 于是本组件推不动，而它们要等本组件的状态变化才会被再次唤醒 —— 死锁。
	CheckDefaultInitializationForImplementers();

	static const TArray<FGameplayTag> StateChain = {
		GGYGOGameplayTags::InitState_Spawned,
		GGYGOGameplayTags::InitState_DataAvailable,
		GGYGOGameplayTags::InitState_DataInitialized,
		GGYGOGameplayTags::InitState_GameplayReady
	};

	// 一次调用可能连续推进多级，直到某一级的条件不满足为止。
	ContinueInitStateChain(StateChain);
}

bool UGGYGOPawnExtensionComponent::CanChangeInitState(UGameFrameworkComponentManager* Manager, FGameplayTag CurrentState, FGameplayTag DesiredState) const
{
	check(Manager);

	APawn* Pawn = GetPawn<APawn>();

	if (!CurrentState.IsValid() && DesiredState == GGYGOGameplayTags::InitState_Spawned)
	{
		// 挂在合法 Pawn 上就算 Spawned。
		return Pawn != nullptr;
	}

	if (CurrentState == GGYGOGameplayTags::InitState_Spawned && DesiredState == GGYGOGameplayTags::InitState_DataAvailable)
	{
		// PawnData 是硬性前提：没有它就不知道该授予什么能力。
		if (!PawnData)
		{
			return false;
		}

		const bool bHasAuthority = Pawn->HasAuthority();
		const bool bIsLocallyControlled = Pawn->IsLocallyControlled();

		if (bHasAuthority || bIsLocallyControlled)
		{
			// 只有服务器和本地控制端需要等 Controller。
			// 模拟代理（别人的角色在我的客户端上）永远等不到 Controller，
			// 若一并要求就会卡在 Spawned，它身上的动画与表现组件全都初始化不了。
			if (!GetController<AController>())
			{
				return false;
			}
		}

		return true;
	}

	if (CurrentState == GGYGOGameplayTags::InitState_DataAvailable && DesiredState == GGYGOGameplayTags::InitState_DataInitialized)
	{
		// 等齐所有 feature。这一句就是"不必手工排初始化顺序"的全部原因。
		return Manager->HaveAllFeaturesReachedInitState(Pawn, GGYGOGameplayTags::InitState_DataAvailable);
	}

	if (CurrentState == GGYGOGameplayTags::InitState_DataInitialized && DesiredState == GGYGOGameplayTags::InitState_GameplayReady)
	{
		return true;
	}

	return false;
}

void UGGYGOPawnExtensionComponent::HandleChangeInitState(UGameFrameworkComponentManager* Manager, FGameplayTag CurrentState, FGameplayTag DesiredState)
{
	if (DesiredState == GGYGOGameplayTags::InitState_DataInitialized)
	{
		// 配置分发放在这一步，而不是 InitializeAbilitySystem 里。
		//
		// 原因是时序：InitializeAbilitySystem 由 Pawn 在 PostInitializeComponents 调用，
		// 那时 PawnData 可能还没设置（运行时生成的角色是先 SpawnActor 再 SetPawnData）。
		// 而 DataInitialized 的前置条件里包含 DataAvailable，后者要求 PawnData 非空，
		// 所以走到这里 PawnData 一定有值。
		ApplyPawnDataToConsumers();
	}
}

void UGGYGOPawnExtensionComponent::OnActorInitStateChanged(const FActorInitStateChangedParams& Params)
{
	// 别人到了 DataAvailable，重新尝试推进自己 —— 可能正是它让"所有 feature 齐了"成立。
	// 排除自己是为了避免递归：本组件推进时也会触发这个回调。
	if (Params.FeatureName != NAME_ActorFeatureName)
	{
		if (Params.FeatureState == GGYGOGameplayTags::InitState_DataAvailable)
		{
			CheckDefaultInitialization();
		}
	}
}

void UGGYGOPawnExtensionComponent::OnAbilitySystemInitialized_RegisterAndCall(FSimpleMulticastDelegate::FDelegate Delegate)
{
	if (!OnAbilitySystemInitialized.IsBoundToObject(Delegate.GetUObject()))
	{
		OnAbilitySystemInitialized.Add(Delegate);
	}

	// 补发已经发生的那次广播。见头文件里对时序竞态的说明。
	if (AbilitySystemComponent)
	{
		Delegate.Execute();
	}
}

void UGGYGOPawnExtensionComponent::OnAbilitySystemUninitialized_Register(FSimpleMulticastDelegate::FDelegate Delegate)
{
	if (!OnAbilitySystemUninitialized.IsBoundToObject(Delegate.GetUObject()))
	{
		OnAbilitySystemUninitialized.Add(Delegate);
	}
}
