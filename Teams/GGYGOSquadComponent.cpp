/**
 * @file GGYGOSquadComponent.cpp
 * @brief 队伍与出战切换实现
 */
#include "Teams/GGYGOSquadComponent.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "Character/Components/GGYGOHeroComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Character/GGYGOCharacterBase.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Misc/ScopeExit.h"
#include "Net/UnrealNetwork.h"
#include "Player/GGYGOPlayerState.h"
#include "System/GGYGOGameplayTags.h"
#include "Teams/GGYGOCharacterSlot.h"
#include "Teams/GGYGOSquadTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOSquadComponent)

class FLifetimeProperty;

UGGYGOSquadComponent::UGGYGOSquadComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;
	PrimaryComponentTick.bStartWithTickEnabled = false;

	SetIsReplicatedByDefault(true);
}

void UGGYGOSquadComponent::DestroySquad()
{
	ConsumeCreatedSlotResources(false);
}

void UGGYGOSquadComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ConsumeCreatedSlotResources(false);
	Super::EndPlay(EndPlayReason);
}

void UGGYGOSquadComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	ConsumeCreatedSlotResources(true);
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UGGYGOSquadComponent::ConsumeCreatedSlotResources(bool bFinalComponentDestruction)
{
	check(IsInGameThread());
	bFinalComponentDestructionRequested |= bFinalComponentDestruction;
	if (bConsumingCreatedSlotResources)
	{
		// 最终销毁通知交给外层责任栈；不重复取得或请求销毁原资源。
		return;
	}

	const bool bFirstTermination = !bSquadTerminationStarted;
	if (!bFirstTermination && CreatedSlotResources.IsEmpty())
	{
		return;
	}

	const TWeakObjectPtr<UGGYGOSquadComponent> OriginalSquad(this);
	const FString SquadPath = GetPathName();
	const TWeakObjectPtr<AGGYGOPlayerState> OriginalPlayerState(
		bFirstTermination ? Cast<AGGYGOPlayerState>(GetOwner()) : nullptr);
	const TWeakObjectPtr<APlayerController> OriginalController(
		bFirstTermination ? GetRegistrationController() : nullptr);
	const TWeakObjectPtr<AGGYGOCharacterSlot> OriginalActiveSlot(
		bFirstTermination ? GetActiveSlot() : nullptr);
	const TWeakObjectPtr<APawn> OriginalActivePawn(
		OriginalActiveSlot.IsValid() ? OriginalActiveSlot->GetAvatarPawn() : nullptr);

	bSquadTerminationStarted = true;
	bConsumingCreatedSlotResources = true;
	TArray<FCreatedSlotResources> OriginalResources = MoveTemp(CreatedSlotResources);
	CreatedSlotResources.Reset();
	ReleaseSlotRuntimes();
	Roster.Reset();
	Slots.Reset();
	ActiveSlotIndex = INDEX_NONE;

	// 捕获的 Avatar 只用于归还原控制关系，绝不据此决定 Actor 创建责任。
	if (bFirstTermination)
	{
		AGGYGOPlayerState* PlayerState = OriginalPlayerState.Get();
		APlayerController* Controller = OriginalController.Get();
		AGGYGOCharacterSlot* ActiveSlot = OriginalActiveSlot.Get();
		APawn* ActivePawn = OriginalActivePawn.Get();
		if (PlayerState && Controller && ActiveSlot && ActivePawn &&
			!PlayerState->IsActorBeingDestroyed() && !Controller->IsActorBeingDestroyed() &&
			!ActiveSlot->IsActorBeingDestroyed() && !ActivePawn->IsActorBeingDestroyed() &&
			PlayerState->GetSquadComponent() == this && GetOwner() == PlayerState &&
			PlayerState->GetOwner() == Controller &&
			Controller->GetPlayerState<AGGYGOPlayerState>() == PlayerState &&
			PlayerState->HasAuthority() && Controller->HasAuthority() &&
			ActiveSlot->HasAuthority() && ActivePawn->HasAuthority() &&
			PlayerState->GetWorld() == GetWorld() && Controller->GetWorld() == PlayerState->GetWorld() &&
			ActiveSlot->GetWorld() == PlayerState->GetWorld() &&
			ActivePawn->GetWorld() == PlayerState->GetWorld() &&
			ActiveSlot->GetOwner() == Controller && ActivePawn->GetOwner() == Controller &&
			Controller->GetPawn() == ActivePawn && ActivePawn->GetController() == Controller &&
			ActiveSlot->GetAvatarPawn() == ActivePawn)
		{
			Controller->UnPossess();
		}

		if (UGGYGOSquadComponent* LiveSquad = OriginalSquad.Get())
		{
			LiveSquad->OnActiveCharacterChanged.Broadcast(nullptr);
		}
	}

	// 闭包不借用 this：回调即使销毁组件，当前栈仍只处理已移出的原 Actor 身份。
	const auto RequestOriginalActorDestruction = [&SquadPath](const auto& OriginalActor, const TCHAR* ResourceKind)
	{
		AActor* Actor = OriginalActor.Get();
		if (!Actor || Actor->IsActorBeingDestroyed())
		{
			return true; // 原对象已退出，或销毁责任已由原生生命周期持有。
		}
		const FString ActorPath = Actor->GetPathName();
		if (!Actor->HasAuthority())
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Teams] DestroySquad: Squad=%s Resource=%s Actor=%s Reason=AuthorityDenied; original resource retained."),
				*SquadPath, ResourceKind, *ActorPath);
			return false;
		}

		const bool bNativeAccepted = Actor->Destroy();
		Actor = OriginalActor.Get();
		if (bNativeAccepted || !Actor || Actor->IsActorBeingDestroyed())
		{
			// 接受可包含 BeginPlay 延迟；这里只移交责任，不证明物理清理完成。
			return true;
		}
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] DestroySquad: Squad=%s Resource=%s Actor=%s Reason=NativeDestroyRejected; original resource retained."),
			*SquadPath, ResourceKind, *ActorPath);
		return false;
	};

	for (FCreatedSlotResources& Resources : OriginalResources)
	{
		if (RequestOriginalActorDestruction(Resources.Slot, TEXT("OriginalSlot")))
		{
			Resources.Slot.Reset();
		}
		if (RequestOriginalActorDestruction(Resources.OriginalCreatedPawn, TEXT("OriginalCreatedPawn")))
		{
			Resources.OriginalCreatedPawn.Reset();
		}
	}

	// 后续原 Actor 回调也可能让先前拒绝的对象退出；不再请求，只核对原弱身份。
	for (FCreatedSlotResources& Resources : OriginalResources)
	{
		if (!Resources.Slot.IsValid() || Resources.Slot->IsActorBeingDestroyed())
		{
			Resources.Slot.Reset();
		}
		if (!Resources.OriginalCreatedPawn.IsValid() || Resources.OriginalCreatedPawn->IsActorBeingDestroyed())
		{
			Resources.OriginalCreatedPawn.Reset();
		}
	}
	OriginalResources.RemoveAll([](const FCreatedSlotResources& Resources)
	{
		return Resources.Slot.IsExplicitlyNull() && Resources.OriginalCreatedPawn.IsExplicitlyNull();
	});

	UGGYGOSquadComponent* LiveSquad = OriginalSquad.Get();
	if (!LiveSquad || LiveSquad->bFinalComponentDestructionRequested)
	{
		const auto ReportUnretainedOriginalActor = [&SquadPath](const auto& OriginalActor, const TCHAR* ResourceKind)
		{
			if (AActor* Actor = OriginalActor.Get())
			{
				UE_LOG(LogGGYGOAbilitySystem, Error,
					TEXT("[Teams] DestroySquad: Squad=%s Resource=%s Actor=%s Reason=FinalComponentDestruction; original destruction obligation cannot outlive this component."),
					*SquadPath, ResourceKind, *Actor->GetPathName());
			}
		};
		for (const FCreatedSlotResources& Resources : OriginalResources)
		{
			ReportUnretainedOriginalActor(Resources.Slot, TEXT("OriginalSlot"));
			ReportUnretainedOriginalActor(Resources.OriginalCreatedPawn, TEXT("OriginalCreatedPawn"));
		}
	}
	if (UGGYGOSquadComponent* SurvivingSquad = OriginalSquad.Get())
	{
		SurvivingSquad->CreatedSlotResources = MoveTemp(OriginalResources);
		SurvivingSquad->bConsumingCreatedSlotResources = false;
	}
}

void UGGYGOSquadComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UGGYGOSquadComponent, Roster);
	DOREPLIFETIME(UGGYGOSquadComponent, Slots);
	DOREPLIFETIME(UGGYGOSquadComponent, ActiveSlotIndex);
}

bool UGGYGOSquadComponent::SetRoster(const TArray<UGGYGOPawnData*>& InRoster)
{
	if (bSquadTerminationStarted || !IsValid(this) || IsBeingDestroyed())
	{
		return false;
	}
	AActor* Owner = GetOwner();
	if (!Owner || Owner->GetLocalRole() != ROLE_Authority)
	{
		return false;
	}

	if (IsSquadAssembled())
	{
		// 装配之后改名单没有正确语义：位置上的属性集是默认子对象，
		// 换角色时数值会残留；已授予的能力也无法干净地对应到另一份 PawnData。
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("SetRoster: 队伍已装配（%d 个位置），拒绝修改编队。换编队请在下一局开始前进行。"),
			Slots.Num());
		return false;
	}

	if (InRoster.IsEmpty())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("SetRoster: 收到空名单，忽略。"));
		return false;
	}

	if (!GGYGOSquad::IsMemberCountWithinCapacity(InRoster.Num()))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SetRoster: 原始名单有 %d 项，超过上限 %d，已拒绝，原名单不变。"),
			InRoster.Num(), GGYGO_MAX_SQUAD_SIZE);
		return false;
	}

	TArray<TObjectPtr<const UGGYGOPawnData>> CandidateRoster;
	CandidateRoster.Reserve(InRoster.Num());

	for (UGGYGOPawnData* PawnData : InRoster)
	{
		if (!PawnData)
		{
			// 空项跳过而不是整体拒绝：编成界面允许留空位，
			// 那时名单里会出现 null，把它当成"这个位置不带人"更合理。
			continue;
		}

		CandidateRoster.Add(PawnData);
	}

	if (CandidateRoster.IsEmpty())
	{
		return false;
	}

	Roster = MoveTemp(CandidateRoster);
	return true;
}

AGGYGOCharacterSlot* UGGYGOSquadComponent::GetActiveSlot() const
{
	return Slots.IsValidIndex(ActiveSlotIndex) ? Slots[ActiveSlotIndex] : nullptr;
}

AGGYGOCharacterSlot* UGGYGOSquadComponent::GetSlot(int32 SlotIndex) const
{
	return Slots.IsValidIndex(SlotIndex) ? Slots[SlotIndex] : nullptr;
}

AGGYGOCharacterBase* UGGYGOSquadComponent::GetActiveCharacter() const
{
	const AGGYGOCharacterSlot* ActiveSlot = GetActiveSlot();
	return ActiveSlot ? Cast<AGGYGOCharacterBase>(ActiveSlot->GetAvatarPawn()) : nullptr;
}

bool UGGYGOSquadComponent::CanSlotBeActive(const AGGYGOCharacterSlot* Slot) const
{
	if (!Slot)
	{
		return false;
	}

	// 没有实体的位置不能出战。Pawn 可能尚未生成或已销毁，
	// 此时位置的属性与冷却仍在，但没有可附身的目标。
	AGGYGOCharacterBase* Character = Cast<AGGYGOCharacterBase>(Slot->GetAvatarPawn());
	if (!Character)
	{
		return false;
	}

	// 已死亡的位置不能出战。切人到尸体上会让玩家失去控制且无法切回。
	if (const UGGYGOHealthComponent* HealthComponent = Character->GetHealthComponent())
	{
		if (HealthComponent->IsDeadOrDying())
		{
			return false;
		}
	}

	return true;
}

bool UGGYGOSquadComponent::RegisterSlot(AGGYGOCharacterSlot* Slot)
{
	return RegisterSlotInternal(Slot, nullptr, false);
}

bool UGGYGOSquadComponent::RegisterCreatedSlot(AGGYGOCharacterSlot* Slot, APawn* OriginalCreatedPawn)
{
	return RegisterSlotInternal(Slot, OriginalCreatedPawn, true);
}

APlayerController* UGGYGOSquadComponent::GetRegistrationController() const
{
	if (bSquadTerminationStarted || !IsValid(this) || IsBeingDestroyed())
	{
		return nullptr;
	}

	const UWorld* World = GetWorld();
	const AGGYGOPlayerState* OwningPlayerState = Cast<AGGYGOPlayerState>(GetOwner());
	if (!World || !IsValid(OwningPlayerState) || OwningPlayerState->IsActorBeingDestroyed() ||
		!OwningPlayerState->HasAuthority() || OwningPlayerState->GetWorld() != World ||
		OwningPlayerState->GetSquadComponent() != this)
	{
		return nullptr;
	}

	APlayerController* OwningController = Cast<APlayerController>(OwningPlayerState->GetOwner());
	if (!IsValid(OwningController) || OwningController->IsActorBeingDestroyed() ||
		!OwningController->HasAuthority() || OwningController->GetWorld() != World ||
		OwningController->GetPlayerState<AGGYGOPlayerState>() != OwningPlayerState)
	{
		return nullptr;
	}

	return OwningController;
}

