/**
 * @file GGYGOHealthComponent.cpp
 * @brief 生命与韧性门面 + 死亡状态机实现
 */
#include "Character/Components/GGYGOHealthComponent.h"

#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "GameFramework/Pawn.h"
#include "GameplayEffect.h"
#include "GameplayEffectExtension.h"
#include "GameplayEffectTypes.h"
#include "Net/UnrealNetwork.h"
#include "System/GGYGOGameData.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHealthComponent)

class FLifetimeProperty;

struct UGGYGOHealthComponent::FAbilitySystemResource
{
	TWeakObjectPtr<UGGYGOHealthComponent> Health{};
	TWeakObjectPtr<APawn> Pawn{};
	TWeakObjectPtr<UGGYGOPawnExtensionComponent> Extension{};
	FGGYGOPawnASCResourceHandle Resource{};
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC{};
	TWeakObjectPtr<const UGGYGOHealthSet> HealthSet{};
	FGGYGOAvatarBindingContext PublishedContext{};
	FDelegateHandle HealthChanged{};
	FDelegateHandle MaxHealthChanged{};
	FDelegateHandle OutOfHealth{};
	FDelegateHandle PoiseChanged{};
	FDelegateHandle PoiseBroken{};
	bool bRetired = false;
};

namespace
{
	bool RejectHealthResource(FString& OutError, const UGGYGOHealthComponent* Health,
		const FGGYGOPawnASCResourceHandle& Resource, const FGGYGOAvatarBindingContext& Context,
		const TCHAR* Reason)
	{
		const FGGYGOPawnASCResourceIdentity Identity = Resource.GetIdentity();
		OutError = FString::Printf(
			TEXT("[Character/Health] Health='%s' Pawn='%s' ASC='%s' Binding=%llu Write=%llu Reason='%s'."),
			*GetPathNameSafe(Health), *GetPathNameSafe(Identity.Pawn.Get()), *GetPathNameSafe(Identity.ASC.Get()),
			static_cast<unsigned long long>(Context.Binding.Serial),
			static_cast<unsigned long long>(Context.LastActorInfoWrite.Serial), Reason);
		return false;
	}
}

UGGYGOHealthComponent::UGGYGOHealthComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 纯事件驱动，不需要 Tick。
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	SetIsReplicatedByDefault(true);

	DeathState = EGGYGODeathState::NotDead;
}

void UGGYGOHealthComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UGGYGOHealthComponent, DeathState);
}

void UGGYGOHealthComponent::OnRegister()
{
	if (IsValid(this) && !IsBeingDestroyed() && !HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		&& IsValid(GetOwner()) && !GetOwner()->IsActorBeingDestroyed())
	{
		bResourceAdmissionClosed = false;
	}
	Super::OnRegister();
}

void UGGYGOHealthComponent::BeginPlay()
{
	AActor* Owner = GetOwner();
	if (!HasBegunPlay() && IsRegistered() && IsValid(this) && !IsBeingDestroyed()
		&& Owner && !Owner->IsActorBeingDestroyed()
		&& (Owner->IsActorBeginningPlay() || Owner->HasActorBegunPlay()))
	{
		bResourceAdmissionClosed = false;
	}
	Super::BeginPlay();
}

void UGGYGOHealthComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bResourceAdmissionClosed = true;
	const TSharedPtr<FAbilitySystemResource> OriginalResource = AbilitySystemResource;
	RetireOriginalResource(OriginalResource);
	Super::EndPlay(EndPlayReason);
}

void UGGYGOHealthComponent::OnUnregister()
{
	bResourceAdmissionClosed = true;
	const TSharedPtr<FAbilitySystemResource> OriginalResource = AbilitySystemResource;
	RetireOriginalResource(OriginalResource);
	Super::OnUnregister();
}

