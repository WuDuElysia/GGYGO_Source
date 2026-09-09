/**
 * @file GGYGOCharacterBase.cpp
 * @brief 新版角色基类实现
 */
#include "Character/GGYGOCharacterBase.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCharacterBase)

AGGYGOCharacterBase::AGGYGOCharacterBase(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 本类自己不 Tick。每帧逻辑属于各组件与 CMC，
	// 由 Actor 统一 Tick 再分发会让执行顺序变成隐式约定。
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// ===== ASC =====
	// 用 CreateDefaultSubobject 而非运行时 NewObject：
	// ASC 的 InitAbilityActorInfo 会扫描 Actor 子对象自动发现 AttributeSet，
	// 运行时创建的对象扫不到。
	AbilitySystemComponent = CreateDefaultSubobject<UGGYGOAbilitySystemComponent>(TEXT("AbilitySystemComponent"));

	// Mixed：队伍角色是玩家自己的单位，需要完整的 GE 与属性信息给 UI，
	// 但对其他玩家只需要最小集。敌人 AI 在各自子类里改为 Minimal。
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

	// ===== 协调者 =====
	PawnExtComponent = CreateDefaultSubobject<UGGYGOPawnExtensionComponent>(TEXT("PawnExtensionComponent"));

	// 用 RegisterAndCall 而不是普通 Add：本构造函数执行时 ASC 还没 InitAbilityActorInfo，
	// 但初始化顺序在联机下不固定，用带补发的版本可以避免漏掉已发生的广播。
	PawnExtComponent->OnAbilitySystemInitialized_RegisterAndCall(
		FSimpleMulticastDelegate::FDelegate::CreateUObject(this, &ThisClass::OnAbilitySystemInitialized));
	PawnExtComponent->OnAbilitySystemUninitialized_Register(
		FSimpleMulticastDelegate::FDelegate::CreateUObject(this, &ThisClass::OnAbilitySystemUninitialized));

	// ===== 生命 =====
	HealthComponent = CreateDefaultSubobject<UGGYGOHealthComponent>(TEXT("HealthComponent"));
	HealthComponent->OnDeathStarted.AddDynamic(this, &ThisClass::OnDeathStarted);
	HealthComponent->OnDeathFinished.AddDynamic(this, &ThisClass::OnDeathFinished);
}

UAbilitySystemComponent* AGGYGOCharacterBase::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void AGGYGOCharacterBase::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// 在这里初始化而不是 BeginPlay：所有组件此时都已注册，
	// 而 BeginPlay 时 InitState 已经开始推进，ASC 还没就位会让 DataAvailable 卡住。
	//
	// OwnerActor 和 AvatarActor 都传 this —— ASC 归角色自己（决策 D1）。
	// 队伍级 ASC 走的是另一条路（挂 PlayerState，阶段 10）。
	if (AbilitySystemComponent && PawnExtComponent)
	{
		PawnExtComponent->InitializeAbilitySystem(AbilitySystemComponent, this);
	}
}

void AGGYGOCharacterBase::BeginPlay()
{
	Super::BeginPlay();
}

void AGGYGOCharacterBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 组件的 EndPlay 会各自清理，这里不重复调用 UninitializeAbilitySystem。
	Super::EndPlay(EndPlayReason);
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

	// 实际的输入绑定不在这里做，将由 HeroComponent 承担（阶段 7）。
	// 本类只通知协调者"输入组件已就绪"，让依赖它的 feature 能推进。
	if (PawnExtComponent)
	{
		PawnExtComponent->SetupPlayerInputComponent();
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

void AGGYGOCharacterBase::OnAbilitySystemInitialized()
{
	UGGYGOAbilitySystemComponent* GGYGOASC = GetGGYGOAbilitySystemComponent();
	check(GGYGOASC);

	// HealthComponent 在这里而不是自己的 BeginPlay 里初始化：
	// 它需要从 ASC 上取 HealthSet，而 AttributeSet 由 AbilitySet 在
	// InitState 的 DataInitialized 阶段授予，时机不由 HealthComponent 自己决定。
	if (HealthComponent)
	{
		HealthComponent->InitializeWithAbilitySystem(GGYGOASC);
	}
}

void AGGYGOCharacterBase::OnAbilitySystemUninitialized()
{
	if (HealthComponent)
	{
		HealthComponent->UninitializeFromAbilitySystem();
	}
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
