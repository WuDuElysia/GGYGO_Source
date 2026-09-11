/**
 * @file GGYGOCharacterSlot.h
 * @brief 队伍中一个位置的角色数据宿主
 *
 * 一支队伍由 N 个位置组成，每个位置由本类承载。它持有该位置角色的 ASC 与属性集，
 * 而角色的实体（Mesh、移动、动画）在 Pawn 上，Pawn 作为本 ASC 的 Avatar。
 *
 * ## 为什么 ASC 挂这里而不挂 Pawn（决策 D1）
 * ASC 的宿主决定了属性集何时就绪。挂 Pawn 时属性集只能在 Pawn 的初始化流程里授予，
 * 而 Pawn 上想读属性的组件（`UGGYGOHealthComponent`）也在同一段流程里初始化 ——
 * 两者的先后变成必须靠约定维护的东西，错了的症状是"属性集找不到"，
 * 而根因在几百行之外的初始化链上。
 *
 * 属性集是本类的默认子对象，`UAbilitySystemComponent::InitializeComponent` 会自动发现它们。
 * 那发生在组件注册阶段，早于任何 Pawn 存在，所以顺序问题在结构上不可能发生。
 *
 * 与 Lyra 的关系：Lyra 有两套布局，`ALyraCharacter` 把 ASC 放 PlayerState（靠
 * "PlayerState 先于 Pawn"保证时序），`ALyraCharacterWithAbilities` 把 ASC 放 Pawn 自己
 * 并**把属性集做成构造函数子对象**。本类取后者的属性集做法，但宿主既不是 PlayerState
 * 也不是 Pawn —— 因为一名玩家带多个角色，PlayerState 上一个 ASC 会让三角色共享血量，
 * 而挂 Pawn 又会把角色数据的生命周期绑死在实体上。
 *
 * ## 生命周期
 * 由 `AGGYGOGameMode` 在装配队伍时生成，早于 Pawn。Pawn 可以后续生成、销毁、重生，
 * 本位置的属性、冷却、Buff 都不受影响 —— 这是队伍制"下场角色继续走冷却"的实现基础。
 *
 * ## Owner 必须指向 PlayerController
 * GAS 的客户端预测依赖 `ASC->GetOwnerActor()->GetNetOwningPlayer()` 能追到玩家连接。
 * 生成本 Actor 时必须 `SetOwner(PlayerController)`，否则 PredictionKey 生成不出来，
 * 所有 `LocalPredicted` 能力静默退化为纯服务器执行，表现为输入延迟一个 RTT 且**不报错**。
 */
#pragma once

#include "AbilitySystemInterface.h"
#include "GameFramework/Info.h"

#include "GGYGOCharacterSlot.generated.h"

class APawn;
class UAbilitySystemComponent;
class UGGYGOAbilitySystemComponent;
class UGGYGOCombatSet;
class UGGYGOHealthSet;
class UGGYGOPawnData;
class UObject;

/**
 * 队伍中的一个角色位置。
 *
 * 派生自 `AInfo`：本类没有空间存在感，不需要 Transform 复制、碰撞或 Tick。
 */
UCLASS(meta = (ShortTooltip = "队伍中一个位置的角色数据宿主"))
class GGYGO_API AGGYGOCharacterSlot : public AInfo, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AGGYGOCharacterSlot(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	//~IAbilitySystemInterface
	/** 返回本位置的 ASC。这是该角色所有 GAS 交互的入口。 */
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	//~End of IAbilitySystemInterface

	/** 类型化访问器，省掉调用方的 Cast。构造完成后始终非空。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponent() const { return AbilitySystemComponent; }

	/**
	 * 确定本位置装哪个角色，并授予该角色的能力。仅服务器。
	 *
	 * 只能调用一次。重复设置意味着"同一个位置换了角色"，那应当新建位置而不是改这个 ——
	 * 已授予的能力与已生效的 Buff 无法干净地对应到另一份 PawnData。
	 */
	void InitializeForPawnData(const UGGYGOPawnData* InPawnData);

	/** 本位置装的角色定义。未初始化时为 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	const UGGYGOPawnData* GetPawnData() const { return PawnData; }

	/**
	 * 设置本位置 ASC 的 Avatar。仅服务器；客户端通过复制回调走同样的流程。
	 *
	 * 这是全项目唯一调用 `InitAbilityActorInfo` 的地方。集中在这里是因为
	 * "Owner 恒为本 Slot、Avatar 随 Pawn 变"这条规则一旦有第二个写入点，
	 * 就会出现两处对 Avatar 是谁的判断不一致。
	 *
	 * 传 nullptr 表示本位置当前没有实体（尚未生成或已销毁）。此时纯数值 GE
	 * 继续正常结算，但依赖 Avatar 的能力无法激活。
	 */
	void SetAvatar(APawn* NewAvatar);

	/** 当前承载本位置的 Pawn。没有实体时为 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	APawn* GetAvatarPawn() const { return AvatarPawn; }

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 绑定 ASC 的 Owner 为本 Slot。此时 Avatar 通常尚不存在。 */
	virtual void PostInitializeComponents() override;

	/** 客户端收到 Avatar 变更：重新绑定 ActorInfo，否则客户端侧的 Avatar 仍是旧的。 */
	UFUNCTION()
	void OnRep_AvatarPawn();

	/** 本位置的 ASC。Owner 恒为本 Slot，Avatar 为当前 Pawn。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Squad", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOAbilitySystemComponent> AbilitySystemComponent;

	/**
	 * 生命与韧性。
	 *
	 * 作为默认子对象持有而不是靠 AbilitySet 授予：`InitializeComponent` 阶段
	 * 就被 ASC 发现，任何时刻读它都不会为空。持一份引用同时也是防 GC。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Squad", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOHealthSet> HealthSet;

	/** 输出侧属性（攻击力、削韧）。持有理由同 `HealthSet`。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Squad", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOCombatSet> CombatSet;

	/** 本位置装的角色定义。复制给客户端用于 UI 与表现。 */
	UPROPERTY(Replicated)
	TObjectPtr<const UGGYGOPawnData> PawnData;

	/** 当前 Avatar。复制以便客户端也能把 ActorInfo 指向正确的 Pawn。 */
	UPROPERTY(ReplicatedUsing = OnRep_AvatarPawn)
	TObjectPtr<APawn> AvatarPawn;

	/** 能力是否已授予。防止 `InitializeForPawnData` 被重复调用时重复授予。 */
	bool bAbilitiesGranted = false;
};
