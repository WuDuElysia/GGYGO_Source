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
#include "Character/Interfaces/GGYGOAvatarBindingHostInterface.h"
#include "Components/GameFrameworkComponentManager.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPawnExtensionComponent)

class FLifetimeProperty;
class UActorComponent;

struct UGGYGOPawnExtensionComponent::FPawnDataInitializationScope final
{
	explicit FPawnDataInitializationScope(UGGYGOPawnExtensionComponent& InExtension)
		: Extension(&InExtension), Pawn(InExtension.GetPawn<APawn>()), World(InExtension.GetWorld()),
		  Previous(InExtension.PawnDataInitializationScope)
	{
		if (IsInGameThread() && !InExtension.bPawnDataInitializationClosed && IsValid(&InExtension)
			&& InExtension.IsRegistered() && !InExtension.IsBeingDestroyed()
			&& !InExtension.HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& Pawn.IsValid() && !Pawn->IsActorBeingDestroyed() && World.IsValid()
			&& World->IsGameWorld() && Pawn->GetWorld() == World.Get())
		{
			InExtension.PawnDataInitializationScope = this;
		}
	}

	~FPawnDataInitializationScope()
	{
		// A lifecycle invalidation or a successor scope must not restore this old call's parent.
		if (UGGYGOPawnExtensionComponent* OriginalExtension = Extension.Get();
			OriginalExtension && OriginalExtension->PawnDataInitializationScope == this)
		{
			OriginalExtension->PawnDataInitializationScope = Previous;
		}
	}

	FPawnDataInitializationScope(const FPawnDataInitializationScope&) = delete;
	FPawnDataInitializationScope& operator=(const FPawnDataInitializationScope&) = delete;

	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> Extension;
	const TWeakObjectPtr<APawn> Pawn;
	const TWeakObjectPtr<UWorld> World;
	const FPawnDataInitializationScope* const Previous;
};

/** 一次订阅的共享状态；分发时被钉住，回调中退订不会销毁正在执行的回调。 */
struct FGGYGOLocalASCSubscription::FState
{
	TWeakObjectPtr<UGGYGOPawnExtensionComponent> Extension;
	FCallback Callback;
	FGGYGOPawnASCResourceHandle Consumed;
	bool bActive = true;

	void Deliver(const FGGYGOPawnASCNotice& Notice)
	{
		if (!bActive) { return; }
		if (Notice.Kind == EGGYGOPawnASCNoticeKind::Ready)
		{
			if (Consumed.HasSameResource(Notice.Resource)) { return; }
			Consumed = Notice.Resource;
		}
		else
		{
			if (!Consumed.HasSameResource(Notice.Resource)) { return; }
			Consumed = FGGYGOPawnASCResourceHandle{};
		}
		Callback(Notice);
	}
};

bool FGGYGOLocalASCSubscription::Subscribe(UGGYGOPawnExtensionComponent* Extension, FCallback Callback)
{
	check(IsInGameThread());
	Unsubscribe();
	if (!IsValid(Extension) || !Callback) { return false; }
	const TSharedPtr<FState> NewState = MakeShared<FState>();
	NewState->Extension = Extension;
	NewState->Callback = MoveTemp(Callback);
	State = NewState;
	Extension->LocalASCSubscribers.Add(NewState);
	if (Extension->LocalAbilitySystemResource.HasResource())
	{
		FGGYGOPawnASCNotice Replay;
		Replay.Kind = EGGYGOPawnASCNoticeKind::Ready;
		Replay.Resource = Extension->LocalAbilitySystemResource;
		NewState->Deliver(Replay);
	}
	return true;
}

void FGGYGOLocalASCSubscription::Unsubscribe()
{
	const TSharedPtr<FState> OldState = MoveTemp(State);
	State.Reset();
	if (!OldState.IsValid()) { return; }
	OldState->bActive = false;
	OldState->Consumed = FGGYGOPawnASCResourceHandle{};
	if (UGGYGOPawnExtensionComponent* Extension = OldState->Extension.Get())
	{
		Extension->LocalASCSubscribers.RemoveAll([&OldState](const TWeakPtr<FState>& Subscriber)
		{
			return !Subscriber.IsValid() || Subscriber.HasSameObject(OldState.Get());
		});
	}
}

bool FGGYGOLocalASCSubscription::IsSubscribed() const
{
	return State.IsValid() && State->bActive;
}

UGGYGOPawnExtensionComponent* FGGYGOLocalASCSubscription::GetExtension() const
{
	return State.IsValid() ? State->Extension.Get() : nullptr;
}