bool UGGYGOSquadComponent::HasValidSlotBinding(const AGGYGOCharacterSlot* Slot, const APlayerController* OwningController) const
{
	if (!IsValid(Slot) || Slot->IsActorBeingDestroyed() || !Slot->HasAuthority() ||
		Slot->GetWorld() != GetWorld() || Slot->GetOwner() != OwningController ||
		!Slot->IsPawnDataInitializationComplete())
	{
		return false;
	}

	const UGGYGOAbilitySystemComponent* ASC = Slot->GetGGYGOAbilitySystemComponent();
	const AGGYGOCharacterBase* Avatar = Cast<AGGYGOCharacterBase>(Slot->GetAvatarPawn());
	// Slot owns the persistent squad connection. Pawn Owner is native control state:
	// APawn::UnPossessed clears it while the same Slot/ASC/Avatar binding remains valid.
	if (!IsValid(ASC) || ASC->IsBeingDestroyed() || ASC->GetOwnerActor() != Slot ||
		!IsValid(Avatar) || Avatar->IsActorBeingDestroyed() || !Avatar->HasAuthority() ||
		Avatar->GetWorld() != GetWorld() ||
		ASC->GetAvatarActor() != Avatar ||
		(Avatar->GetController() && Avatar->GetController() != OwningController))
	{
		return false;
	}

	const UGGYGOPawnExtensionComponent* PawnExtension =
		UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Avatar);
	return IsValid(PawnExtension) && !PawnExtension->IsBeingDestroyed() &&
		PawnExtension->GetGGYGOAbilitySystemComponent() == ASC &&
		PawnExtension->GetPawnData<UGGYGOPawnData>() == Slot->GetPawnData();
}

bool UGGYGOSquadComponent::RegisterSlotInternal(AGGYGOCharacterSlot* Slot, APawn* OriginalCreatedPawn, bool bAcceptCreatedResources)
{
	APlayerController* OwningController = GetRegistrationController();
	if (!OwningController || !Slot || (bAcceptCreatedResources && !OriginalCreatedPawn))
	{
		return false;
	}

	const TWeakObjectPtr<AGGYGOCharacterSlot> SlotIdentity(Slot);
	const TWeakObjectPtr<APawn> OriginalPawnIdentity(OriginalCreatedPawn);
	const FCreatedSlotResources* ExistingResources = CreatedSlotResources.FindByPredicate(
		[&SlotIdentity](const FCreatedSlotResources& Resources)
		{
			return Resources.Slot.HasSameIndexAndSerialNumber(SlotIdentity);
		});

	if (Slots.Contains(Slot))
	{
		// 重复接收不重放表现；创建责任只匹配原始身份，不按当前 Avatar 补算。
		return !bAcceptCreatedResources ||
			(ExistingResources && ExistingResources->OriginalCreatedPawn.HasSameIndexAndSerialNumber(OriginalPawnIdentity));
	}

	// 尚未清理的创建记录不能被重新接收覆盖，或降级成借用关系。
	if (ExistingResources || !HasValidSlotBinding(Slot, OwningController))
	{
		return false;
	}

	if (!GGYGOSquad::IsMemberCountWithinCapacity(Slots.Num()) || Slots.Num() >= GGYGO_MAX_SQUAD_SIZE)
	{
		// 在装配阶段拒绝而不是接受后再承担复制开销：每个位置带一个 ASC，
		// 配置越界的代价是成倍的网络流量，而那要到压测时才会被发现。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("RegisterSlot: 队伍已满（上限 %d），拒绝登记 [%s]。请检查玩法配置的队伍规模。"),
			GGYGO_MAX_SQUAD_SIZE, *GetNameSafe(Slot));
		return false;
	}

	if (bAcceptCreatedResources && Slot->GetAvatarPawn() != OriginalCreatedPawn)
	{
		return false;
	}

	if (bAcceptCreatedResources && CreatedSlotResources.ContainsByPredicate(
		[&OriginalPawnIdentity](const FCreatedSlotResources& Resources)
		{
			return Resources.OriginalCreatedPawn.HasSameIndexAndSerialNumber(OriginalPawnIdentity);
		}))
	{
		// 原 Pawn 即使已不再是旧 Slot 的 Avatar，也不能被第二份创建责任重复接收。
		return false;
	}

	// 两份接收结果先一起提交；下面的移动/附身/广播回调可以同步重入。
	Slots.Add(Slot);
	FSlotRuntime& Runtime = SlotRuntimes.AddDefaulted_GetRef();
	Runtime.ObservedASC = Slot->GetGGYGOAbilitySystemComponent();
	Runtime.AbilityEndedHandle = Runtime.ObservedASC->OnAbilityEnded.AddUObject(
		this, &ThisClass::HandleSlotAbilityEnded, TWeakObjectPtr<AGGYGOCharacterSlot>(Slot));
	if (const AGGYGOCharacterBase* Member = Cast<AGGYGOCharacterBase>(Slot->GetAvatarPawn()))
	{
		if (UGGYGOHealthComponent* Health = Member->GetHealthComponent())
		{
			Runtime.ObservedHealth = Health;
			Health->OnDeathFinished.AddUniqueDynamic(this, &ThisClass::HandleMemberDeathFinished);
		}
	}
	if (bAcceptCreatedResources)
	{
		FCreatedSlotResources& Resources = CreatedSlotResources.AddDefaulted_GetRef();
		Resources.Slot = SlotIdentity;
		Resources.OriginalCreatedPawn = OriginalPawnIdentity;
	}

	// 新登记的位置默认待命。第一个位置随后被激活。
	const TWeakObjectPtr<UGGYGOSquadComponent> OriginalSquad(this);
	DeactivateSlot(Slot);

	// 回调可能销毁对象或改变连接/Avatar，只对当前仍一致的首位尝试原自动激活。
	UGGYGOSquadComponent* LiveSquad = OriginalSquad.Get();
	if (!LiveSquad || LiveSquad->bSquadTerminationStarted || LiveSquad->IsBeingDestroyed())
	{
		return true; // 接收已提交；终止不把创建责任反交给调用方。
	}
	APlayerController* CurrentController = LiveSquad->GetRegistrationController();
	if (CurrentController && LiveSquad->ActiveSlotIndex == INDEX_NONE &&
		LiveSquad->HasValidSlotBinding(LiveSquad->GetSlot(0), CurrentController))
	{
		LiveSquad->SwitchToSlot(0);
	}

	// 返回接收提交结果，不因自动激活或 Possess 的结果误交回创建责任。
	return true;
}

namespace
{
	/** Spec 仍在执行时返回其项目能力实例；InstancedPerActor 只有主实例。 */
	UGGYGOGameplayAbility* FindActiveProjectAbility(const UAbilitySystemComponent* ASC, FGameplayAbilitySpecHandle Handle)
	{
		const FGameplayAbilitySpec* Spec = ASC ? ASC->FindAbilitySpecFromHandle(Handle) : nullptr;
		return Spec && Spec->IsActive() ? Cast<UGGYGOGameplayAbility>(Spec->GetPrimaryInstance()) : nullptr;
	}
}

