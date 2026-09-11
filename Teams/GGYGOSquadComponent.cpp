/**
 * @file GGYGOSquadComponent.cpp
 * @brief 队伍与出战切换实现
 */
#include "Teams/GGYGOSquadComponent.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "Character/GGYGOCharacterBase.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
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

void UGGYGOSquadComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UGGYGOSquadComponent, Slots);
	DOREPLIFETIME(UGGYGOSquadComponent, ActiveSlotIndex);
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

void UGGYGOSquadComponent::RegisterSlot(AGGYGOCharacterSlot* Slot)
{
	if (!Slot)
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner || Owner->GetLocalRole() != ROLE_Authority)
	{
		// 队伍名单是服务器权威的，客户端靠复制拿到。
		return;
	}

	if (Slots.Contains(Slot))
	{
		return;
	}

	if (Slots.Num() >= GGYGO_MAX_SQUAD_SIZE)
	{
		// 在装配阶段拒绝而不是接受后再承担复制开销：每个位置带一个 ASC，
		// 配置越界的代价是成倍的网络流量，而那要到压测时才会被发现。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("RegisterSlot: 队伍已满（上限 %d），拒绝登记 [%s]。请检查玩法配置的队伍规模。"),
			GGYGO_MAX_SQUAD_SIZE, *GetNameSafe(Slot));
		return;
	}

	Slots.Add(Slot);

	// 新登记的位置默认待命。第一个位置随后被激活。
	DeactivateSlot(Slot);

	if (ActiveSlotIndex == INDEX_NONE)
	{
		SwitchToSlot(0);
	}
}

bool UGGYGOSquadComponent::SwitchToSlot(int32 SlotIndex)
{
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
	if (!CanSlotBeActive(NewSlot))
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

	// 顺序很重要：先解除旧的再附身新的。
	//
	// 反过来做会让 UnPossess 在新 Pawn 已被附身之后执行，
	// 而 UnPossess 会清掉 Controller 的 Pawn 引用 —— 刚建立的附身关系被清掉。
	if (OldSlot)
	{
		PC->UnPossess();
		DeactivateSlot(OldSlot);
	}

	ActiveSlotIndex = SlotIndex;

	ActivateSlot(NewSlot);

	AGGYGOCharacterBase* NewCharacter = Cast<AGGYGOCharacterBase>(NewSlot->GetAvatarPawn());
	PC->Possess(NewCharacter);

	OnActiveCharacterChanged.Broadcast(NewCharacter);

	return true;
}

bool UGGYGOSquadComponent::SwitchToNextSlot()
{
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
	}

	return false;
}

bool UGGYGOSquadComponent::SwitchToPreviousSlot()
{
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
	}

	return false;
}

void UGGYGOSquadComponent::ActivateSlot(AGGYGOCharacterSlot* Slot)
{
	AGGYGOCharacterBase* Character = Slot ? Cast<AGGYGOCharacterBase>(Slot->GetAvatarPawn()) : nullptr;
	if (!Character)
	{
		return;
	}

	Character->SetActorHiddenInGame(false);
	Character->SetActorEnableCollision(true);

	if (UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement())
	{
		// 恢复到行走模式。待命期间移动模式被设为 None，不恢复的话角色不会动。
		MoveComp->SetMovementMode(MOVE_Walking);
	}
}

void UGGYGOSquadComponent::DeactivateSlot(AGGYGOCharacterSlot* Slot)
{
	AGGYGOCharacterBase* Character = Slot ? Cast<AGGYGOCharacterBase>(Slot->GetAvatarPawn()) : nullptr;
	if (!Character)
	{
		return;
	}

	Character->SetActorHiddenInGame(true);
	Character->SetActorEnableCollision(false);

	if (UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement())
	{
		// 停住并禁用移动。只隐藏不停移动会让待命角色带着上一刻的速度
		// 继续滑行，切回来时位置已经偏离。
		MoveComp->StopMovementImmediately();
		MoveComp->DisableMovement();
	}

	// 待命位置的 ASC **不做任何处理**：冷却要继续走、Buff 要继续计时、
	// 血量要保留。这些状态都在位置上而不在 Pawn 上，所以即便将来改成
	// 待命时销毁 Pawn，它们同样不受影响。
}

void UGGYGOSquadComponent::OnRep_ActiveSlotIndex()
{
	// 客户端只更新表现。隐藏与碰撞由 Actor 自身的复制属性同步，
	// 这里只需要广播事件让 UI 与相机跟上。
	OnActiveCharacterChanged.Broadcast(GetActiveCharacter());
}