UGGYGOAbilitySystemComponent* FGGYGOLocalASCSubscription::GetReadyASC() const
{
	const UGGYGOPawnExtensionComponent* Extension = GetExtension();
	return Extension && Extension->IsLocalAbilitySystemResourceReady(State->Consumed)
		? State->Consumed.GetIdentity().ASC.Get() : nullptr;
}

FGGYGOPawnASCResourceHandle FGGYGOLocalASCSubscription::GetConsumedResource() const
{
	return State.IsValid() ? State->Consumed : FGGYGOPawnASCResourceHandle{};
}

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

void UGGYGOPawnExtensionComponent::OnUnregister()
{
	PawnDataInitializationScope = nullptr;
	Super::OnUnregister();
}

void UGGYGOPawnExtensionComponent::BeginPlay()
{
	// 首次 PreBegin 默认开放；只在真实组件 BeginPlay 入口重开上一生命周期。
	APawn* Pawn = GetPawn<APawn>();
	if (!HasBegunPlay() && IsRegistered() && IsValid(this) && !IsBeingDestroyed()
		&& Pawn && !Pawn->IsActorBeingDestroyed()
		&& (Pawn->IsActorBeginningPlay() || Pawn->HasActorBegunPlay()))
	{
		bPawnDataInitializationClosed = false;
	}
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
	bPawnDataInitializationClosed = true;
	PawnDataInitializationScope = nullptr;
	// 宿主监听 Pawn 的 OnEndPlay，先于组件 EndPlay 释放绑定。仍有资源说明宿主未执行释放，
	// 这里只让本地消费方清理自身状态，不写 ASC。
	if (LocalAbilitySystemResource.HasResource())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Character/PawnExtension] EndPlay: Extension=%s 仍持有 ASC=%s 的绑定资源，宿主未释放；只通知本地消费方。"),
			*GetPathNameSafe(this), *GetPathNameSafe(LocalAbilitySystemResource.GetIdentity().ASC.Get()));
		HandleAbilitySystemReleased();
	}
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

UGGYGOAbilitySystemComponent* UGGYGOPawnExtensionComponent::GetGGYGOAbilitySystemComponent() const
{
	check(IsInGameThread());
	return IsLocalAbilitySystemResourceReady(LocalAbilitySystemResource)
		? LocalAbilitySystemResource.GetIdentity().ASC.Get() : nullptr;
}

void UGGYGOPawnExtensionComponent::UninitializeAbilitySystem()
{
	check(IsInGameThread());
	if (!LocalAbilitySystemResource.HasResource()) { return; }
	UGGYGOAbilitySystemComponent* ASC = LocalAbilitySystemResource.GetIdentity().ASC.Get();
	if (!ASC)
	{
		// ASC 已不存在，没有可释放的 ActorInfo；只让本地消费方清理。
		HandleAbilitySystemReleased();
		return;
	}
	if (IGGYGOAvatarBindingHostInterface* Host = Cast<IGGYGOAvatarBindingHostInterface>(ASC->GetOwner()))
	{
		Host->ReleaseAvatarBinding(GetPawn<APawn>());
		return;
	}
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("[Character/PawnExtension] UninitializeAbilitySystem: ASC=%s 的 Owner=%s 未实现 IGGYGOAvatarBindingHostInterface，无法释放 Pawn=%s。"),
		*GetPathNameSafe(ASC), *GetPathNameSafe(ASC->GetOwner()), *GetPathNameSafe(GetOwner()));
}