bool UGGYGOHealthComponent::ValidateLocalReadyResource(
	UGGYGOPawnExtensionComponent* ExpectedExtension,
	const FGGYGOPawnASCResourceHandle& ExpectedResource,
	const FGGYGOAvatarBindingContext& PublishedContext,
	UGGYGOAbilitySystemComponent*& OutASC, const UGGYGOHealthSet*& OutHealthSet, FString& OutError) const
{
	OutASC = nullptr;
	OutHealthSet = nullptr;
	if (bResourceAdmissionClosed || !IsRegistered() || !IsValid(this) || IsBeingDestroyed()
		|| HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return RejectHealthResource(OutError, this, ExpectedResource, PublishedContext, TEXT("LifecycleClosed"));
	}
	APawn* Pawn = Cast<APawn>(GetOwner());
	const FGGYGOPawnASCResourceIdentity Identity = ExpectedResource.GetIdentity();
	if (!IsValid(Pawn) || Pawn->IsActorBeingDestroyed() || !IsValid(ExpectedExtension)
		|| ExpectedExtension != UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Pawn)
		|| ExpectedExtension->GetOwner() != Pawn
		|| !Identity.Pawn.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Pawn)))
	{
		return RejectHealthResource(OutError, this, ExpectedResource, PublishedContext, TEXT("OwnerOrExtensionMismatch"));
	}
	UGGYGOAbilitySystemComponent* ASC = Identity.ASC.Get();
	if (!ExpectedResource.HasResource() || !PublishedContext.HasIssuedContext()
		|| !Identity.Binding.HasSameIdentity(PublishedContext.Binding) || !ASC
		|| !ExpectedExtension->IsLocalAbilitySystemResourceReady(ExpectedResource)
		|| !ASC->IsAvatarBindingPublicationContextCurrent(PublishedContext))
	{
		return RejectHealthResource(OutError, this, ExpectedResource, PublishedContext, TEXT("OriginalResourceNotReady"));
	}
	const UGGYGOHealthSet* Set = ASC->GetSet<UGGYGOHealthSet>();
	if (!IsValid(Set) || Set->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return RejectHealthResource(OutError, this, ExpectedResource, PublishedContext, TEXT("MissingHealthSet"));
	}
	OutASC = ASC;
	OutHealthSet = Set;
	return true;
}

bool UGGYGOHealthComponent::IsOriginalResourceCurrent(
	const TSharedPtr<FAbilitySystemResource>& ExpectedResource,
	const FGGYGOAvatarBindingContext& ExpectedContext)
{
	if (!ExpectedResource.IsValid() || ExpectedResource->bRetired
		|| !ExpectedResource->PublishedContext.HasSameContext(ExpectedContext)
		|| !ExpectedResource->HealthChanged.IsValid() || !ExpectedResource->MaxHealthChanged.IsValid()
		|| !ExpectedResource->OutOfHealth.IsValid() || !ExpectedResource->PoiseChanged.IsValid()
		|| !ExpectedResource->PoiseBroken.IsValid())
	{
		return false;
	}
	UGGYGOHealthComponent* Health = ExpectedResource->Health.Get();
	APawn* Pawn = ExpectedResource->Pawn.Get();
	UGGYGOPawnExtensionComponent* Extension = ExpectedResource->Extension.Get();
	UGGYGOAbilitySystemComponent* ASC = ExpectedResource->ASC.Get();
	const UGGYGOHealthSet* Set = ExpectedResource->HealthSet.Get();
	const FGGYGOPawnASCResourceIdentity Identity = ExpectedResource->Resource.GetIdentity();
	return Health && !Health->bResourceAdmissionClosed && Health->IsRegistered()
		&& !Health->IsBeingDestroyed() && !Health->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		&& Health->AbilitySystemResource == ExpectedResource
		&& Pawn && !Pawn->IsActorBeingDestroyed() && Health->GetOwner() == Pawn
		&& Extension && Extension->GetOwner() == Pawn
		&& Extension == UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Pawn)
		&& ASC && Set && !Set->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		&& Identity.ASC.HasSameIndexAndSerialNumber(ExpectedResource->ASC)
		&& Identity.Pawn.HasSameIndexAndSerialNumber(ExpectedResource->Pawn)
		&& Identity.Binding.HasSameIdentity(ExpectedContext.Binding)
		&& Extension->IsLocalAbilitySystemResourceReady(ExpectedResource->Resource)
		&& ASC->IsAvatarBindingPublicationContextCurrent(ExpectedContext)
		&& ASC->GetSet<UGGYGOHealthSet>() == Set;
}

