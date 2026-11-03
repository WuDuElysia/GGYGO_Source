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
#include "FX/Components/GGYGOMontageFXReplicationComponent.h"
#include "System/GGYGOGameplayTags.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCombatantState)

AGGYGOCombatantState::AGGYGOCombatantState(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	SetReplicatingMovement(false);

	AbilitySystemComponent = CreateDefaultSubobject<UGGYGOAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);
	HealthSet = CreateDefaultSubobject<UGGYGOHealthSet>(TEXT("HealthSet"));
	CombatSet = CreateDefaultSubobject<UGGYGOCombatSet>(TEXT("CombatSet"));
	MontageFXReplication = CreateDefaultSubobject<UGGYGOMontageFXReplicationComponent>(TEXT("MontageFXReplication"));
}

UAbilitySystemComponent* AGGYGOCombatantState::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void AGGYGOCombatantState::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	check(AbilitySystemComponent);
	// 没有 Avatar 时 ASC 也要有 Owner：属性、GE 与能力授予不依赖 Pawn。
	AbilitySystemComponent->InitAbilityActorInfo(this, nullptr);
	if (AvatarPawn)
	{
		BindAvatar(AvatarPawn);
	}
}

void AGGYGOCombatantState::BeginPlay()
{
	if (IsActorBeginningPlay() && IsValid(this) && !IsActorBeingDestroyed())
	{
		bAvatarBindingClosed = false;
	}
	Super::BeginPlay();
}

void AGGYGOCombatantState::Destroyed()
{
	CloseAvatarBinding();
	Super::Destroyed();
}

void AGGYGOCombatantState::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	CloseAvatarBinding();
	Super::EndPlay(EndPlayReason);
}

void AGGYGOCombatantState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AGGYGOCombatantState, AvatarPawn);
}

EGGYGOAvatarBindResult AGGYGOCombatantState::AttachAvatar(APawn* NewAvatar)
{
	if (!HasAuthority())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("[Combatants] AttachAvatar authority denied: Host=%s Pawn=%s."),
			*GetPathNameSafe(this), *GetPathNameSafe(NewAvatar));
		return EGGYGOAvatarBindResult::AuthorityDenied;
	}
	return BindAvatar(NewAvatar);
}

void AGGYGOCombatantState::DetachAvatar(APawn* ExpectedAvatar)
{
	if (!HasAuthority() || !BoundAvatar.IsValid() || (ExpectedAvatar && BoundAvatar.Get() != ExpectedAvatar))
	{
		return;
	}
	BindAvatar(nullptr);
}

void AGGYGOCombatantState::ReleaseAvatarBinding(APawn* ExpectedAvatar)
{
	if (!ExpectedAvatar || BoundAvatar.Get() != ExpectedAvatar)
	{
		return;
	}
	BindAvatar(nullptr);
}

void AGGYGOCombatantState::OnRep_AvatarPawn()
{
	BindAvatar(AvatarPawn);
}

void AGGYGOCombatantState::HandleAvatarEndPlay(AActor* Actor, EEndPlayReason::Type EndPlayReason)
{
	HandleAvatarDestroyed(Actor);
}

void AGGYGOCombatantState::HandleAvatarDestroyed(AActor* DestroyedActor)
{
	// 只处理当前 Avatar；已被替换的旧 Pawn 在释放时已退订，这里再按身份过滤一次。
	if (!DestroyedActor || BoundAvatar.Get() != DestroyedActor)
	{
		return;
	}
	BindAvatar(nullptr);
}

