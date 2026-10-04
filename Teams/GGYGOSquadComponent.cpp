/**
 * @file GGYGOSquadComponent.cpp
 * @brief 队伍与出战切换实现
 */
#include "Teams/GGYGOSquadComponent.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Character/GGYGOCharacterBase.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
#include "Player/GGYGOPlayerState.h"
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
	if (!IsValid(ASC) || ASC->IsBeingDestroyed() || ASC->GetOwnerActor() != Slot ||
		!IsValid(Avatar) || Avatar->IsActorBeingDestroyed() || !Avatar->HasAuthority() ||
		Avatar->GetWorld() != GetWorld() || Avatar->GetOwner() != OwningController ||
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

bool UGGYGOSquadComponent::SwitchToSlot(int32 SlotIndex)
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

	if (!Slots.IsValidIndex(SlotIndex) || SlotIndex == ActiveSlotIndex)
	{
		return false;
	}

	AGGYGOCharacterSlot* NewSlot = Slots[SlotIndex];
	if (!IsValid(NewSlot) || NewSlot->IsActorBeingDestroyed() || !CanSlotBeActive(NewSlot))
	{
		return false;
	}

	APlayerState* OwningPlayerState = Cast<APlayerState>(Owner);
	APlayerController* PC = OwningPlayerState ? Cast<APlayerController>(OwningPlayerState->GetOwner()) : nullptr;
	if (!PC)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SwitchToSlot: [%s] 找不到 PlayerController，无法转移控制权。"), *GetNameSafe(Owner));
		return false;
	}

	AGGYGOCharacterSlot* OldSlot = GetActiveSlot();
	const TWeakObjectPtr<UGGYGOSquadComponent> OriginalSquad(this);
	const TWeakObjectPtr<APlayerController> OriginalController(PC);
	const TWeakObjectPtr<AGGYGOCharacterSlot> OriginalNewSlot(NewSlot);
	const TWeakObjectPtr<AGGYGOCharacterSlot> OriginalOldSlot(OldSlot);
	const auto CanContinueSwitch = [&]()
	{
		UGGYGOSquadComponent* Squad = OriginalSquad.Get();
		return Squad && !Squad->bSquadTerminationStarted && !Squad->IsBeingDestroyed() &&
			OriginalController.IsValid() && !OriginalController->IsActorBeingDestroyed() &&
			OriginalNewSlot.IsValid() && !OriginalNewSlot->IsActorBeingDestroyed();
	};

	// 顺序很重要：先解除旧的再附身新的。
	//
	// 反过来做会让 UnPossess 在新 Pawn 已被附身之后执行，
	// 而 UnPossess 会清掉 Controller 的 Pawn 引用 —— 刚建立的附身关系被清掉。
	if (OldSlot)
	{
		PC->UnPossess();
		if (!CanContinueSwitch() || !OriginalOldSlot.IsValid())
		{
			return false;
		}
		OriginalSquad->DeactivateSlot(OriginalOldSlot.Get());
		if (!CanContinueSwitch())
		{
			return false;
		}
	}

	OriginalSquad->ActiveSlotIndex = SlotIndex;

	OriginalSquad->ActivateSlot(OriginalNewSlot.Get());
	if (!CanContinueSwitch())
	{
		return false;
	}

	const TWeakObjectPtr<AGGYGOCharacterBase> OriginalNewCharacter(
		Cast<AGGYGOCharacterBase>(OriginalNewSlot->GetAvatarPawn()));
	if (!OriginalNewCharacter.IsValid() || OriginalNewCharacter->IsActorBeingDestroyed())
	{
		return false;
	}
	OriginalController->Possess(OriginalNewCharacter.Get());
	if (!CanContinueSwitch() || !OriginalNewCharacter.IsValid())
	{
		return false;
	}

	OriginalSquad->OnActiveCharacterChanged.Broadcast(OriginalNewCharacter.Get());

	return CanContinueSwitch();
}

bool UGGYGOSquadComponent::SwitchToNextSlot()
{
	if (bSquadTerminationStarted || !IsValid(this) || IsBeingDestroyed())
	{
		return false;
	}
	const TWeakObjectPtr<UGGYGOSquadComponent> OriginalSquad(this);
	const int32 Count = Slots.Num();
	if (Count <= 1)
	{
		return false;
	}

	// 从下一个开始逐个试，跳过死亡或无实体的位置。
	// 最多试 Count - 1 次：试满一圈还没成功说明只有自己可出战。
	for (int32 Offset = 1; Offset < Count; ++Offset)
	{
		const int32 Candidate = (ActiveSlotIndex + Offset) % Count;
		if (SwitchToSlot(Candidate))
		{
			return true;
		}
		if (!OriginalSquad.IsValid() || OriginalSquad->bSquadTerminationStarted || OriginalSquad->IsBeingDestroyed())
		{
			return false;
		}
	}

	return false;
}

bool UGGYGOSquadComponent::SwitchToPreviousSlot()
{
	if (bSquadTerminationStarted || !IsValid(this) || IsBeingDestroyed())
	{
		return false;
	}
	const TWeakObjectPtr<UGGYGOSquadComponent> OriginalSquad(this);
	const int32 Count = Slots.Num();
	if (Count <= 1)
	{
		return false;
	}

	for (int32 Offset = 1; Offset < Count; ++Offset)
	{
		// 加 Count 再取模，避免负数下标。
		const int32 Candidate = ((ActiveSlotIndex - Offset) % Count + Count) % Count;
		if (SwitchToSlot(Candidate))
		{
			return true;
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
	OriginalCharacter->SetActorEnableCollision(true);
	if (!CanContinuePresentation())
	{
		return;
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
	OriginalCharacter->SetActorEnableCollision(false);
	if (!CanContinuePresentation())
	{
		return;
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
	// 客户端只更新表现。隐藏与碰撞由 Actor 自身的复制属性同步，
	// 这里只需要广播事件让 UI 与相机跟上。
	OnActiveCharacterChanged.Broadcast(GetActiveCharacter());
}