void UGGYGOHealthComponent::RetireOriginalResource(const TSharedPtr<FAbilitySystemResource>& ExpectedResource)
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = ExpectedResource;
	if (!OriginalResource.IsValid() || OriginalResource->bRetired) { return; }
	// 先退休并摘自身原槽。此后只操作这份记录及其原 Set/token，不写当前后继。
	OriginalResource->bRetired = true;
	if (UGGYGOHealthComponent* Health = OriginalResource->Health.Get();
		Health && Health->AbilitySystemResource == OriginalResource)
	{
		Health->AbilitySystemResource.Reset();
	}
	const FDelegateHandle HealthChanged = OriginalResource->HealthChanged;
	const FDelegateHandle MaxHealthChanged = OriginalResource->MaxHealthChanged;
	const FDelegateHandle OutOfHealth = OriginalResource->OutOfHealth;
	const FDelegateHandle PoiseChanged = OriginalResource->PoiseChanged;
	const FDelegateHandle PoiseBroken = OriginalResource->PoiseBroken;
	OriginalResource->HealthChanged.Reset();
	OriginalResource->MaxHealthChanged.Reset();
	OriginalResource->OutOfHealth.Reset();
	OriginalResource->PoiseChanged.Reset();
	OriginalResource->PoiseBroken.Reset();
	if (const UGGYGOHealthSet* OriginalSet = OriginalResource->HealthSet.Get();
		OriginalSet && !OriginalSet->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		if (HealthChanged.IsValid()) { OriginalSet->OnHealthChanged.Remove(HealthChanged); }
		if (MaxHealthChanged.IsValid()) { OriginalSet->OnMaxHealthChanged.Remove(MaxHealthChanged); }
		if (OutOfHealth.IsValid()) { OriginalSet->OnOutOfHealth.Remove(OutOfHealth); }
		if (PoiseChanged.IsValid()) { OriginalSet->OnPoiseChanged.Remove(PoiseChanged); }
		if (PoiseBroken.IsValid()) { OriginalSet->OnPoiseBroken.Remove(PoiseBroken); }
	}
	// Set 已失效时只退休这份历史义务，不查找新 Set 或宣称发出了通知。
}

TSharedPtr<UGGYGOHealthComponent::FAbilitySystemResource> UGGYGOHealthComponent::GetReadyResource() const
{
	check(IsInGameThread());
	const TSharedPtr<FAbilitySystemResource> OriginalResource = AbilitySystemResource;
	return OriginalResource.IsValid()
		&& IsOriginalResourceCurrent(OriginalResource, OriginalResource->PublishedContext)
		? OriginalResource : TSharedPtr<FAbilitySystemResource>{};
}

bool UGGYGOHealthComponent::ProjectDeathStateToOriginalResource(
	const TSharedPtr<FAbilitySystemResource>& ExpectedResource,
	const FGGYGOAvatarBindingContext& ExpectedContext)
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = ExpectedResource;
	const FGGYGOAvatarBindingContext OriginalContext = ExpectedContext;
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return false; }
	const EGGYGODeathState OriginalDeathState = OriginalResource->Health.Get()->DeathState;
	UGGYGOAbilitySystemComponent* OriginalASC = OriginalResource->ASC.Get();
	if (OriginalDeathState >= EGGYGODeathState::DeathStarted)
	{
		OriginalASC->SetLooseGameplayTagCount(GGYGOGameplayTags::State_Dying, 1);
		if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return false; }
	}
	if (OriginalDeathState >= EGGYGODeathState::DeathFinished)
	{
		OriginalASC = OriginalResource->ASC.Get();
		OriginalASC->SetLooseGameplayTagCount(GGYGOGameplayTags::State_Dead, 1);
		if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return false; }
	}
	return true;
}

