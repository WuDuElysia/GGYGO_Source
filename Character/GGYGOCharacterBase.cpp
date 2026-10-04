/**
 * @file GGYGOCharacterBase.cpp
 * @brief 新版角色基类实现
 */
#include "Character/GGYGOCharacterBase.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "Character/Components/GGYGOHeroComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCharacterBase)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOCharacterBase, Log, All);

// Only this record owns the original source's returned subscription token.
struct AGGYGOCharacterBase::FHealthAbilitySystemSubscription
{
	FHealthAbilitySystemSubscription(AGGYGOCharacterBase* InCharacter,
		UGGYGOPawnExtensionComponent* InExtension, UGGYGOHealthComponent* InHealth)
		: Character(InCharacter), Extension(InExtension), Health(InHealth)
	{
	}

	~FHealthAbilitySystemSubscription() { Retire(); }
	FHealthAbilitySystemSubscription(const FHealthAbilitySystemSubscription&) = delete;
	FHealthAbilitySystemSubscription& operator=(const FHealthAbilitySystemSubscription&) = delete;

	void AcceptReturnedHandle(FDelegateHandle ReturnedHandle)
	{
		check(IsInGameThread());
		check(!NoticeHandle.IsValid());
		if (bRetired)
		{
			// RegisterAndCall can retire this record before its actual token returns.
			if (ReturnedHandle.IsValid())
			{
				if (UGGYGOPawnExtensionComponent* OriginalExtension = Extension.Get())
				{
					OriginalExtension->UnregisterLocalAbilitySystemNotice(ReturnedHandle);
				}
			}
			return;
		}
		NoticeHandle = ReturnedHandle;
	}

	void Retire()
	{
		bRetired = true;
		const FDelegateHandle OriginalHandle = NoticeHandle;
		NoticeHandle.Reset();
		if (OriginalHandle.IsValid())
		{
			if (UGGYGOPawnExtensionComponent* OriginalExtension = Extension.Get())
			{
				OriginalExtension->UnregisterLocalAbilitySystemNotice(OriginalHandle);
			}
		}
	}

	const TWeakObjectPtr<AGGYGOCharacterBase> Character;
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> Extension;
	const TWeakObjectPtr<UGGYGOHealthComponent> Health;
	FDelegateHandle NoticeHandle{};
	bool bRetired = false;
};

AGGYGOCharacterBase::AGGYGOCharacterBase(const FObjectInitializer& ObjectInitializer)
	// 把 ACharacter 自带的 CMC 换成项目 CMC。
	//
	// 必须用 SetDefaultSubobjectClass 而不是 CreateDefaultSubobject 再挂一个：
	// ACharacter 内部有大量代码直接引用 CharacterMovement 成员（跳跃、蹲伏、
	// RootMotion、网络同步），额外挂一个组件不会被那些代码使用，
	// 结果是两个 CMC 同时存在而只有引擎那个真正生效。
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UGGYGOCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	// 本类自己不 Tick。每帧逻辑属于各组件与 CMC，
	// 由 Actor 统一 Tick 再分发会让执行顺序变成隐式约定。
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// ===== 协调者 =====
	PawnExtComponent = CreateDefaultSubobject<UGGYGOPawnExtensionComponent>(TEXT("PawnExtensionComponent"));

	// ===== 生命 =====
	HealthComponent = CreateDefaultSubobject<UGGYGOHealthComponent>(TEXT("HealthComponent"));
	HealthComponent->OnDeathStarted.AddDynamic(this, &ThisClass::OnDeathStarted);
	HealthComponent->OnDeathFinished.AddDynamic(this, &ThisClass::OnDeathFinished);
}

UAbilitySystemComponent* AGGYGOCharacterBase::GetAbilitySystemComponent() const
{
	return GetGGYGOAbilitySystemComponent();
}

