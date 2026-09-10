/**
 * @file GGYGOSquadComponent.h
 * @brief 一名玩家的队伍与出战角色切换
 *
 * 挂在 PlayerState 上，因为队伍是**玩家**的属性而不是角色的：
 * 出战角色会换，队伍成员名单不会随之改变。挂在角色上会让"我的队伍有谁"
 * 这个问题在换人瞬间失去答案。
 *
 * ## 换人的方式：换 Avatar，不换 Pawn
 * 三名成员各自是一个已生成的 Pawn，都持有自己的 ASC 与属性
 * （决策 D1：ASC 挂角色自己，因为一个 ASC 只能挂一套同类 AttributeSet，
 * 共用会让三角色共享生命值与冷却）。
 *
 * 换人时把 Controller 从当前 Pawn 转到目标 Pawn，非出战成员留在场上但
 * 隐藏且不参与碰撞。这样做而不是销毁重建，是因为后者会丢掉非出战成员的
 * 状态：冷却在走、Buff 在计时、血量要保留 —— 这些正是队伍制玩法的核心。
 *
 * ## 为什么不做成能力
 * 换人牵动 Controller 归属、相机、输入绑定这些跨 Actor 的关系，
 * 而能力的作用域是单个 ASC 的 Avatar。做成能力会让它在换人途中
 * 因为 Avatar 变更而被取消。
 */
#pragma once

#include "Components/GameFrameworkComponent.h"

#include "GGYGOSquadComponent.generated.h"

class APawn;
class APlayerController;
class AGGYGOCharacterBase;
class UObject;

/** 出战角色变更。@param NewCharacter 新的出战角色，可能为 nullptr。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FGGYGOActiveCharacterChanged, AGGYGOCharacterBase*, NewCharacter);

UCLASS(meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOSquadComponent : public UGameFrameworkComponent
{
	GENERATED_BODY()

public:
	UGGYGOSquadComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 取某个 Actor 上的本组件。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	static UGGYGOSquadComponent* FindSquadComponent(const AActor* Actor)
	{
		return Actor ? Actor->FindComponentByClass<UGGYGOSquadComponent>() : nullptr;
	}

	/**
	 * 登记一名队伍成员。仅服务器有效。
	 *
	 * 由生成流程调用：先按 PawnData 生成三个 Pawn，再逐个登记。
	 * 第一个登记的成员会自动成为出战角色。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	void RegisterMember(AGGYGOCharacterBase* Member);

	/** 当前出战角色。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	AGGYGOCharacterBase* GetActiveCharacter() const;

	/** 队伍成员数量。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	int32 GetMemberCount() const { return Members.Num(); }

	/**
	 * 切到指定序号的成员。仅服务器有效。
	 *
	 * @return 是否真的发生了切换。目标已出战、序号越界、目标已死亡都返回 false。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool SwitchToMember(int32 MemberIndex);

	/** 切到下一个存活成员。已死亡的成员会被跳过。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool SwitchToNextMember();

	/** 切到上一个存活成员。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool SwitchToPreviousMember();

	/** 出战角色变更时广播。UI 与相机据此更新。 */
	UPROPERTY(BlueprintAssignable, Category = "GGYGO|Squad")
	FGGYGOActiveCharacterChanged OnActiveCharacterChanged;

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 出战序号复制到达。客户端据此更新表现。 */
	UFUNCTION()
	void OnRep_ActiveMemberIndex();

	/** 把成员设为出战：接管 Controller、显示、开碰撞。 */
	void ActivateMember(AGGYGOCharacterBase* Member);

	/** 把成员设为待命：隐藏、关碰撞、停止移动。 */
	void DeactivateMember(AGGYGOCharacterBase* Member);

	/** 成员是否可以出战（存在且未死亡）。 */
	bool CanMemberBeActive(const AGGYGOCharacterBase* Member) const;

	/**
	 * 队伍成员。
	 *
	 * 复制给拥有者，UI 需要显示全队的血量与冷却。
	 * 用 TObjectPtr 持强引用：非出战成员不在任何 Controller 名下，
	 * 弱引用会让它们被 GC 回收。
	 */
	UPROPERTY(Replicated)
	TArray<TObjectPtr<AGGYGOCharacterBase>> Members;

	/**
	 * 当前出战成员的序号。
	 *
	 * 复制它而不是复制角色指针：序号在客户端与服务器一致，
	 * 而指针在成员数组尚未复制完成时可能指向 null。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_ActiveMemberIndex)
	int32 ActiveMemberIndex = INDEX_NONE;
};