bool UGGYGOHealthComponent::InitializeWithLocalAbilitySystemResource(
	UGGYGOPawnExtensionComponent* ExpectedExtension,
	const FGGYGOPawnASCResourceHandle& ExpectedResource,
	const FGGYGOAvatarBindingContext& PublishedContext, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(ExpectedExtension);
	const FGGYGOPawnASCResourceHandle OriginalHandle = ExpectedResource;
	const FGGYGOAvatarBindingContext OriginalContext = PublishedContext;
	UGGYGOAbilitySystemComponent* OriginalASC;
	const UGGYGOHealthSet* OriginalSet;
	if (!ValidateLocalReadyResource(OriginalExtension.Get(), OriginalHandle, OriginalContext,
		OriginalASC, OriginalSet, OutError)) { return false; }
	const TSharedPtr<FAbilitySystemResource> Existing = AbilitySystemResource;
	if (Existing.IsValid() && !Existing->bRetired)
	{
		if (Existing->Resource.HasSameResource(OriginalHandle))
		{
			// 同 H 走同一认证/投影实现，不重复 token 或 UI 初值。
			return RefreshLocalAbilitySystemResource(OriginalExtension.Get(), OriginalHandle, OriginalContext, OutError);
		}
		UGGYGOPawnExtensionComponent* PreviousSource = Existing->Extension.Get();
		if (PreviousSource && PreviousSource->IsLocalAbilitySystemResourceInstalled(Existing->Resource))
		{
			return RejectHealthResource(OutError, this, OriginalHandle, OriginalContext, TEXT("ResourceConflict"));
		}
		// 新请求明确指定真实 Ready H；仅退休已失效的旧 Health 自有记录，再复核本请求。
		RetireOriginalResource(Existing);
		if (!ValidateLocalReadyResource(OriginalExtension.Get(), OriginalHandle, OriginalContext,
			OriginalASC, OriginalSet, OutError)) { return false; }
		if (AbilitySystemResource.IsValid())
		{
			return RejectHealthResource(OutError, this, OriginalHandle, OriginalContext, TEXT("CallbackInvalidated"));
		}
	}
	const TSharedPtr<FAbilitySystemResource> OriginalResource = MakeShared<FAbilitySystemResource>();
	OriginalResource->Health = this;
	OriginalResource->Pawn = Cast<APawn>(GetOwner());
	OriginalResource->Extension = OriginalExtension;
	OriginalResource->Resource = OriginalHandle;
	OriginalResource->ASC = OriginalASC;
	OriginalResource->HealthSet = OriginalSet;
	OriginalResource->PublishedContext = OriginalContext;
	AbilitySystemResource = OriginalResource;
	OriginalResource->HealthChanged = OriginalSet->OnHealthChanged.AddLambda(
		[OriginalResource](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
		{
			// 局部副本保住本次调用；Context 按事件入口读取，不固定为 Add 时的安装 Context。
			const TSharedPtr<FAbilitySystemResource> EventResource = OriginalResource;
			const FGGYGOAvatarBindingContext EventContext = EventResource->PublishedContext;
			if (UGGYGOHealthComponent* Health = EventResource->Health.Get();
				Health && IsOriginalResourceCurrent(EventResource, EventContext))
			{
				Health->HandleHealthChanged(EventResource, EventContext, Instigator, Causer, Spec, Magnitude, OldValue, NewValue);
			}
		});
	OriginalResource->MaxHealthChanged = OriginalSet->OnMaxHealthChanged.AddLambda(
		[OriginalResource](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
		{
			// 局部副本保住本次调用；Context 按事件入口读取，不固定为 Add 时的安装 Context。
			const TSharedPtr<FAbilitySystemResource> EventResource = OriginalResource;
			const FGGYGOAvatarBindingContext EventContext = EventResource->PublishedContext;
			if (UGGYGOHealthComponent* Health = EventResource->Health.Get();
				Health && IsOriginalResourceCurrent(EventResource, EventContext))
			{
				Health->HandleMaxHealthChanged(EventResource, EventContext, Instigator, Causer, Spec, Magnitude, OldValue, NewValue);
			}
		});
	OriginalResource->OutOfHealth = OriginalSet->OnOutOfHealth.AddLambda(
		[OriginalResource](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
		{
			// 局部副本保住本次调用；Context 按事件入口读取，不固定为 Add 时的安装 Context。
			const TSharedPtr<FAbilitySystemResource> EventResource = OriginalResource;
			const FGGYGOAvatarBindingContext EventContext = EventResource->PublishedContext;
			if (UGGYGOHealthComponent* Health = EventResource->Health.Get();
				Health && IsOriginalResourceCurrent(EventResource, EventContext))
			{
				Health->HandleOutOfHealth(EventResource, EventContext, Instigator, Causer, Spec, Magnitude, OldValue, NewValue);
			}
		});
	OriginalResource->PoiseChanged = OriginalSet->OnPoiseChanged.AddLambda(
		[OriginalResource](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
		{
			// 局部副本保住本次调用；Context 按事件入口读取，不固定为 Add 时的安装 Context。
			const TSharedPtr<FAbilitySystemResource> EventResource = OriginalResource;
			const FGGYGOAvatarBindingContext EventContext = EventResource->PublishedContext;
			if (UGGYGOHealthComponent* Health = EventResource->Health.Get();
				Health && IsOriginalResourceCurrent(EventResource, EventContext))
			{
				Health->HandlePoiseChanged(EventResource, EventContext, Instigator, Causer, Spec, Magnitude, OldValue, NewValue);
			}
		});
	OriginalResource->PoiseBroken = OriginalSet->OnPoiseBroken.AddLambda(
		[OriginalResource](AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
		{
			// 局部副本保住本次调用；Context 按事件入口读取，不固定为 Add 时的安装 Context。
			const TSharedPtr<FAbilitySystemResource> EventResource = OriginalResource;
			const FGGYGOAvatarBindingContext EventContext = EventResource->PublishedContext;
			if (UGGYGOHealthComponent* Health = EventResource->Health.Get();
				Health && IsOriginalResourceCurrent(EventResource, EventContext))
			{
				Health->HandlePoiseBroken(EventResource, EventContext, Instigator, Causer, Spec, Magnitude, OldValue, NewValue);
			}
		});
	const auto FailOriginalInstall = [&]()
	{
		// 后继刷新同记录 Context 或同 H 重建另一记录时，不退休其有效装配。
		if (!OriginalResource->bRetired && OriginalResource->PublishedContext.HasSameContext(OriginalContext))
		{
			RetireOriginalResource(OriginalResource);
		}
		return RejectHealthResource(OutError, OriginalResource->Health.Get(), OriginalHandle, OriginalContext,
			TEXT("OriginalInstallInvalidated"));
	};
	if (!ProjectDeathStateToOriginalResource(OriginalResource, OriginalContext)) { return FailOriginalInstall(); }

	// 三次初值来自捕获的原 Set；每次广播后只复核本次记录/H/Set/Context。
	OriginalSet = OriginalResource->HealthSet.Get();
	OriginalResource->Health.Get()->OnHealthChanged.Broadcast(
		OriginalResource->Health.Get(), 0.0f, OriginalSet->GetHealth(), nullptr);
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return FailOriginalInstall(); }
	OriginalSet = OriginalResource->HealthSet.Get();
	OriginalResource->Health.Get()->OnMaxHealthChanged.Broadcast(
		OriginalResource->Health.Get(), 0.0f, OriginalSet->GetMaxHealth(), nullptr);
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return FailOriginalInstall(); }
	OriginalSet = OriginalResource->HealthSet.Get();
	OriginalResource->Health.Get()->OnPoiseChanged.Broadcast(
		OriginalResource->Health.Get(), 0.0f, OriginalSet->GetPoise(), nullptr);
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return FailOriginalInstall(); }
	return true;
}

bool UGGYGOHealthComponent::RefreshLocalAbilitySystemResource(
	UGGYGOPawnExtensionComponent* ExpectedExtension,
	const FGGYGOPawnASCResourceHandle& ExpectedResource,
	const FGGYGOAvatarBindingContext& PublishedContext, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(ExpectedExtension);
	const FGGYGOPawnASCResourceHandle OriginalHandle = ExpectedResource;
	const FGGYGOAvatarBindingContext OriginalContext = PublishedContext;
	UGGYGOAbilitySystemComponent* ASC;
	const UGGYGOHealthSet* Set;
	if (!ValidateLocalReadyResource(OriginalExtension.Get(), OriginalHandle, OriginalContext,
		ASC, Set, OutError)) { return false; }
	const TSharedPtr<FAbilitySystemResource> OriginalResource = AbilitySystemResource;
	if (!OriginalResource.IsValid() || OriginalResource->bRetired
		|| !OriginalResource->Resource.HasSameResource(OriginalHandle)
		|| !OriginalResource->Extension.HasSameIndexAndSerialNumber(OriginalExtension)
		|| OriginalResource->ASC.Get() != ASC || OriginalResource->HealthSet.Get() != Set)
	{
		return RejectHealthResource(OutError, this, OriginalHandle, OriginalContext, TEXT("OriginalRecordOrHealthSetMismatch"));
	}
	// 更新同记录的认证元数据；五个 lambda 下次调用会取新 Context，不重装或重放初值。
	OriginalResource->PublishedContext = OriginalContext;
	if (!ProjectDeathStateToOriginalResource(OriginalResource, OriginalContext))
	{
		return RejectHealthResource(OutError, OriginalResource->Health.Get(), OriginalHandle, OriginalContext,
			TEXT("OriginalRefreshInvalidated"));
	}
	return true;
}

bool UGGYGOHealthComponent::UninitializeFromLocalAbilitySystemResource(
	const FGGYGOPawnASCResourceHandle& ExpectedResource, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	const FGGYGOPawnASCResourceHandle OriginalHandle = ExpectedResource;
	if (!OriginalHandle.HasResource())
	{
		return RejectHealthResource(OutError, this, OriginalHandle, FGGYGOAvatarBindingContext{}, TEXT("MissingOriginalResource"));
	}
	const TSharedPtr<FAbilitySystemResource> OriginalResource = AbilitySystemResource;
	if (!OriginalResource.IsValid()) { return true; }
	if (!OriginalResource->Resource.HasSameResource(OriginalHandle))
	{
		return RejectHealthResource(OutError, this, OriginalHandle, OriginalResource->PublishedContext,
			TEXT("ResourceMismatch"));
	}
	RetireOriginalResource(OriginalResource);
	return true;
}

void UGGYGOHealthComponent::InitializeWithAbilitySystem(UGGYGOAbilitySystemComponent* InASC)
{
	check(IsInGameThread());
	UGGYGOPawnExtensionComponent* Source = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(GetOwner());
	const FGGYGOPawnASCResourceHandle OriginalHandle =
		Source ? Source->GetCurrentLocalAbilitySystemResource() : FGGYGOPawnASCResourceHandle{};
	const FGGYGOPawnASCResourceIdentity Identity = OriginalHandle.GetIdentity();
	FGGYGOAvatarBindingContext OriginalContext;
	if (IsValid(InASC) && Identity.ASC.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGGYGOAbilitySystemComponent>(InASC)))
	{
		OriginalContext = InASC->GetAvatarBindingContext();
	}
	FString Error;
	if (!IsValid(InASC) || Identity.ASC.Get() != InASC)
	{
		RejectHealthResource(Error, this, OriginalHandle, OriginalContext, TEXT("ExpectedASCMismatchOrMissingResource"));
	}
	else if (InitializeWithLocalAbilitySystemResource(Source, OriginalHandle, OriginalContext, Error))
	{
		return;
	}
	UE_LOG(LogGGYGOAbilitySystem, Warning,
		TEXT("[Character/Health] InitializeWithAbilitySystem RequestedASC='%s': %s"), *GetPathNameSafe(InASC), *Error);
}

void UGGYGOHealthComponent::UninitializeFromAbilitySystem()
{
	check(IsInGameThread());
	const TSharedPtr<FAbilitySystemResource> OriginalResource = AbilitySystemResource;
	if (!OriginalResource.IsValid()) { return; }
	FString Error;
	if (!UninitializeFromLocalAbilitySystemResource(OriginalResource->Resource, Error))
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("%s"), *Error);
	}
}