bool UGGYGOSquadComponent::SwitchToSlot(int32 SlotIndex, EGGYGOSwitchSource Source)
{
	check(IsInGameThread());
	const FString SquadPath = GetPathName();
	const int32 OldIndex = ActiveSlotIndex;
	const auto Reject = [&SquadPath, SlotIndex, OldIndex](const TCHAR* Reason, const FString& Detail = FString())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Teams] SwitchToSlot Squad=%s Target=%d Active=%d Reason=%s%s; rejected before any switch effect."),
			*SquadPath, SlotIndex, OldIndex, Reason, *Detail);
		return false;
	};
	if (bSquadTerminationStarted || !IsValid(this) || IsBeingDestroyed()) { return Reject(TEXT("SquadTerminated")); }
	if (bSwitchRequestInProgress) { return Reject(TEXT("SwitchInProgress")); }
	if (!Slots.IsValidIndex(SlotIndex) || !SlotRuntimes.IsValidIndex(SlotIndex)) { return Reject(TEXT("InvalidSlotIndex")); }
	if (SlotIndex == OldIndex) { return Reject(TEXT("AlreadyActive")); }
	APlayerController* PC = GetRegistrationController();
	if (!PC) { return Reject(TEXT("NoAuthoritativeController")); }
	AGGYGOCharacterSlot* NewSlot = Slots[SlotIndex];
	AGGYGOCharacterSlot* OldSlot = GetActiveSlot();
	if (!CanSlotBeActive(NewSlot)) { return Reject(TEXT("TargetWithoutAvatarOrDead")); }
	if (!HasValidSlotBinding(NewSlot, PC) || (OldIndex != INDEX_NONE && !HasValidSlotBinding(OldSlot, PC)))
	{
		return Reject(TEXT("SlotBindingInvalid"));
	}
	AGGYGOCharacterBase* NewCharacter = Cast<AGGYGOCharacterBase>(NewSlot->GetAvatarPawn());
	AGGYGOCharacterBase* OldCharacter = OldSlot ? Cast<AGGYGOCharacterBase>(OldSlot->GetAvatarPawn()) : nullptr;
	if (NewCharacter->GetController() || NewCharacter == OldCharacter
		|| (OldCharacter ? PC->GetPawn() != OldCharacter || OldCharacter->GetController() != PC : PC->GetPawn() != nullptr))
	{
		return Reject(TEXT("OriginalControlMismatch"));
	}
	if (Source == EGGYGOSwitchSource::PlayerRequest)
	{
		// 玩家操作的正常结果，不是配置或状态错误：Verbose 记录，不报 Warning。
		const UGGYGOHealthComponent* OldHealth = OldCharacter ? OldCharacter->GetHealthComponent() : nullptr;
		const TCHAR* BlockedReason = OldHealth && OldHealth->IsDeadOrDying() ? TEXT("ActiveMemberDying")
			: GetSwitchCooldownRemaining() > 0.0f ? TEXT("SwitchCooldown") : nullptr;
		if (BlockedReason)
		{
			UE_LOG(LogGGYGOAbilitySystem, Verbose,
				TEXT("[Teams] SwitchToSlot Squad=%s Target=%d Active=%d Reason=%s; player request not executed."),
				*SquadPath, SlotIndex, OldIndex, BlockedReason);
			return false;
		}
	}
	UGGYGOAbilitySystemComponent* NewASC = NewSlot->GetGGYGOAbilitySystemComponent();
	UGGYGOAbilitySystemComponent* OldASC = OldSlot ? OldSlot->GetGGYGOAbilitySystemComponent() : nullptr;

	// 退场中切回要取消退场能力；它声明不可取消时不切。
	for (const FGameplayAbilitySpecHandle Handle : SlotRuntimes[SlotIndex].ExitAbilities)
	{
		const FGameplayAbilitySpec* Spec = NewASC->FindAbilitySpecFromHandle(Handle);
		const UGameplayAbility* Exit = Spec && Spec->IsActive() ? Spec->GetPrimaryInstance() : nullptr;
		if (Exit && !Exit->CanBeCanceled())
		{
			return Reject(TEXT("TargetExitNotCancelable"), FString::Printf(TEXT(" Spec=%s"), *Handle.ToString()));
		}
	}

	bSwitchRequestInProgress = true;
	const TWeakObjectPtr<UGGYGOSquadComponent> WeakSquad(this);
	ON_SCOPE_EXIT
	{
		if (UGGYGOSquadComponent* Squad = WeakSquad.Get()) { Squad->bSwitchRequestInProgress = false; }
	};
	// 能力蓝图、取消、输入释放与附身都会同步外调。之后只核对原布局：组件未终止、Controller 未换、
	// 两个槽位仍在原序号且 Avatar 未换；能力与控制的实际状态以 GAS 和原生读回为准。
	const TWeakObjectPtr<APlayerController> WeakPC(PC);
	const TWeakObjectPtr<AGGYGOCharacterSlot> WeakNewSlot(NewSlot);
	const TWeakObjectPtr<AGGYGOCharacterSlot> WeakOldSlot(OldSlot);
	const TWeakObjectPtr<AGGYGOCharacterBase> WeakNewCharacter(NewCharacter);
	const TWeakObjectPtr<AGGYGOCharacterBase> WeakOldCharacter(OldCharacter);
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> WeakNewASC(NewASC);
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> WeakOldASC(OldASC);
	const auto IsLayoutCurrent = [&]()
	{
		const UGGYGOSquadComponent* Squad = WeakSquad.Get();
		return Squad && WeakPC.IsValid() && Squad->GetRegistrationController() == WeakPC.Get()
			&& Squad->IsSlotCurrent(SlotIndex, WeakNewSlot.Get()) && WeakNewCharacter.IsValid() && WeakNewASC.IsValid()
			&& WeakNewSlot->GetAvatarPawn() == WeakNewCharacter.Get()
			&& (OldIndex == INDEX_NONE || (Squad->IsSlotCurrent(OldIndex, WeakOldSlot.Get())
				&& WeakOldCharacter.IsValid() && WeakOldASC.IsValid()
				&& WeakOldSlot->GetAvatarPawn() == WeakOldCharacter.Get()));
	};
	const auto Stop = [&SquadPath, SlotIndex, OldIndex](const TCHAR* Reason)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] SwitchToSlot Squad=%s Target=%d Old=%d Reason=%s; switch stopped, effects already applied are not restored."),
			*SquadPath, SlotIndex, OldIndex, Reason);
		return false;
	};

	// 1. 先向旧队员全部活动能力取得换出决定，再检查 Cancel 决定能否执行；此前没有任何切换效果。
	struct FSwitchOutDecision
	{
		TWeakObjectPtr<UGGYGOGameplayAbility> Ability;
		FGameplayAbilitySpecHandle Handle;
		FGGYGOAbilityActivationHandle Original;
		EGGYGOSwitchOutDecision Decision = EGGYGOSwitchOutDecision::Cancel;
	};
	TArray<FSwitchOutDecision> Decisions;
	if (OldASC)
	{
		TArray<FSwitchOutDecision> Candidates;
		for (const FGameplayAbilitySpec& Spec : OldASC->GetActivatableAbilities())
		{
			if (!Spec.IsActive()) { continue; }
			UGGYGOGameplayAbility* Ability = Cast<UGGYGOGameplayAbility>(Spec.GetPrimaryInstance());
			if (!Ability)
			{
				return Reject(TEXT("UnsupportedActiveAbility"), FString::Printf(TEXT(" Spec=%s"), *Spec.Handle.ToString()));
			}
			FSwitchOutDecision& Candidate = Candidates.AddDefaulted_GetRef();
			Candidate.Ability = Ability;
			Candidate.Handle = Spec.Handle;
			Candidate.Original = Ability->CaptureCurrentActivation();
		}
		FGGYGOSwitchContext Context;
		Context.TargetSlotIndex = SlotIndex;
		Context.Source = Source;
		for (const FSwitchOutDecision& Candidate : Candidates)
		{
			UGGYGOGameplayAbility* Ability = Candidate.Ability.Get();
			if (!Ability || !Ability->IsActivationCurrent(Candidate.Original)) { continue; } // 正在结束。
			const EGGYGOSwitchOutDecision Decision = Ability->OnAvatarSwitchOut(Context);
			if (!IsLayoutCurrent()) { return Reject(TEXT("SwitchOutDecisionChangedLayout")); }
			Decisions.Add_GetRef(Candidate).Decision = Decision;
		}
		for (FSwitchOutDecision& Decided : Decisions)
		{
			const UGGYGOGameplayAbility* Ability = Decided.Ability.Get();
			if (Decided.Decision == EGGYGOSwitchOutDecision::Cancel && Ability
				&& Ability->IsActivationCurrent(Decided.Original) && !Ability->CanBeCanceled())
			{
				// 不可取消的能力不阻止换人：改为留场放完，与 ContinueOnField 同一条留场路径。
				Decided.Decision = EGGYGOSwitchOutDecision::ContinueOnField;
				UE_LOG(LogGGYGOAbilitySystem, Log,
					TEXT("[Teams] SwitchToSlot Squad=%s Target=%d Spec=%s Ability=%s: uncancelable Cancel decision continues on field."),
					*SquadPath, SlotIndex, *Decided.Handle.ToString(), *GetPathNameSafe(Ability));
			}
		}
	}

	if (OldIndex != INDEX_NONE)
	{
		// 2. 留场集合先登记再取消其余；回调里结束的能力由 OnAbilityEnded 从集合移除。
		SetReturnPending(OldIndex, false);
		SlotRuntimes[OldIndex].Continuing.Reset();
		for (const FSwitchOutDecision& Decided : Decisions)
		{
			if (Decided.Decision == EGGYGOSwitchOutDecision::ContinueOnField)
			{
				SlotRuntimes[OldIndex].Continuing.AddUnique(Decided.Handle);
			}
		}
		for (const FSwitchOutDecision& Decided : Decisions)
		{
			UGGYGOGameplayAbility* Ability = Decided.Ability.Get();
			if (Decided.Decision != EGGYGOSwitchOutDecision::Cancel || !Ability) { continue; }
			Ability->RequestAbilityCancel(Decided.Original, true);
			if (!IsLayoutCurrent()) { return Stop(TEXT("CancelChangedLayout")); }
		}

		// 3. 失去控制：补发真实释放，再通知续放能力（Controller 仍可读），然后退役 Hero 输入并解除附身。
		WeakOldASC->ReleaseHeldInputForControlLoss();
		if (!IsLayoutCurrent()) { return Stop(TEXT("InputReleaseChangedLayout")); }
		const TArray<FGameplayAbilitySpecHandle> Continuing = SlotRuntimes[OldIndex].Continuing;
		for (const FGameplayAbilitySpecHandle Handle : Continuing)
		{
			if (UGGYGOGameplayAbility* Ability = FindActiveProjectAbility(WeakOldASC.Get(), Handle))
			{
				Ability->NotifyOwnerControlLost();
				if (!IsLayoutCurrent()) { return Stop(TEXT("ControlLostChangedLayout")); }
			}
		}
		if (UGGYGOHeroComponent* Hero = UGGYGOHeroComponent::FindHeroComponent(WeakOldCharacter.Get()))
		{
			Hero->ReleasePlayerInput();
			if (!IsLayoutCurrent()) { return Stop(TEXT("HeroReleaseChangedLayout")); }
		}
		WeakPC->UnPossess();
		if (!IsLayoutCurrent() || WeakPC->GetPawn() || WeakOldCharacter->GetController())
		{
			return Stop(TEXT("UnPossessReadbackFailed"));
		}
	}

	// 4. 控制与镜头立即交给新队员；原生 Possess 无回执，以双向关系读回为准。
	WeakPC->Possess(WeakNewCharacter.Get());
	if (!IsLayoutCurrent()) { return Stop(TEXT("PossessChangedLayout")); }
	if (WeakPC->GetPawn() != WeakNewCharacter.Get() || WeakNewCharacter->GetController() != WeakPC.Get())
	{
		// 旧队员已失去控制，照常离场；出战位置空出，留给下一次明确的切换。
		ActiveSlotIndex = INDEX_NONE;
		if (OldIndex != INDEX_NONE) { MoveOffField(OldIndex); }
		if (IsLayoutCurrent()) { OnActiveCharacterChanged.Broadcast(nullptr); }
		return Stop(TEXT("PossessReadbackFailed"));
	}
	ActiveSlotIndex = SlotIndex;
	if (OldIndex != INDEX_NONE)
	{
		LastSwitchWorldTime = GetWorld()->GetTimeSeconds();
	}

	// 5. 新队员：留场／退场中切回不播进场，待命队员显示后发 SwitchIn。
	const EGGYGOSquadMemberState PreviousState = SlotRuntimes[SlotIndex].State;
	SlotRuntimes[SlotIndex].State = EGGYGOSquadMemberState::Active;
	if (PreviousState == EGGYGOSquadMemberState::Lingering || PreviousState == EGGYGOSquadMemberState::Exiting)
	{
		// 先移出退场集合，取消回调不会被当成退场完成。
		const TArray<FGameplayAbilitySpecHandle> Exits = MoveTemp(SlotRuntimes[SlotIndex].ExitAbilities);
		SlotRuntimes[SlotIndex].ExitAbilities.Reset();
		for (const FGameplayAbilitySpecHandle Handle : Exits)
		{
			WeakNewASC->CancelAbilityHandle(Handle);
			if (!IsLayoutCurrent()) { return Stop(TEXT("ExitCancelChangedLayout")); }
		}
		SetOffField(SlotIndex, false);
		if (!IsLayoutCurrent()) { return Stop(TEXT("OffFieldRemovalChangedLayout")); }
		if (!SlotRuntimes[SlotIndex].Continuing.IsEmpty())
		{
			SetReturnPending(SlotIndex, true);
			const TArray<FGameplayAbilitySpecHandle> Continuing = SlotRuntimes[SlotIndex].Continuing;
			for (const FGameplayAbilitySpecHandle Handle : Continuing)
			{
				if (UGGYGOGameplayAbility* Ability = FindActiveProjectAbility(WeakNewASC.Get(), Handle))
				{
					Ability->NotifyOwnerControlRegained();
					if (!IsLayoutCurrent()) { return Stop(TEXT("ControlRegainedChangedLayout")); }
				}
			}
		}
	}
	else
	{
		ActivateSlot(WeakNewSlot.Get());
		if (!IsLayoutCurrent()) { return Stop(TEXT("SwitchInPresentationChangedLayout")); }
		// 开局第一名没有替换任何人，不播进场。
		if (OldIndex != INDEX_NONE)
		{
			SendSquadEvent(WeakNewSlot.Get(), GGYGOGameplayTags::Event_Squad_SwitchIn);
			if (!IsLayoutCurrent()) { return Stop(TEXT("SwitchInEventChangedLayout")); }
		}
	}

	// 6. 旧队员离场。
	if (OldIndex != INDEX_NONE)
	{
		MoveOffField(OldIndex);
		if (!IsLayoutCurrent()) { return Stop(TEXT("OldOffFieldChangedLayout")); }
	}
	OnActiveCharacterChanged.Broadcast(WeakNewCharacter.Get());
	return IsLayoutCurrent() && ActiveSlotIndex == SlotIndex;
}