UGGYGOAbilitySystemComponent* AGGYGOCharacterBase::GetGGYGOAbilitySystemComponent() const
{
	// ASC 不属于本角色，而属于与战斗状态同生命周期的外部宿主。
	// 本角色只是那个 ASC 的 Avatar，通过协调者拿到被注入的实例。
	//
	// **可能返回 nullptr**：从 Pawn 生成到队伍位置注入 ASC 之间存在一个窗口。
	// 调用方必须判空，不能沿用"角色一定有 ASC"的旧假设。
	return PawnExtComponent ? PawnExtComponent->GetGGYGOAbilitySystemComponent() : nullptr;
}

UGGYGOCharacterMovementComponent* AGGYGOCharacterBase::GetGGYGOMovementComponent() const
{
	// CastChecked 是安全的：构造函数已经用 SetDefaultSubobjectClass 保证了类型。
	// 若这里真的失败，说明有子类又把 CMC 类型换回去了，那是应当立刻暴露的配置错误。
	return CastChecked<UGGYGOCharacterMovementComponent>(GetCharacterMovement());
}

void AGGYGOCharacterBase::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// 这里**不绑定 ASC**。外部 CombatantState 在生成本角色后通过
	// `AttachAvatar` 统一注入，Character 不知道宿主是玩家 Slot 还是 BossState。
	//
	// 之所以不能在这里自己建一个再绑：那样属性集只能跟着 Pawn 的初始化流程走，
	// 而本角色的 HealthComponent 也在同一段流程里初始化，两者先后无法保证。
	// 属性集现在是 Slot 的默认子对象，Slot 一存在就绪，早于本角色。
	// 组件实际注册后建立完整订阅；迟到时只回放 Extension 认证的真实 Ready。
	FString Error;
	if (!RegisterHealthAbilitySystemSubscription(Error))
	{
		UE_LOG(LogGGYGOCharacterBase, Warning, TEXT("%s"), *Error);
	}
}

void AGGYGOCharacterBase::BeginPlay()
{
	const TWeakObjectPtr<AGGYGOCharacterBase> OriginalCharacter(this);
	const bool bReopenSubscription = bHealthAbilitySystemSubscriptionClosed
		&& !HasActorBegunPlay() && !IsActorBeingDestroyed();
	Super::BeginPlay();
	AGGYGOCharacterBase* Character = OriginalCharacter.Get();
	if (bReopenSubscription && Character && !Character->IsActorBeingDestroyed()
		&& Character->HasActorBegunPlay() && Character->bHealthAbilitySystemSubscriptionClosed)
	{
		// 真正的新 Actor 生命周期在所有组件 BeginPlay 完成后重新注册一次。
		Character->bHealthAbilitySystemSubscriptionClosed = false;
		FString Error;
		if (!Character->RegisterHealthAbilitySystemSubscription(Error))
		{
			UE_LOG(LogGGYGOCharacterBase, Warning, TEXT("%s"), *Error);
		}
	}
}

void AGGYGOCharacterBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bHealthAbilitySystemSubscriptionClosed = true;
	UnregisterHealthAbilitySystemSubscription();
	// Health 自身 EndPlay／OnUnregister 归还五个原 token；Base 只退休通知订阅。
	Super::EndPlay(EndPlayReason);
}

void AGGYGOCharacterBase::BeginDestroy()
{
	bHealthAbilitySystemSubscriptionClosed = true;
	UnregisterHealthAbilitySystemSubscription();
	Super::BeginDestroy();
}

void AGGYGOCharacterBase::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// ASC 的 ActorInfo 缓存了 Controller，附身后必须刷新，
	// 否则能力里 GetOwningActorFromActorInfo / 目标选择拿到的还是旧 Controller。
	if (PawnExtComponent)
	{
		PawnExtComponent->HandleControllerChanged();
	}
}

void AGGYGOCharacterBase::UnPossessed()
{
	Super::UnPossessed();

	if (PawnExtComponent)
	{
		PawnExtComponent->HandleControllerChanged();
	}
}

void AGGYGOCharacterBase::OnRep_Controller()
{
	Super::OnRep_Controller();

	// 客户端路径。本地控制端的 InitState 在此才能推进到 DataAvailable。
	if (PawnExtComponent)
	{
		PawnExtComponent->HandleControllerChanged();
	}
}