EGGYGOAvatarBindResult AGGYGOCombatantState::BindAvatar(APawn* NewPawn)
{
	check(IsInGameThread());
	APawn* OldPawn = BoundAvatar.Get();
	const auto Reject = [&](EGGYGOAvatarBindResult Result, const TCHAR* Reason)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Combatants] BindAvatar rejected: Host=%s ASC=%s CurrentAvatar=%s RequestedPawn=%s Reason=%s."),
			*GetPathNameSafe(this), *GetPathNameSafe(AbilitySystemComponent), *GetPathNameSafe(OldPawn),
			*GetPathNameSafe(NewPawn), Reason);
		return Result;
	};
	if (bBindingInProgress)
	{
		return Reject(EGGYGOAvatarBindResult::Reentrant, TEXT("NestedRequestDuringBinding"));
	}
	UGGYGOPawnExtensionComponent* NewExtension = nullptr;
	if (NewPawn)
	{
		if (OldPawn == NewPawn)
		{
			return EGGYGOAvatarBindResult::AlreadyBound;
		}
		if (bAvatarBindingClosed || !IsValid(this) || IsActorBeingDestroyed())
		{
			return Reject(EGGYGOAvatarBindResult::LifecycleClosed, TEXT("HostLifecycleClosed"));
		}
		if (!IsValid(NewPawn) || NewPawn->IsActorBeingDestroyed())
		{
			return Reject(EGGYGOAvatarBindResult::InvalidPawn, TEXT("PawnInvalidOrBeingDestroyed"));
		}
		if (AbilitySystemComponent->HasMatchingGameplayTag(GGYGOGameplayTags::State_Dying)
			|| AbilitySystemComponent->HasMatchingGameplayTag(GGYGOGameplayTags::State_Dead))
		{
			return Reject(EGGYGOAvatarBindResult::HostDying, TEXT("HostDyingOrDead"));
		}
		NewExtension = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(NewPawn);
		const UGGYGOAbilitySystemComponent* OtherASC = NewExtension
			? NewExtension->GetCurrentLocalAbilitySystemResource().GetIdentity().ASC.Get() : nullptr;
		if (OtherASC && OtherASC != AbilitySystemComponent)
		{
			return Reject(EGGYGOAvatarBindResult::PawnBoundElsewhere, TEXT("PawnBoundToAnotherHost"));
		}
	}
	else if (!OldPawn && !BoundAvatar.IsStale())
	{
		if (HasAuthority()) { AvatarPawn = nullptr; }
		return EGGYGOAvatarBindResult::Cleared;
	}

	TGuardValue<bool> BindingGuard(bBindingInProgress, true);
	if (OldPawn || BoundAvatar.IsStale())
	{
		ReleaseAvatar(OldPawn, /*bClearActorInfo=*/false);
	}
	if (!NewPawn)
	{
		if (HasAuthority())
		{
			AvatarPawn = nullptr;
			ForceNetUpdate();
		}
		return EGGYGOAvatarBindResult::Cleared;
	}

	AbilitySystemComponent->InitAbilityActorInfo(this, NewPawn);
	BoundAvatar = NewPawn;
	NewPawn->OnEndPlay.AddUniqueDynamic(this, &ThisClass::HandleAvatarEndPlay);
	NewPawn->OnDestroyed.AddUniqueDynamic(this, &ThisClass::HandleAvatarDestroyed);
	if (HasAuthority())
	{
		AvatarPawn = NewPawn;
		ForceNetUpdate();
	}
	if (NewExtension)
	{
		NewExtension->HandleAbilitySystemBound(AbilitySystemComponent);
	}
	else
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Combatants] BindAvatar: Host=%s Avatar=%s 缺少 PawnExtension，本地消费方不会收到就绪。"),
			*GetPathNameSafe(this), *GetPathNameSafe(NewPawn));
	}
	return EGGYGOAvatarBindResult::Bound;
}

void AGGYGOCombatantState::ReleaseAvatar(APawn* OldPawn, bool bClearActorInfo)
{
	check(bBindingInProgress);
	BoundAvatar.Reset();
	if (OldPawn)
	{
		OldPawn->OnEndPlay.RemoveDynamic(this, &ThisClass::HandleAvatarEndPlay);
		OldPawn->OnDestroyed.RemoveDynamic(this, &ThisClass::HandleAvatarDestroyed);
	}
	// 死亡等需要跨 Avatar 存活的能力由 Tag 声明保留；其余能力不延续到下一个 Avatar。
	FGameplayTagContainer IgnoredAbilities;
	IgnoredAbilities.AddTag(GGYGOGameplayTags::Ability_Behavior_SurvivesDeath);
	AbilitySystemComponent->CancelAbilities(nullptr, &IgnoredAbilities);
	AbilitySystemComponent->ClearAbilityInput();
	AbilitySystemComponent->RemoveAllGameplayCues();
	// 与 Lyra 的 UninitializeAbilitySystem 相同：先解除 ActorInfo，再通知旧 Pawn 的消费方，
	// 收到 Released 时 ASC 已不再以旧 Pawn 为 Avatar。宿主仍存活时保留 Owner。
	if (bClearActorInfo)
	{
		AbilitySystemComponent->ClearActorInfo();
	}
	else
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, nullptr);
	}
	if (UGGYGOPawnExtensionComponent* OldExtension = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(OldPawn))
	{
		OldExtension->HandleAbilitySystemReleased();
	}
}

void AGGYGOCombatantState::CloseAvatarBinding()
{
	check(IsInGameThread());
	if (bAvatarBindingClosed)
	{
		return;
	}
	bAvatarBindingClosed = true;
	if (bBindingInProgress)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Combatants] CloseAvatarBinding: Host=%s 在绑定流程中结束，Avatar=%s 由进行中的流程收尾。"),
			*GetPathNameSafe(this), *GetPathNameSafe(BoundAvatar.Get()));
		return;
	}
	TGuardValue<bool> BindingGuard(bBindingInProgress, true);
	if (HasAuthority())
	{
		AvatarPawn = nullptr;
	}
	if (BoundAvatar.IsValid() || BoundAvatar.IsStale())
	{
		ReleaseAvatar(BoundAvatar.Get(), /*bClearActorInfo=*/true);
	}
	else
	{
		AbilitySystemComponent->ClearActorInfo();
	}
}