void UGGYGOSquadComponent::ServerRequestSwitchToSlot_Implementation(int32 SlotIndex)
{
	SwitchToSlot(SlotIndex, EGGYGOSwitchSource::PlayerRequest);
}

bool UGGYGOSquadComponent::ServerRequestSwitchToSlot_Validate(int32 SlotIndex)
{
	return SlotIndex >= 0 && SlotIndex < GGYGO_MAX_SQUAD_SIZE;
}

EGGYGOSquadMemberState UGGYGOSquadComponent::GetMemberState(int32 SlotIndex) const
{
	return SlotRuntimes.IsValidIndex(SlotIndex) ? SlotRuntimes[SlotIndex].State : EGGYGOSquadMemberState::Benched;
}

bool UGGYGOSquadComponent::SwitchToNextSlot()
{
	return SwitchToAdjacentSlot(1, EGGYGOSwitchSource::Script);
}

bool UGGYGOSquadComponent::SwitchToPreviousSlot()
{
	return SwitchToAdjacentSlot(-1, EGGYGOSwitchSource::Script);
}

void UGGYGOSquadComponent::ServerRequestSwitchToAdjacentSlot_Implementation(int32 Direction)
{
	SwitchToAdjacentSlot(Direction, EGGYGOSwitchSource::PlayerRequest);
}