void AGGYGOCharacterBase::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();

	if (PawnExtComponent)
	{
		PawnExtComponent->HandlePlayerStateReplicated();
	}
}

void AGGYGOCharacterBase::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// 通知协调者"输入组件已就绪"，让依赖它的 feature 能推进。
	if (PawnExtComponent)
	{
		PawnExtComponent->SetupPlayerInputComponent();
	}

	// HeroComponent 只在被玩家操控的单位上存在，所以要判空而不是断言。
	// 这里必须再调一次绑定：InitState 推进到 DataInitialized 的时机可能早于
	// 输入组件创建，那一次会因为 InputComponent 为空而跳过。
	if (UGGYGOHeroComponent* HeroComponent = UGGYGOHeroComponent::FindHeroComponent(this))
	{
		HeroComponent->InitializePlayerInput(PlayerInputComponent);
	}
}

void AGGYGOCharacterBase::FellOutOfWorld(const UDamageType& DmgType)
{
	// 不用引擎默认的 Destroy()，改走自毁伤害链路。
	// 直接销毁会跳过死亡流程，于是死亡计数、掉落、复活这些逻辑对"掉出世界"全都不生效，
	// 需要在每个地方补一个特例判断。
	if (HealthComponent)
	{
		HealthComponent->DamageSelfDestruct(/*bFellOutOfWorld=*/true);
	}
	else
	{
		Super::FellOutOfWorld(DmgType);
	}
}

bool AGGYGOCharacterBase::RegisterHealthAbilitySystemSubscription(FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	const TWeakObjectPtr<AGGYGOCharacterBase> OriginalCharacter(this);
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(PawnExtComponent.Get());
	const TWeakObjectPtr<UGGYGOHealthComponent> OriginalHealth(HealthComponent.Get());
	const FString DiagnosticContext = FString::Printf(
		TEXT("[Character/Base] Character='%s' Extension='%s' Health='%s'"),
		*GetPathNameSafe(this), *GetPathNameSafe(OriginalExtension.Get()), *GetPathNameSafe(OriginalHealth.Get()));
	const auto Reject = [&OutError, &DiagnosticContext](const TCHAR* Reason)
	{
		OutError = FString::Printf(TEXT("%s Reason='%s'."), *DiagnosticContext, Reason);
		return false;
	};
	const auto IsOriginalTargetLive = [&]()
	{
		const AGGYGOCharacterBase* Character = OriginalCharacter.Get();
		const UGGYGOPawnExtensionComponent* Extension = OriginalExtension.Get();
		const UGGYGOHealthComponent* Health = OriginalHealth.Get();
		return Character && !Character->bHealthAbilitySystemSubscriptionClosed
			&& !Character->IsActorBeingDestroyed()
			&& !Character->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& Extension && Extension->IsRegistered() && !Extension->IsBeingDestroyed()
			&& !Extension->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& Health && Health->IsRegistered() && !Health->IsBeingDestroyed()
			&& !Health->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& Character->PawnExtComponent.Get() == Extension && Extension->GetOwner() == Character
			&& Character->HealthComponent.Get() == Health && Health->GetOwner() == Character;
	};
	if (!IsOriginalTargetLive())
	{
		return Reject(TEXT("LifecycleClosedOrOriginalComponentsNotRegistered"));
	}
	if (HealthAbilitySystemSubscription.IsValid())
	{
		const TSharedPtr<FHealthAbilitySystemSubscription> Existing = HealthAbilitySystemSubscription;
		if (!Existing->bRetired && Existing->Character.HasSameIndexAndSerialNumber(OriginalCharacter)
			&& Existing->Extension.HasSameIndexAndSerialNumber(OriginalExtension)
			&& Existing->Health.HasSameIndexAndSerialNumber(OriginalHealth))
		{
			return Existing->NoticeHandle.IsValid()
				? true : Reject(TEXT("OriginalSubscriptionTokenHasNotReturned"));
		}
		// No implicit source replacement or cleanup of another lifecycle.
		return Reject(TEXT("OriginalSubscriptionConflict"));
	}
	const TSharedPtr<FHealthAbilitySystemSubscription> OriginalSubscription =
		MakeShared<FHealthAbilitySystemSubscription>(this, OriginalExtension.Get(), OriginalHealth.Get());
	HealthAbilitySystemSubscription = OriginalSubscription;
	const TWeakPtr<FHealthAbilitySystemSubscription> WeakSubscription(OriginalSubscription);
	const FDelegateHandle ReturnedHandle = OriginalExtension.Get()->RegisterLocalAbilitySystemNoticeAndCall(
		FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateWeakLambda(this,
			[WeakSubscription](const FGGYGOPawnASCLocalNotice& Notice)
			{
				const TSharedPtr<FHealthAbilitySystemSubscription> Subscription = WeakSubscription.Pin();
				if (Subscription.IsValid())
				{
					if (AGGYGOCharacterBase* Character = Subscription->Character.Get())
					{
						Character->ConsumeLocalAbilitySystemNotice(Subscription, Notice);
					}
				}
			}));
	OriginalSubscription->AcceptReturnedHandle(ReturnedHandle);
	AGGYGOCharacterBase* Character = OriginalCharacter.Get();
	if (!ReturnedHandle.IsValid() || !IsOriginalTargetLive() || !Character
		|| Character->HealthAbilitySystemSubscription != OriginalSubscription || OriginalSubscription->bRetired)
	{
		if (Character && Character->HealthAbilitySystemSubscription == OriginalSubscription)
		{
			Character->UnregisterHealthAbilitySystemSubscription();
		}
		else
		{
			OriginalSubscription->Retire();
		}
		return Reject(ReturnedHandle.IsValid()
			? TEXT("OriginalSubscriptionInvalidatedDuringReplay")
			: TEXT("ExtensionRejectedSubscription"));
	}
	// The token proves only registration history; Health operation results are diagnosed at entry.
	return true;
}

