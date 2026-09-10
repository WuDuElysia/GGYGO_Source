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

	DOREPLIFETIME(UGGYGOSquadComponent, Members);
	DOREPLIFETIME(UGGYGOSquadComponent, ActiveMemberIndex);
}

AGGYGOCharacterBase* UGGYGOSquadComponent::GetActiveCharacter() const
{
	return Members.IsValidIndex(ActiveMemberIndex) ? Members[ActiveMemberIndex] : nullptr;
}

bool UGGYGOSquadComponent::CanMemberBeActive(const AGGYGOCharacterBase* Member) const
{
	if (!Member)
	{
		return false;
	}

	// 已死亡的成员不能出战。切人到尸体上会让玩家失去控制且无法切回。
	if (const UGGYGOHealthComponent* HealthComponent = Member->GetHealthComponent())
	{
		if (HealthComponent->IsDeadOrDying())
		{
			return false;
		}
	}

	return true;
}

void UGGYGOSquadComponent::RegisterMember(AGGYGOCharacterBase* Member)
{
	if (!Member)
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner || Owner->GetLocalRole() != ROLE_Authority)
	{
		// 队伍名单是服务器权威的，客户端靠复制拿到。
		return;
	}

	if (Members.Contains(Member))
	{
		return;
	}

	Members.Add(Member);

	// 新登记的成员默认待命。第一个成员随后被激活。
	DeactivateMember(Member);

	if (ActiveMemberIndex == INDEX_NONE)
	{
		SwitchToMember(0);
	}
}

bool UGGYGOSquadComponent::SwitchToMember(int32 MemberIndex)
{
	AActor* Owner = GetOwner();
	if (!Owner || Owner->GetLocalRole() != ROLE_Authority)
	{
		return false;
	}

	if (!Members.IsValidIndex(MemberIndex) || MemberIndex == ActiveMemberIndex)
	{
		return false;
	}

	AGGYGOCharacterBase* NewActive = Members[MemberIndex];
	if (!CanMemberBeActive(NewActive))
	{
		return false;
	}

	APlayerState* OwningPlayerState = Cast<APlayerState>(Owner);
	APlayerController* PC = OwningPlayerState ? Cast<APlayerController>(OwningPlayerState->GetOwner()) : nullptr;
	if (!PC)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SwitchToMember: [%s] 找不到 PlayerController，无法转移控制权。"), *GetNameSafe(Owner));
		return false;
	}

	AGGYGOCharacterBase* OldActive = GetActiveCharacter();

	// 顺序很重要：先解除旧的再附身新的。
	//
	// 反过来做会让 UnPossess 在新 Pawn 已被附身之后执行，
	// 而 UnPossess 会清掉 Controller 的 Pawn 引用 —— 刚建立的附身关系被清掉。
	if (OldActive)
	{
		PC->UnPossess();
		DeactivateMember(OldActive);
	}

	ActiveMemberIndex = MemberIndex;

	ActivateMember(NewActive);
	PC->Possess(NewActive);

	OnActiveCharacterChanged.Broadcast(NewActive);

	return true;
}

bool UGGYGOSquadComponent::SwitchToNextMember()
{
	const int32 Count = Members.Num();
	if (Count <= 1)
	{
		return false;
	}

	// 从下一个开始逐个试，跳过死亡成员。
	// 最多试 Count - 1 次：试满一圈还没成功说明只有自己活着。
	for (int32 Offset = 1; Offset < Count; ++Offset)
	{
		const int32 Candidate = (ActiveMemberIndex + Offset) % Count;
		if (SwitchToMember(Candidate))
		{
			return true;
		}
	}

	return false;
}

bool UGGYGOSquadComponent::SwitchToPreviousMember()
{
	const int32 Count = Members.Num();
	if (Count <= 1)
	{
		return false;
	}

	for (int32 Offset = 1; Offset < Count; ++Offset)
	{
		// 加 Count 再取模，避免负数下标。
		const int32 Candidate = ((ActiveMemberIndex - Offset) % Count + Count) % Count;
		if (SwitchToMember(Candidate))
		{
			return true;
		}
	}

	return false;
}

void UGGYGOSquadComponent::ActivateMember(AGGYGOCharacterBase* Member)
{
	if (!Member)
	{
		return;
	}

	Member->SetActorHiddenInGame(false);
	Member->SetActorEnableCollision(true);

	if (UCharacterMovementComponent* MoveComp = Member->GetCharacterMovement())
	{
		// 恢复到行走模式。待命期间移动模式被设为 None，不恢复的话角色不会动。
		MoveComp->SetMovementMode(MOVE_Walking);
	}
}

void UGGYGOSquadComponent::DeactivateMember(AGGYGOCharacterBase* Member)
{
	if (!Member)
	{
		return;
	}

	Member->SetActorHiddenInGame(true);
	Member->SetActorEnableCollision(false);

	if (UCharacterMovementComponent* MoveComp = Member->GetCharacterMovement())
	{
		// 停住并禁用移动。只隐藏不停移动会让待命角色带着上一刻的速度
		// 继续滑行，切回来时位置已经偏离。
		MoveComp->StopMovementImmediately();
		MoveComp->DisableMovement();
	}

	// 待命角色的 ASC **不做任何处理**：冷却要继续走、Buff 要继续计时、
	// 血量要保留。这是队伍制玩法的核心，也是选择换 Avatar 而不是
	// 销毁重建的理由。
}

void UGGYGOSquadComponent::OnRep_ActiveMemberIndex()
{
	// 客户端只更新表现。隐藏与碰撞由 Actor 自身的复制属性同步，
	// 这里只需要广播事件让 UI 与相机跟上。
	OnActiveCharacterChanged.Broadcast(GetActiveCharacter());
}