bool UGGYGOSquadComponent::ServerRequestSwitchToAdjacentSlot_Validate(int32 Direction)
{
	return Direction == 1 || Direction == -1;
}

float UGGYGOSquadComponent::GetSwitchCooldownRemaining() const
{
	const UWorld* World = GetWorld();
	if (!LastSwitchWorldTime.IsSet() || !World) { return 0.0f; }
	const double Elapsed = World->GetTimeSeconds() - LastSwitchWorldTime.GetValue();
	return static_cast<float>(FMath::Max(0.0, SwitchCooldownSeconds - Elapsed));
}

void UGGYGOSquadComponent::HandleMemberDeathFinished(AActor* OwningActor)
{
	// 客户端经 OnRep 也会广播；换人只在服务器。
	if (!GetOwner() || !GetOwner()->HasAuthority() || bSquadTerminationStarted || !OwningActor) { return; }
	const AGGYGOCharacterSlot* ActiveSlot = GetActiveSlot();
	if (!ActiveSlot || ActiveSlot->GetAvatarPawn() != OwningActor) { return; }
	// 在 FinishDeath 广播内同步换人：角色在下一帧才销毁，此时附身关系与槽位绑定仍完整。
	if (!SwitchToAdjacentSlot(1, EGGYGOSwitchSource::DeathAutoSwitch))
	{
		UE_LOG(LogGGYGOAbilitySystem, Log,
			TEXT("[Teams] Squad=%s Dead=%s: no member could take the field after death (none alive, or switch in progress)."),
			*GetPathName(), *GetPathNameSafe(OwningActor));
	}
}

bool UGGYGOSquadComponent::SwitchToAdjacentSlot(int32 Direction, EGGYGOSwitchSource Source)
{
	check(Direction == 1 || Direction == -1);
	if (bSquadTerminationStarted || bSwitchRequestInProgress || !IsValid(this) || IsBeingDestroyed())
	{
		return false;
	}
	const TWeakObjectPtr<UGGYGOSquadComponent> OriginalSquad(this);
	const int32 Count = Slots.Num();
	const bool bHasActive = ActiveSlotIndex != INDEX_NONE;
	if (Count == 0 || (bHasActive && Count == 1))
	{
		return false;
	}

	// 从相邻位置起逐个试，跳过死亡或无实体的位置；有出战位置时试 Count - 1 个（不含自己），
	// 没有时从队首／队尾起试全部 Count 个。加 Count 再取模，避免负数下标。
	const int32 Origin = bHasActive ? ActiveSlotIndex : (Direction > 0 ? -1 : 0);
	const int32 Attempts = bHasActive ? Count - 1 : Count;
	for (int32 Offset = 1; Offset <= Attempts; ++Offset)
	{
		const int32 Candidate = ((Origin + Direction * Offset) % Count + Count) % Count;
		if (CanSlotBeActive(GetSlot(Candidate)))
		{
			// 真实切换请求失败后不换目标重试：它可能已执行了部分换出。
			return SwitchToSlot(Candidate, Source);
		}
		if (!OriginalSquad.IsValid() || OriginalSquad->bSquadTerminationStarted || OriginalSquad->IsBeingDestroyed())
		{
			return false;
		}
	}

	return false;
}

bool UGGYGOSquadComponent::CanContinueSlotPresentation(
	const TWeakObjectPtr<AGGYGOCharacterSlot>& OriginalSlot,
	const TWeakObjectPtr<AGGYGOCharacterBase>& OriginalCharacter) const
{
	return IsValid(this) && !bSquadTerminationStarted && !IsBeingDestroyed() &&
		OriginalSlot.IsValid() && !OriginalSlot->IsActorBeingDestroyed() &&
		OriginalCharacter.IsValid() && !OriginalCharacter->IsActorBeingDestroyed() &&
		OriginalSlot->GetAvatarPawn() == OriginalCharacter.Get();
}