void AGGYGOCharacterBase::UnregisterHealthAbilitySystemSubscription()
{
	check(IsInGameThread());
	const TSharedPtr<FHealthAbilitySystemSubscription> OriginalSubscription = HealthAbilitySystemSubscription;
	if (!OriginalSubscription.IsValid()) { return; }
	OriginalSubscription->bRetired = true;
	HealthAbilitySystemSubscription.Reset();
	OriginalSubscription->Retire();
	// No Actor or Health writes after removing the original source's token.
}

void AGGYGOCharacterBase::ConsumeLocalAbilitySystemNotice(
	const TSharedPtr<FHealthAbilitySystemSubscription>& ExpectedSubscription,
	const FGGYGOPawnASCLocalNotice& Notice)
{
	check(IsInGameThread());
	const TSharedPtr<FHealthAbilitySystemSubscription> OriginalSubscription = ExpectedSubscription;
	const FGGYGOPawnASCLocalNotice OwnNotice = Notice;
	if (!OriginalSubscription.IsValid() || OriginalSubscription->bRetired
		|| HealthAbilitySystemSubscription != OriginalSubscription)
	{
		return;
	}
	const auto Diagnose = [&](const FString& Reason)
	{
		const FGGYGOPawnASCResourceIdentity Identity = OwnNotice.Resource.GetIdentity();
		UE_LOG(LogGGYGOCharacterBase, Warning,
			TEXT("[Character/Base] Character='%s' Extension='%s' Health='%s' ASC='%s' Pawn='%s' Kind=%u Binding=%llu Write=%llu Reason='%s'."),
			*GetPathNameSafe(OriginalSubscription->Character.Get()),
			*GetPathNameSafe(OriginalSubscription->Extension.Get()),
			*GetPathNameSafe(OriginalSubscription->Health.Get()),
			*GetPathNameSafe(Identity.ASC.Get()), *GetPathNameSafe(Identity.Pawn.Get()),
			static_cast<uint32>(OwnNotice.Kind), static_cast<unsigned long long>(Identity.Binding.Serial),
			static_cast<unsigned long long>(OwnNotice.PublishedContext.LastActorInfoWrite.Serial), *Reason);
	};
	AGGYGOCharacterBase* Character = OriginalSubscription->Character.Get();
	UGGYGOPawnExtensionComponent* Extension = OriginalSubscription->Extension.Get();
	UGGYGOHealthComponent* Health = OriginalSubscription->Health.Get();
	if (Character != this || bHealthAbilitySystemSubscriptionClosed || IsActorBeingDestroyed()
		|| HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !Extension || Extension->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !Health || Health->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| PawnExtComponent.Get() != Extension || Extension->GetOwner() != Character
		|| HealthComponent.Get() != Health || Health->GetOwner() != Character)
	{
		Diagnose(TEXT("OriginalSubscriptionOrObjectsClosed"));
		return;
	}
	if (!OwnNotice.Resource.HasResource())
	{
		Diagnose(TEXT("NoticeMissingOriginalResource"));
		return;
	}
	FString Error;
	bool bSucceeded = false;
	switch (OwnNotice.Kind)
	{
	case EGGYGOPawnASCLocalNoticeKind::Ready:
		bSucceeded = Health->InitializeWithLocalAbilitySystemResource(
			Extension, OwnNotice.Resource, OwnNotice.PublishedContext, Error);
		break;
	case EGGYGOPawnASCLocalNoticeKind::Refreshed:
		bSucceeded = Health->RefreshLocalAbilitySystemResource(
			Extension, OwnNotice.Resource, OwnNotice.PublishedContext, Error);
		break;
	case EGGYGOPawnASCLocalNoticeKind::Released:
		// Withdrawal has already invalidated Ready; only the delivered historical H is released.
		bSucceeded = Health->UninitializeFromLocalAbilitySystemResource(OwnNotice.Resource, Error);
		break;
	default:
		Diagnose(TEXT("InvalidLocalNoticeKind"));
		return;
	}
	if (!bSucceeded)
	{
		Diagnose(Error);
	}
	// A callback can synchronously install/refresh a successor. This tail performs no cleanup or adoption.
}