void UGGYGOHealthComponent::ApplyDeathStateToAbilitySystem()
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = GetReadyResource();
	if (OriginalResource.IsValid())
	{
		const FGGYGOAvatarBindingContext OriginalContext = OriginalResource->PublishedContext;
		ProjectDeathStateToOriginalResource(OriginalResource, OriginalContext);
	}
}

float UGGYGOHealthComponent::GetHealth() const
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = GetReadyResource();
	return OriginalResource.IsValid() ? OriginalResource->HealthSet.Get()->GetHealth() : 0.0f;
}

float UGGYGOHealthComponent::GetMaxHealth() const
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = GetReadyResource();
	return OriginalResource.IsValid() ? OriginalResource->HealthSet.Get()->GetMaxHealth() : 0.0f;
}

float UGGYGOHealthComponent::GetPoise() const
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = GetReadyResource();
	return OriginalResource.IsValid() ? OriginalResource->HealthSet.Get()->GetPoise() : 0.0f;
}

float UGGYGOHealthComponent::GetMaxPoise() const
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = GetReadyResource();
	return OriginalResource.IsValid() ? OriginalResource->HealthSet.Get()->GetMaxPoise() : 0.0f;
}

float UGGYGOHealthComponent::GetHealthNormalized() const
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = GetReadyResource();
	if (!OriginalResource.IsValid()) { return 0.0f; }
	const UGGYGOHealthSet* OriginalSet = OriginalResource->HealthSet.Get();
	const float Value = OriginalSet->GetHealth();
	const float Maximum = OriginalSet->GetMaxHealth();
	return Maximum > 0.0f ? Value / Maximum : 0.0f;
}