void UGGYGOSquadComponent::ActivateSlot(AGGYGOCharacterSlot* Slot)
{
	const TWeakObjectPtr<UGGYGOSquadComponent> OriginalSquad(this);
	const TWeakObjectPtr<AGGYGOCharacterSlot> OriginalSlot(Slot);
	const TWeakObjectPtr<AGGYGOCharacterBase> OriginalCharacter(
		OriginalSlot.IsValid() ? Cast<AGGYGOCharacterBase>(OriginalSlot->GetAvatarPawn()) : nullptr);
	const auto CanContinuePresentation = [&]()
	{
		UGGYGOSquadComponent* Squad = OriginalSquad.Get();
		return Squad && Squad->CanContinueSlotPresentation(OriginalSlot, OriginalCharacter);
	};
	if (!CanContinuePresentation())
	{
		return;
	}

	OriginalCharacter->SetActorHiddenInGame(false);
	if (!CanContinuePresentation())
	{
		return;
	}
	OriginalCharacter->SetReplicatedActorEnableCollision(true);
	if (!CanContinuePresentation())
	{
		return;
	}
	if (USkeletalMeshComponent* Mesh = OriginalCharacter->GetMesh())
	{
		Mesh->bNoSkeletonUpdate = false;
	}

	if (UCharacterMovementComponent* MoveComp = OriginalCharacter->GetCharacterMovement())
	{
		// 恢复到行走模式。待命期间移动模式被设为 None，不恢复的话角色不会动。
		MoveComp->SetMovementMode(MOVE_Walking);
	}
}

void UGGYGOSquadComponent::DeactivateSlot(AGGYGOCharacterSlot* Slot)
{
	const TWeakObjectPtr<UGGYGOSquadComponent> OriginalSquad(this);
	const TWeakObjectPtr<AGGYGOCharacterSlot> OriginalSlot(Slot);
	const TWeakObjectPtr<AGGYGOCharacterBase> OriginalCharacter(
		OriginalSlot.IsValid() ? Cast<AGGYGOCharacterBase>(OriginalSlot->GetAvatarPawn()) : nullptr);
	const auto CanContinuePresentation = [&]()
	{
		UGGYGOSquadComponent* Squad = OriginalSquad.Get();
		return Squad && Squad->CanContinueSlotPresentation(OriginalSlot, OriginalCharacter);
	};
	if (!CanContinuePresentation())
	{
		return;
	}

	OriginalCharacter->SetActorHiddenInGame(true);
	if (!CanContinuePresentation())
	{
		return;
	}
	// 引擎的整体碰撞不复制；客户端若保留待命胶囊，本地预测移动会撞上看不见的队员。
	OriginalCharacter->SetReplicatedActorEnableCollision(false);
	if (!CanContinuePresentation())
	{
		return;
	}
	// 待命 Pawn 不更新动画。服务器上从未附身的队员没有 Controller，PawnExtension 停在 Spawned、
	// CMC 没有 MovementSet；ACharacter 的 Mesh 隐藏时仍 AlwaysTickPoseAndRefreshBones，
	// 动画会读到未初始化的移动配置。用 bNoSkeletonUpdate 而不关组件 Tick：登记可能早于
	// BeginPlay，那时关掉的 Tick 会被注册按 bStartWithTickEnabled 重新打开。
	// 出战时先 Possess 完成初始化，再由 ActivateSlot 恢复。
	if (USkeletalMeshComponent* Mesh = OriginalCharacter->GetMesh())
	{
		Mesh->bNoSkeletonUpdate = true;
	}

	if (UCharacterMovementComponent* MoveComp = OriginalCharacter->GetCharacterMovement())
	{
		const TWeakObjectPtr<UCharacterMovementComponent> OriginalMovement(MoveComp);
		// 停住并禁用移动。只隐藏不停移动会让待命角色带着上一刻的速度
		// 继续滑行，切回来时位置已经偏离。
		MoveComp->StopMovementImmediately();
		if (!CanContinuePresentation() || !OriginalMovement.IsValid() || OriginalMovement->IsBeingDestroyed())
		{
			return;
		}
		OriginalMovement->DisableMovement();
	}

	// 待命位置的 ASC **不做任何处理**：冷却要继续走、Buff 要继续计时、
	// 血量要保留。这些状态都在位置上而不在 Pawn 上，所以即便将来改成
	// 待命时销毁 Pawn，它们同样不受影响。
}

void UGGYGOSquadComponent::OnRep_ActiveSlotIndex()
{
	if (bSquadTerminationStarted || !IsValid(this) || IsBeingDestroyed())
	{
		return;
	}
	// 客户端只更新表现。隐藏由 Actor 的 bHidden、碰撞由角色的复制碰撞开关同步；
	// 隐藏且无碰撞、又不属于该客户端的待命角色按引擎相关性规则不复制给它。
	// 这里只需要广播事件让 UI 与相机跟上。
	OnActiveCharacterChanged.Broadcast(GetActiveCharacter());
}

bool UGGYGOSquadComponent::IsSlotCurrent(int32 Index, const AGGYGOCharacterSlot* Slot) const
{
	return IsValid(this) && !bSquadTerminationStarted && Slot && Slots.IsValidIndex(Index)
		&& SlotRuntimes.IsValidIndex(Index) && Slots[Index] == Slot;
}

void UGGYGOSquadComponent::HandleSlotAbilityEnded(const FAbilityEndedData& Data, TWeakObjectPtr<AGGYGOCharacterSlot> Slot)
{
	const int32 Index = Slot.IsValid() ? Slots.IndexOfByKey(Slot.Get()) : INDEX_NONE;
	if (!IsSlotCurrent(Index, Slot.Get())) { return; }
	FSlotRuntime& Runtime = SlotRuntimes[Index];
	const bool bContinuingEnded = Runtime.Continuing.Remove(Data.AbilitySpecHandle) > 0;
	const bool bExitEnded = Runtime.ExitAbilities.Remove(Data.AbilitySpecHandle) > 0;
	if (bContinuingEnded && Runtime.Continuing.IsEmpty())
	{
		// Off_Field 保持到退场结束；切回后的 ReturnPending 在续放全部结束时解除。
		if (Runtime.State == EGGYGOSquadMemberState::Lingering) { BeginExit(Index); }
		else if (Runtime.State == EGGYGOSquadMemberState::Active && Runtime.bReturnPending) { SetReturnPending(Index, false); }
	}
	else if (bExitEnded && Runtime.ExitAbilities.IsEmpty() && Runtime.State == EGGYGOSquadMemberState::Exiting)
	{
		FinishExit(Index);
	}
}

void UGGYGOSquadComponent::MoveOffField(int32 Index)
{
	AGGYGOCharacterSlot* Slot = Slots[Index];
	SetOffField(Index, true);
	if (!IsSlotCurrent(Index, Slot)) { return; }
	if (SlotRuntimes[Index].Continuing.IsEmpty())
	{
		BeginExit(Index);
	}
	else
	{
		SlotRuntimes[Index].State = EGGYGOSquadMemberState::Lingering;
	}
}

