/**
 * @file GGYGOCombatantState.cpp
 * @brief 可替换 Pawn 的持久战斗状态宿主实现
 */
#include "Combatants/GGYGOCombatantState.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Attributes/GGYGOCombatSet.h"
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCombatantState)

AGGYGOCombatantState::AGGYGOCombatantState(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 纯状态宿主没有空间行为，也不需要 Actor Tick。GE 时间由 ASC 自己推进。
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	bReplicates = true;
	SetReplicatingMovement(false);

	AbilitySystemComponent = CreateDefaultSubobject<UGGYGOAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);

	// 默认子对象会在 ASC InitializeComponent 阶段被自动发现，早于 Avatar 存在。
	HealthSet = CreateDefaultSubobject<UGGYGOHealthSet>(TEXT("HealthSet"));
	CombatSet = CreateDefaultSubobject<UGGYGOCombatSet>(TEXT("CombatSet"));
}

UAbilitySystemComponent* AGGYGOCombatantState::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void AGGYGOCombatantState::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	check(AbilitySystemComponent);

	// 先建立稳定 Owner；放置实例或无延迟生成路径若已有 Avatar，直接走统一绑定。
	if (AvatarPawn)
	{
		SynchronizeAvatarBinding();
	}
	else
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, nullptr);
	}
}

void AGGYGOCombatantState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AGGYGOCombatantState, AvatarPawn);
}

void AGGYGOCombatantState::AttachAvatar(APawn* NewAvatar)
{
	if (!HasAuthority())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("AttachAvatar: 非权威端 [%s] 尝试绑定 Avatar [%s]，已拒绝。"),
			*GetNameSafe(this), *GetNameSafe(NewAvatar));
		return;
	}

	if (!NewAvatar)
	{
		DetachAvatar();
		return;
	}

	if (!UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(NewAvatar))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("AttachAvatar: Pawn [%s] 缺少 PawnExtension，状态宿主 [%s] 保留原 Avatar [%s]。"),
			*GetNameSafe(NewAvatar), *GetNameSafe(this), *GetNameSafe(AvatarPawn));
		return;
	}

	if (AvatarPawn != NewAvatar)
	{
		DetachAvatar(AvatarPawn);
		AvatarPawn = NewAvatar;
	}

	SynchronizeAvatarBinding();
	ForceNetUpdate();
}

void AGGYGOCombatantState::DetachAvatar(APawn* ExpectedAvatar)
{
	if (!HasAuthority())
	{
		return;
	}

	if (ExpectedAvatar && AvatarPawn != ExpectedAvatar)
	{
		// 旧 Pawn 的延迟清理不能把新 Avatar 摘掉。
		UE_LOG(LogGGYGOAbilitySystem, Verbose,
			TEXT("DetachAvatar: [%s] 期望 [%s]，当前为 [%s]，忽略过期请求。"),
			*GetNameSafe(this), *GetNameSafe(ExpectedAvatar), *GetNameSafe(AvatarPawn));
		return;
	}

	APawn* OldAvatar = AvatarPawn;
	if (!OldAvatar)
	{
		return;
	}

	if (UGGYGOPawnExtensionComponent* OldExtension =
		UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(OldAvatar))
	{
		OldExtension->UninitializeAbilitySystem();
	}
	else if (AbilitySystemComponent && AbilitySystemComponent->GetAvatarActor() == OldAvatar)
	{
		// 缺协调者属于异常退化路径；至少不能留下指向已销毁 Pawn 的 ActorInfo。
		AbilitySystemComponent->SetAvatarActor(nullptr);
	}

	AvatarPawn = nullptr;
	ForceNetUpdate();
}

void AGGYGOCombatantState::OnRep_AvatarPawn()
{
	SynchronizeAvatarBinding();
}

void AGGYGOCombatantState::SynchronizeAvatarBinding()
{
	if (!AbilitySystemComponent)
	{
		return;
	}

	APawn* ExistingAvatar = Cast<APawn>(AbilitySystemComponent->GetAvatarActor());
	if (ExistingAvatar && ExistingAvatar != AvatarPawn)
	{
		if (UGGYGOPawnExtensionComponent* ExistingExtension =
			UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(ExistingAvatar))
		{
			ExistingExtension->UninitializeAbilitySystem();
		}
		else
		{
			AbilitySystemComponent->SetAvatarActor(nullptr);
		}
	}

	if (!AvatarPawn)
	{
		// 保留 Owner，使无 Avatar 时属性、冷却和纯数值 GE 仍能继续。
		AbilitySystemComponent->InitAbilityActorInfo(this, nullptr);
		return;
	}

	if (UGGYGOPawnExtensionComponent* NewExtension =
		UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(AvatarPawn))
	{
		NewExtension->InitializeAbilitySystem(AbilitySystemComponent, this);
	}
	else
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SynchronizeAvatarBinding: Avatar [%s] 缺少 PawnExtension，无法绑定 [%s]。"),
			*GetNameSafe(AvatarPawn), *GetNameSafe(this));
	}
}
