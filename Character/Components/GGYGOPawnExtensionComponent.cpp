/**
 * @file GGYGOPawnExtensionComponent.cpp
 * @brief Pawn 初始化协调者实现
 */
#include "Character/Components/GGYGOPawnExtensionComponent.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
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

	if (AbilitySystemComponent == InASC)
	{
		// 幂等：同一个 ASC 重复初始化直接返回。
		// Pawn 的 PostInitializeComponents 与 Controller 变更都可能触发这条路径。
		return;
	}

	if (AbilitySystemComponent)
	{
		// 换 ASC 前先清理旧的，否则旧 ASC 会一直把本 Pawn 当 Avatar。
		UninitializeAbilitySystem();
	}

	APawn* Pawn = GetPawnChecked<APawn>();
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

	// 注入 PawnData 里的两张配置表。
	// 必须在 InitAbilityActorInfo 之后：那之前 ASC 还不知道自己属于谁，
	// 而两张表的消费方（仲裁、Tag 关系扩展）都需要 ActorInfo 有效。
	if (PawnData)
	{
		AbilitySystemComponent->SetAbilityGroupConfig(PawnData->AbilityGroupConfig);
		AbilitySystemComponent->SetTagRelationshipMapping(PawnData->TagRelationshipMapping);
	}

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
		// 回收本组件授予的能力。必须在 CancelAbilities 之前 ——
		// ClearAbility 内部会结束正在激活的实例，反过来就会留下已取消但未移除的 spec。
		GrantedHandles.TakeFromAbilitySystem(AbilitySystemComponent);
		bAbilitySetsGranted = false;

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

void UGGYGOPawnExtensionComponent::GrantAbilitySets()
{
	// 授予是服务器权威操作，客户端靠 AbilitySpec 的复制拿到能力。
	if (!AbilitySystemComponent || !PawnData || bAbilitySetsGranted)
	{
		return;
	}

	if (GetOwner()->GetLocalRole() != ROLE_Authority)
	{
		return;
	}

	for (const TObjectPtr<UGGYGOAbilitySet>& AbilitySet : PawnData->AbilitySets)
	{
		if (AbilitySet)
		{
			// SourceObject 传 PawnData：能力里可以由此回溯自己的配置来源，
			// 比传 Pawn 更有用（Pawn 用 ActorInfo 就能拿到）。
			AbilitySet->GiveToAbilitySystem(AbilitySystemComponent, &GrantedHandles, const_cast<UGGYGOPawnData*>(PawnData.Get()));
		}
	}

	bAbilitySetsGranted = true;
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
		// 在这一步授予能力，而不是在 InitializeAbilitySystem 里，原因有两个：
		// 1. 语义上"授予能力"正是数据初始化这件事本身
		// 2. 到这一步所有 feature 都已 DataAvailable，能力激活时依赖的其它组件
		//    （HealthComponent 等）都已就绪，OnSpawn 策略的能力可以安全激活
		GrantAbilitySets();
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