void UGGYGOSquadComponent::BeginExit(int32 Index)
{
	AGGYGOCharacterSlot* Slot = Slots[Index];
	SlotRuntimes[Index].State = EGGYGOSquadMemberState::Exiting;
	SlotRuntimes[Index].ExitAbilities.Reset();
	const TArray<FGameplayAbilitySpecHandle> Activated = SendSquadEvent(Slot, GGYGOGameplayTags::Event_Squad_SwitchOut);
	// 事件回调可能切回该队员或终止编队；只在仍处于这次退场时登记等待。
	if (!IsSlotCurrent(Index, Slot) || SlotRuntimes[Index].State != EGGYGOSquadMemberState::Exiting) { return; }
	const UGGYGOAbilitySystemComponent* ASC = Slot->GetGGYGOAbilitySystemComponent();
	for (const FGameplayAbilitySpecHandle Handle : Activated)
	{
		const FGameplayAbilitySpec* Spec = ASC ? ASC->FindAbilitySpecFromHandle(Handle) : nullptr;
		if (Spec && Spec->IsActive()) { SlotRuntimes[Index].ExitAbilities.AddUnique(Handle); }
	}
	if (SlotRuntimes[Index].ExitAbilities.IsEmpty())
	{
		FinishExit(Index);
	}
}

void UGGYGOSquadComponent::FinishExit(int32 Index)
{
	AGGYGOCharacterSlot* Slot = Slots[Index];
	SlotRuntimes[Index].State = EGGYGOSquadMemberState::Benched;
	SlotRuntimes[Index].ExitAbilities.Reset();
	SetOffField(Index, false);
	if (!IsSlotCurrent(Index, Slot)) { return; }
	DeactivateSlot(Slot);
}

void UGGYGOSquadComponent::SetOffField(int32 Index, bool bOffField)
{
	FSlotRuntime& Runtime = SlotRuntimes[Index];
	if (Runtime.bOffField == bOffField) { return; }
	Runtime.bOffField = bOffField;
	AGGYGOCharacterSlot* Slot = Slots[Index];
	if (bOffField)
	{
		// 留场与退场期间没有 Controller；原生 CMC 只在此开关打开时继续执行技能位移与重力。
		const ACharacter* Character = Cast<ACharacter>(Slot->GetAvatarPawn());
		if (UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr)
		{
			Runtime.OffFieldMovement = Movement;
			Runtime.bSavedRunPhysicsWithNoController = Movement->bRunPhysicsWithNoController;
			Movement->bRunPhysicsWithNoController = true;
		}
	}
	else
	{
		if (UCharacterMovementComponent* Movement = Runtime.OffFieldMovement.Get())
		{
			Movement->bRunPhysicsWithNoController = Runtime.bSavedRunPhysicsWithNoController;
		}
		Runtime.OffFieldMovement.Reset();
	}
	// Tag 事件可以同步回调，放在最后且不再使用 Runtime 引用。
	if (UGGYGOAbilitySystemComponent* ASC = Slot->GetGGYGOAbilitySystemComponent())
	{
		// OffField 复制给客户端；伤害只在服务器结算，免疫 Tag 不复制。
		if (bOffField)
		{
			ASC->AddLooseGameplayTag(GGYGOGameplayTags::State_OffField, 1, EGameplayTagReplicationState::TagOnly);
			ASC->AddLooseGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity);
		}
		else
		{
			ASC->RemoveLooseGameplayTag(GGYGOGameplayTags::State_OffField, 1, EGameplayTagReplicationState::TagOnly);
			ASC->RemoveLooseGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity);
		}
	}
}

void UGGYGOSquadComponent::SetReturnPending(int32 Index, bool bPending)
{
	FSlotRuntime& Runtime = SlotRuntimes[Index];
	if (Runtime.bReturnPending == bPending) { return; }
	Runtime.bReturnPending = bPending;
	// 复制给拥有者客户端，使其本地预测同样拦截激活与移动；输入缓冲在标签移除后自行唤醒。
	if (UGGYGOAbilitySystemComponent* ASC = Slots[Index]->GetGGYGOAbilitySystemComponent())
	{
		if (bPending) { ASC->AddLooseGameplayTag(GGYGOGameplayTags::State_SwitchReturnPending, 1, EGameplayTagReplicationState::TagOnly); }
		else { ASC->RemoveLooseGameplayTag(GGYGOGameplayTags::State_SwitchReturnPending, 1, EGameplayTagReplicationState::TagOnly); }
	}
}

TArray<FGameplayAbilitySpecHandle> UGGYGOSquadComponent::SendSquadEvent(AGGYGOCharacterSlot* Slot, FGameplayTag EventTag)
{
	TArray<FGameplayAbilitySpecHandle> Activated;
	UGGYGOAbilitySystemComponent* ASC = Slot ? Slot->GetGGYGOAbilitySystemComponent() : nullptr;
	APawn* Avatar = Slot ? Slot->GetAvatarPawn() : nullptr;
	if (!ASC || !Avatar) { return Activated; }
	// 只记录这次事件同步激活的能力；激活后同步结束的由调用方按 IsActive 过滤。
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> WeakASC(ASC);
	const FDelegateHandle Observer = ASC->AbilityActivatedCallbacks.AddLambda([&Activated](UGameplayAbility* Ability)
	{
		if (Ability) { Activated.AddUnique(Ability->GetCurrentAbilitySpecHandle()); }
	});
	FGameplayEventData Payload;
	Payload.EventTag = EventTag;
	Payload.Instigator = Avatar;
	Payload.Target = Avatar;
	{
		FScopedPredictionWindow PredictionWindow(ASC, true);
		ASC->HandleGameplayEvent(EventTag, &Payload);
	}
	if (UGGYGOAbilitySystemComponent* LiveASC = WeakASC.Get())
	{
		LiveASC->AbilityActivatedCallbacks.Remove(Observer);
	}
	return Activated;
}

void UGGYGOSquadComponent::ReleaseSlotRuntimes()
{
	// 只归还本组件施加的订阅、Tag 与移动开关；槽位的 GE、冷却与能力状态不动。
	TArray<FSlotRuntime> Runtimes = MoveTemp(SlotRuntimes);
	SlotRuntimes.Reset();
	for (FSlotRuntime& Runtime : Runtimes)
	{
		if (UCharacterMovementComponent* Movement = Runtime.OffFieldMovement.Get())
		{
			Movement->bRunPhysicsWithNoController = Runtime.bSavedRunPhysicsWithNoController;
		}
		if (UGGYGOHealthComponent* Health = Runtime.ObservedHealth.Get())
		{
			Health->OnDeathFinished.RemoveDynamic(this, &ThisClass::HandleMemberDeathFinished);
		}
		UGGYGOAbilitySystemComponent* ASC = Runtime.ObservedASC.Get();
		if (!ASC) { continue; }
		ASC->OnAbilityEnded.Remove(Runtime.AbilityEndedHandle);
		if (Runtime.bOffField)
		{
			ASC->RemoveLooseGameplayTag(GGYGOGameplayTags::State_OffField, 1, EGameplayTagReplicationState::TagOnly);
			ASC->RemoveLooseGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity);
		}
		if (Runtime.bReturnPending)
		{
			ASC->RemoveLooseGameplayTag(GGYGOGameplayTags::State_SwitchReturnPending, 1, EGameplayTagReplicationState::TagOnly);
		}
	}
}