void UGGYGOPawnExtensionComponent::HandleControllerChanged()
{
	check(IsInGameThread());
	// ActorInfo 缓存了 PlayerController；Owner／Avatar 不变，只刷新，不经过宿主。
	if (UGGYGOAbilitySystemComponent* ASC = GetGGYGOAbilitySystemComponent())
	{
		ASC->RefreshAbilityActorInfo();
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
	const FPawnDataInitializationScope InitializationScope(*this);
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
	// Include synchronous implementer callbacks before our own DataInitialized distribution.
	const FPawnDataInitializationScope InitializationScope(*this);
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

bool UGGYGOPawnExtensionComponent::IsPawnDataInitializationInProgress() const
{
	if (!IsInGameThread() || bPawnDataInitializationClosed || !IsValid(this) || !IsRegistered()
		|| IsBeingDestroyed() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !PawnDataInitializationScope)
	{
		return false;
	}
	const APawn* OriginalPawn = GetPawn<APawn>();
	const UWorld* OriginalWorld = GetWorld();
	return IsValid(OriginalPawn) && !OriginalPawn->IsActorBeingDestroyed()
		&& IsValid(OriginalWorld) && OriginalWorld->IsGameWorld()
		&& OriginalPawn->GetWorld() == OriginalWorld
		&& PawnDataInitializationScope->Extension.Get() == this
		&& PawnDataInitializationScope->Pawn.Get() == OriginalPawn
		&& PawnDataInitializationScope->World.Get() == OriginalWorld;
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
		// 配置分发放在这一步，而不是 ASC 绑定时。
		//
		// 原因是时序：Host 绑定与 PawnData 到达相互独立，
		// 运行时生成的角色可能先 SpawnActor 再 SetPawnData。
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

bool UGGYGOPawnExtensionComponent::HandleAbilitySystemBound(UGGYGOAbilitySystemComponent* ASC)
{
	check(IsInGameThread());
	APawn* Pawn = GetPawn<APawn>();
	if (!IsValid(ASC) || !Pawn || ASC->GetAvatarActor() != Pawn)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Character/PawnExtension] HandleAbilitySystemBound: Extension=%s ASC=%s 的 Avatar=%s 不是本 Pawn，拒绝就绪。"),
			*GetPathNameSafe(this), *GetPathNameSafe(ASC), *GetPathNameSafe(ASC ? ASC->GetAvatarActor() : nullptr));
		return false;
	}
	if (LocalAbilitySystemResource.HasResource())
	{
		UGGYGOAbilitySystemComponent* CurrentASC = LocalAbilitySystemResource.GetIdentity().ASC.Get();
		if (CurrentASC == ASC) { return true; }
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Character/PawnExtension] HandleAbilitySystemBound: Extension=%s 仍绑定 ASC=%s，拒绝改为 ASC=%s；跨宿主须先释放。"),
			*GetPathNameSafe(this), *GetPathNameSafe(CurrentASC), *GetPathNameSafe(ASC));
		return false;
	}
	FGGYGOPawnASCResourceIdentity Identity;
	Identity.ASC = ASC;
	Identity.Pawn = Pawn;
	FGGYGOPawnASCNotice Notice;
	Notice.Kind = EGGYGOPawnASCNoticeKind::Ready;
	Notice.Resource.Resource = MakeShared<const FGGYGOPawnASCResourceIdentity>(Identity);
	LocalAbilitySystemResource = Notice.Resource;
	BroadcastLocalASCNotice(Notice);
	return true;
}

void UGGYGOPawnExtensionComponent::HandleAbilitySystemReleased()
{
	check(IsInGameThread());
	if (!LocalAbilitySystemResource.HasResource()) { return; }
	FGGYGOPawnASCNotice Notice;
	Notice.Kind = EGGYGOPawnASCNoticeKind::Released;
	Notice.Resource = LocalAbilitySystemResource;
	// 先清空：回调里的就绪查询已看不到这次绑定，重新绑定会新建资源。
	LocalAbilitySystemResource = FGGYGOPawnASCResourceHandle{};
	BroadcastLocalASCNotice(Notice);
}

void UGGYGOPawnExtensionComponent::BroadcastLocalASCNotice(const FGGYGOPawnASCNotice& Notice)
{
	// 回调可能退订、重新订阅或触发重新绑定：复制列表，Ready 只在仍是当前资源时继续分发。
	const TArray<TWeakPtr<FGGYGOLocalASCSubscription::FState>> Subscribers = LocalASCSubscribers;
	for (const TWeakPtr<FGGYGOLocalASCSubscription::FState>& Subscriber : Subscribers)
	{
		if (Notice.Kind == EGGYGOPawnASCNoticeKind::Ready
			&& !LocalAbilitySystemResource.HasSameResource(Notice.Resource))
		{
			return;
		}
		if (const TSharedPtr<FGGYGOLocalASCSubscription::FState> State = Subscriber.Pin())
		{
			State->Deliver(Notice);
		}
	}
}

FGGYGOPawnASCResourceHandle UGGYGOPawnExtensionComponent::GetCurrentLocalAbilitySystemResource() const
{
	check(IsInGameThread());
	return LocalAbilitySystemResource;
}

bool UGGYGOPawnExtensionComponent::IsLocalAbilitySystemResourceReady(
	const FGGYGOPawnASCResourceHandle& ExpectedResource) const
{
	check(IsInGameThread());
	const UGGYGOAbilitySystemComponent* ASC = ExpectedResource.GetIdentity().ASC.Get();
	return LocalAbilitySystemResource.HasSameResource(ExpectedResource)
		&& ASC && ASC->GetAvatarActor() == GetOwner();
}