float UGGYGOHealthComponent::GetPoiseNormalized() const
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = GetReadyResource();
	if (!OriginalResource.IsValid()) { return 0.0f; }
	const UGGYGOHealthSet* OriginalSet = OriginalResource->HealthSet.Get();
	const float Value = OriginalSet->GetPoise();
	const float Maximum = OriginalSet->GetMaxPoise();
	return Maximum > 0.0f ? Value / Maximum : 0.0f;
}

void UGGYGOHealthComponent::HandleHealthChanged(
	const TSharedPtr<FAbilitySystemResource>& ExpectedResource,
	const FGGYGOAvatarBindingContext& ExpectedContext,
	AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = ExpectedResource;
	const FGGYGOAvatarBindingContext OriginalContext = ExpectedContext;
	if (!OriginalResource.IsValid() || OriginalResource->Health.Get() != this
		|| !IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
	OnHealthChanged.Broadcast(OriginalResource->Health.Get(), OldValue, NewValue, Instigator);
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
}

void UGGYGOHealthComponent::HandleMaxHealthChanged(
	const TSharedPtr<FAbilitySystemResource>& ExpectedResource,
	const FGGYGOAvatarBindingContext& ExpectedContext,
	AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = ExpectedResource;
	const FGGYGOAvatarBindingContext OriginalContext = ExpectedContext;
	if (!OriginalResource.IsValid() || OriginalResource->Health.Get() != this
		|| !IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
	OnMaxHealthChanged.Broadcast(OriginalResource->Health.Get(), OldValue, NewValue, Instigator);
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
}

void UGGYGOHealthComponent::HandlePoiseChanged(
	const TSharedPtr<FAbilitySystemResource>& ExpectedResource,
	const FGGYGOAvatarBindingContext& ExpectedContext,
	AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = ExpectedResource;
	const FGGYGOAvatarBindingContext OriginalContext = ExpectedContext;
	if (!OriginalResource.IsValid() || OriginalResource->Health.Get() != this
		|| !IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
	OnPoiseChanged.Broadcast(OriginalResource->Health.Get(), OldValue, NewValue, Instigator);
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
}

void UGGYGOHealthComponent::HandlePoiseBroken(
	const TSharedPtr<FAbilitySystemResource>& ExpectedResource,
	const FGGYGOAvatarBindingContext& ExpectedContext,
	AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
{
	const TSharedPtr<FAbilitySystemResource> OriginalResource = ExpectedResource;
	const FGGYGOAvatarBindingContext OriginalContext = ExpectedContext;
	if (!OriginalResource.IsValid() || OriginalResource->Health.Get() != this
		|| !IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
	OnPoiseBroken.Broadcast(OriginalResource->Pawn.Get());
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
}

void UGGYGOHealthComponent::HandleOutOfHealth(
	const TSharedPtr<FAbilitySystemResource>& ExpectedResource,
	const FGGYGOAvatarBindingContext& ExpectedContext,
	AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
{
#if WITH_SERVER_CODE
	const TSharedPtr<FAbilitySystemResource> OriginalResource = ExpectedResource;
	const FGGYGOAvatarBindingContext OriginalContext = ExpectedContext;
	if (!Spec || !OriginalResource.IsValid() || OriginalResource->Health.Get() != this
		|| !IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
	UGGYGOAbilitySystemComponent* OriginalASC = OriginalResource->ASC.Get();
	// 死亡仍只发原 GameplayEvent 给 GA；目标和 ASC来自这次原记录。
	FGameplayEventData Payload;
	Payload.EventTag = GGYGOGameplayTags::Event_Death;
	Payload.Instigator = Instigator;
	Payload.Target = OriginalResource->Pawn.Get();
	Payload.OptionalObject = Spec->Def;
	Payload.ContextHandle = Spec->GetEffectContext();
	Payload.InstigatorTags = *Spec->CapturedSourceTags.GetAggregatedTags();
	Payload.TargetTags = *Spec->CapturedTargetTags.GetAggregatedTags();
	Payload.EventMagnitude = Magnitude;

	FScopedPredictionWindow NewScopedWindow(OriginalASC, true);
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
	OriginalASC->HandleGameplayEvent(Payload.EventTag, &Payload);
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
#endif // WITH_SERVER_CODE
}

void UGGYGOHealthComponent::OnRep_DeathState(EGGYGODeathState OldDeathState)
{
	const EGGYGODeathState NewDeathState = DeathState;

	// 先回滚到旧值：状态推进必须经过 StartDeath / FinishDeath，
	// 它们除了改状态还要施加 Tag 和广播事件。直接接受复制值会跳过那些副作用。
	DeathState = OldDeathState;

	if (OldDeathState > NewDeathState)
	{
		// 本地已经预测到比服务器更靠后的阶段。不回退 ——
		// 死亡表现已经播了，倒回去会出现"起身又倒下"。
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("OnRep_DeathState: [%s] 本地已预测超过服务器状态 [%d] -> [%d]，忽略。"),
			*GetNameSafe(GetOwner()), (uint8)OldDeathState, (uint8)NewDeathState);
		return;
	}

	// 把跳级变化补成逐级调用。
	// 两次状态变化可能被合并进同一个网络包（角色秒死时很常见），
	// 客户端直接看到 NotDead → DeathFinished，不补 StartDeath 就会丢掉整段死亡表现。
	if (OldDeathState == EGGYGODeathState::NotDead)
	{
		if (NewDeathState == EGGYGODeathState::DeathStarted)
		{
			StartDeath();
		}
		else if (NewDeathState == EGGYGODeathState::DeathFinished)
		{
			StartDeath();
			FinishDeath();
		}
		else
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("OnRep_DeathState: [%s] 非法状态转换 [%d] -> [%d]。"),
				*GetNameSafe(GetOwner()), (uint8)OldDeathState, (uint8)NewDeathState);
		}
	}
	else if (OldDeathState == EGGYGODeathState::DeathStarted)
	{
		if (NewDeathState == EGGYGODeathState::DeathFinished)
		{
			FinishDeath();
		}
		else
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("OnRep_DeathState: [%s] 非法状态转换 [%d] -> [%d]。"),
				*GetNameSafe(GetOwner()), (uint8)OldDeathState, (uint8)NewDeathState);
		}
	}

	ensureMsgf((DeathState == NewDeathState),
		TEXT("OnRep_DeathState: [%s] 状态机未能对齐服务器，期望 [%d] 实际 [%d]。"),
		*GetNameSafe(GetOwner()), (uint8)NewDeathState, (uint8)DeathState);
}

void UGGYGOHealthComponent::StartDeath()
{
	if (DeathState != EGGYGODeathState::NotDead)
	{
		return;
	}

	DeathState = EGGYGODeathState::DeathStarted;
	ApplyDeathStateToAbilitySystem();

	AActor* Owner = GetOwner();
	check(Owner);

	OnDeathStarted.Broadcast(Owner);

	// 立即同步。死亡是玩家能立刻看到的事件，走默认复制节流会有可感知的延迟。
	Owner->ForceNetUpdate();
}

void UGGYGOHealthComponent::FinishDeath()
{
	if (DeathState != EGGYGODeathState::DeathStarted)
	{
		return;
	}

	DeathState = EGGYGODeathState::DeathFinished;
	ApplyDeathStateToAbilitySystem();

	AActor* Owner = GetOwner();
	check(Owner);

	OnDeathFinished.Broadcast(Owner);

	Owner->ForceNetUpdate();
}

void UGGYGOHealthComponent::DamageSelfDestruct(bool bFellOutOfWorld)
{
	if (DeathState != EGGYGODeathState::NotDead)
	{
		// 已经在死亡流程里，再补一次伤害没有意义。
		return;
	}

	const TSharedPtr<FAbilitySystemResource> OriginalResource = GetReadyResource();
	if (!OriginalResource.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("DamageSelfDestruct: [%s] 没有经过认证的原 Health Ready 资源。"), *GetNameSafe(GetOwner()));
		return;
	}
	const FGGYGOAvatarBindingContext OriginalContext = OriginalResource->PublishedContext;
	UGGYGOAbilitySystemComponent* OriginalASC = OriginalResource->ASC.Get();

	// 非空覆盖优先；空覆盖只读取启动预载快照，不在自毁路径加载或重试。
	TSubclassOf<UGameplayEffect> EffectToApply = SelfDestructEffectOverride;
	if (!EffectToApply)
	{
		EffectToApply = UGGYGOGameData::GetSharedSelfDestructGameplayEffect();
	}

	if (!EffectToApply)
	{
		// 显式报错而不是退化为直接改属性。直接改会绕过免疫判定与元属性消费，
		// 让"无敌帧内掉出世界"的行为与正常受伤不一致。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("DamageSelfDestruct: [%s] 未配置SelfDestructEffectOverride，且启动预载的共享自毁GE不可用（Manager/预载未就绪、共享配置缺失或加载失败）；本次不施加自毁伤害。请检查System启动诊断及GameData的SelfDestructGameplayEffect配置。"),
			*GetNameSafe(GetOwner()));
		return;
	}

	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
	FGameplayEffectContextHandle Context = OriginalASC->MakeEffectContext();
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
	Context.AddSourceObject(this);

	const FGameplayEffectSpecHandle SpecHandle = OriginalASC->MakeOutgoingSpec(EffectToApply, 1.0f, Context);
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
	if (!SpecHandle.IsValid() || !SpecHandle.Data.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("DamageSelfDestruct: [%s] 无法为 [%s] 创建 GE Spec。"),
			*GetNameSafe(GetOwner()), *GetNameSafe(EffectToApply));
		return;
	}

	FGameplayEffectSpec* Spec = SpecHandle.Data.Get();

	// 必须加这个动态 Tag，否则自毁会被无敌帧和开发期 GodMode 挡住。
	// `UGGYGOHealthSet::PreGameplayEffectExecute` 检查的正是 spec 的动态资产 Tag：
	// 有 SelfDestruct 就跳过 Gameplay.Damage.Immunity 与 Cheat.GodMode 两道拦截。
	// 注意它对 bFellOutOfWorld 无条件生效 —— 自毁的定义就是"必须死成"。
	Spec->AddDynamicAssetTag(GGYGOGameplayTags::Gameplay_Damage_SelfDestruct);

	if (bFellOutOfWorld)
	{
		// 死因标记。免疫穿透已由上面的 SelfDestruct Tag 负责，这个 Tag 只用于区分死法，
		// 供死亡 Cue 与死亡消息选择表现（掉出世界不该播倒地动画）。
		Spec->AddDynamicAssetTag(GGYGOGameplayTags::Gameplay_Damage_FellOutOfWorld);
	}

	// 伤害量给足以致死的值。用当前最大生命而不是一个魔数，
	// 这样上限被 Buff 抬高时也仍然致死。
	const float DamageAmount = FMath::Max(OriginalResource->HealthSet.Get()->GetMaxHealth(), 1.0f);
	Spec->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, DamageAmount);

	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
	OriginalASC->ApplyGameplayEffectSpecToSelf(*Spec);
	if (!IsOriginalResourceCurrent(OriginalResource, OriginalContext)) { return; }
}