void AGGYGOCharacterBase::OnDeathStarted(AActor* OwningActor)
{
	DisableMovementAndCollision();
}

void AGGYGOCharacterBase::OnDeathFinished(AActor* OwningActor)
{
	// 下一帧再清理，不在本帧立刻做。
	// 本函数是从 HealthComponent 的委托广播里被调用的，
	// 此刻同一个广播的其它订阅者还没执行完，立即销毁会让它们访问已失效的对象。
	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &ThisClass::UninitAndDestroy);
}

void AGGYGOCharacterBase::DisableMovementAndCollision()
{
	if (Controller)
	{
		// 断开控制但不销毁 Controller —— 死亡镜头仍需要它，复活也要复用。
		Controller->SetIgnoreMoveInput(true);
	}

	if (UCapsuleComponent* CapsuleComp = GetCapsuleComponent())
	{
		check(CapsuleComp);
		// 关碰撞查询而不是整个 Destroy：尸体仍需要被追踪到（拾取判定、镜头避让）。
		CapsuleComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		CapsuleComp->SetCollisionResponseToAllChannels(ECR_Ignore);
	}

	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		// None 而不是 Falling：死亡后不该继续受重力驱动位移，
		// 倒地表现应由动画或物理资产接管。
		MoveComp->StopMovementImmediately();
		MoveComp->DisableMovement();
	}
}

void AGGYGOCharacterBase::UninitAndDestroy()
{
	if (GetLocalRole() == ROLE_Authority)
	{
		// 只有服务器销毁。客户端的副本靠销毁复制自然消失，
		// 本地提前销毁会让服务器后续的复制找不到目标。
		DetachFromControllerPendingDestroy();
		SetLifeSpan(0.1f);
	}

	// ASC 的 Avatar 立刻解除，不等销毁 —— 期间可能有 GE 到达，
	// 作用在一个正在销毁的 Avatar 上会产生半应用状态。
	if (UGGYGOAbilitySystemComponent* GGYGOASC = GetGGYGOAbilitySystemComponent())
	{
		if (GGYGOASC->GetAvatarActor() == this)
		{
			PawnExtComponent->UninitializeAbilitySystem();
		}
	}

	SetActorHiddenInGame(true);
}
