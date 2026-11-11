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
#include "Logging/LogMacros.h"
#include "Net/UnrealNetwork.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCharacterBase)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOCharacterDeath, Log, All);

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

void AGGYGOCharacterBase::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AGGYGOCharacterBase, bReplicatedActorEnableCollision);
}

void AGGYGOCharacterBase::SetReplicatedActorEnableCollision(bool bEnable)
{
	if (!ensureMsgf(HasAuthority(), TEXT("SetReplicatedActorEnableCollision [%s]: 只有权威端能设置，客户端以复制值为准。"),
		*GetPathName()))
	{
		return;
	}
	bReplicatedActorEnableCollision = bEnable;
	SetActorEnableCollision(bEnable);
}

void AGGYGOCharacterBase::OnRep_ReplicatedActorEnableCollision()
{
	SetActorEnableCollision(bReplicatedActorEnableCollision);
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

void AGGYGOCharacterBase::PostNetReceiveRole()
{
	Super::PostNetReceiveRole();

	// Controller 的 RepNotify 可能仍在后续；订阅者按当前状态重评。
	// 广播可重入或销毁本角色，之后不再访问成员。
	ReplicatedLocalRoleChanged.Broadcast(this);
}

void AGGYGOCharacterBase::OnRep_Controller()
{
	Super::OnRep_Controller();

	// 客户端：服务器解除附身只经复制到达，本地不调用 UnPossessed。与服务器 UnPossessed 一样归还本地输入：
	// 退役 Hero 输入会话、销毁玩家输入组件；之后再附身时 PawnClientRestart 才会重新建立输入与移动会话。
	if (!IsLocallyControlled() && InputComponent)
	{
		if (UGGYGOHeroComponent* Hero = UGGYGOHeroComponent::FindHeroComponent(this))
		{
			Hero->ReleasePlayerInput();
		}
		DestroyPlayerInputComponent();
	}

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

void AGGYGOCharacterBase::OnDeathStarted(AActor* OwningActor)
{
	DisableMovementAndCollision();
}

void AGGYGOCharacterBase::OnDeathFinished(AActor* OwningActor)
{
	// 下一帧再交给蓝图决定 Pawn 的去留。
	// 本函数是从 HealthComponent 的委托广播里被调用的，
	// 此刻同一个广播的其它订阅者还没执行完，立即销毁会让它们访问已失效的对象。
	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &ThisClass::HandleDeathFinishedNextTick);
}

bool AGGYGOCharacterBase::K2_HandleDeathFinished_Implementation()
{
	return false;
}

void AGGYGOCharacterBase::HandleDeathFinishedNextTick()
{
	// 蓝图可以销毁本 Pawn；返栈后只用调用前保存的路径诊断，不再读取角色资源。
	const FString CharacterPath = GetPathName();
	if (!K2_HandleDeathFinished())
	{
		UE_LOG(LogGGYGOCharacterDeath, Error,
			TEXT("[Character.Death] Pawn=%s: Handle Death Finished 未配置或未确认处理；请配置共享角色蓝图。"),
			*CharacterPath);
	}
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
