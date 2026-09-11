/**
 * @file GGYGOCharacterSlot.cpp
 * @brief 队伍位置的角色数据宿主实现
 */
#include "Teams/GGYGOCharacterSlot.h"

#include "AbilitySystem/GGYGOAbilitySet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Attributes/GGYGOCombatSet.h"
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "Character/Data/GGYGOPawnData.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCharacterSlot)

AGGYGOCharacterSlot::AGGYGOCharacterSlot(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 纯数据宿主：不 Tick。GE 的时间推进由 ASC 自己管，不需要本 Actor 参与。
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	bReplicates = true;

	// bAlwaysRelevant 而不是 bOnlyRelevantToOwner：
	// 其他客户端需要收到本角色的 GameplayCue 与 GameplayTag（打击特效、状态表现）。
	// 限制为仅所有者相关会让整个 ASC 对旁观者不可见，别人就看不到你的命中反馈。
	// "属性只给自己、GE 与 Cue 给所有人"这件事由下面的 Mixed 复制模式负责，
	// 不该用相关性来做 —— 相关性是全有或全无的。
	bAlwaysRelevant = true;

	// 位置没有空间语义，Transform 不需要复制。
	SetReplicatingMovement(false);

	// ASC 承载属性与 GE，变更频繁且直接影响表现，需要高更新频率。
	SetNetUpdateFrequency(100.0f);

	AbilitySystemComponent = CreateDefaultSubobject<UGGYGOAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);

	// Mixed：属性明细只发给拥有本队伍的客户端，GE 与 Cue 发给所有人。
	// 敌人 AI 不走本类（它们的 ASC 在自己身上），所以这里不需要 Minimal 分支。
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

	// 属性集作为默认子对象：`UAbilitySystemComponent::InitializeComponent` 会在
	// 组件注册阶段扫描 Owner 的子对象并自动登记它们。这比任何 InitState 阶段都早，
	// 也早于本位置的 Pawn 存在 —— 这正是 ASC 放在本类而非 Pawn 上的目的。
	HealthSet = CreateDefaultSubobject<UGGYGOHealthSet>(TEXT("HealthSet"));
	CombatSet = CreateDefaultSubobject<UGGYGOCombatSet>(TEXT("CombatSet"));
}

UAbilitySystemComponent* AGGYGOCharacterSlot::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void AGGYGOCharacterSlot::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AGGYGOCharacterSlot, PawnData);
	DOREPLIFETIME(AGGYGOCharacterSlot, AvatarPawn);
}

void AGGYGOCharacterSlot::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	check(AbilitySystemComponent);

	// Owner 恒为本 Slot；Avatar 此刻通常还不存在，由后续 SetAvatar 补上。
	// 即便 Avatar 为空也要先绑一次：这一步让 ASC 记住 OwnerActor，
	// 属性集的初始值与 GE 结算在没有实体的情况下同样需要它。
	AbilitySystemComponent->InitAbilityActorInfo(this, AvatarPawn);
}

void AGGYGOCharacterSlot::InitializeForPawnData(const UGGYGOPawnData* InPawnData)
{
	if (!HasAuthority())
	{
		return;
	}

	if (!InPawnData)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializeForPawnData: 位置 [%s] 收到空 PawnData，该位置不会有可用角色。"),
			*GetNameSafe(this));
		return;
	}

	if (PawnData)
	{
		// 换角色应当换位置，理由见头文件：已授予的能力与已生效的 Buff
		// 无法干净地对应到另一份 PawnData。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializeForPawnData: 位置 [%s] 已装载 [%s]，拒绝改为 [%s]。"),
			*GetNameSafe(this), *GetNameSafe(PawnData), *GetNameSafe(InPawnData));
		return;
	}

	PawnData = InPawnData;

	// 组仲裁与 Tag 关系表随角色定义走，注入时机不依赖 Pawn。
	AbilitySystemComponent->SetAbilityGroupConfig(PawnData->AbilityGroupConfig);
	AbilitySystemComponent->SetTagRelationshipMapping(PawnData->TagRelationshipMapping);

	if (bAbilitiesGranted)
	{
		return;
	}

	for (const TObjectPtr<UGGYGOAbilitySet>& AbilitySet : PawnData->AbilitySets)
	{
		if (AbilitySet)
		{
			// 不保存回收句柄：本位置的能力与本位置同生共死，Slot 销毁时 ASC 一起销毁。
			// SourceObject 传 PawnData，让能力能回溯自己的配置来源。
			AbilitySet->GiveToAbilitySystem(
				AbilitySystemComponent, nullptr, const_cast<UGGYGOPawnData*>(PawnData.Get()));
		}
	}

	bAbilitiesGranted = true;
}

void AGGYGOCharacterSlot::SetAvatar(APawn* NewAvatar)
{
	if (AvatarPawn == NewAvatar)
	{
		return;
	}

	AvatarPawn = NewAvatar;

	check(AbilitySystemComponent);
	AbilitySystemComponent->InitAbilityActorInfo(this, AvatarPawn);
}

void AGGYGOCharacterSlot::OnRep_AvatarPawn()
{
	// 客户端也必须重绑：ActorInfo 是本地状态，不随属性复制过来。
	// 漏掉这一步的症状是客户端上 Montage 与 Cue 找不到播放目标。
	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, AvatarPawn);
	}
}
