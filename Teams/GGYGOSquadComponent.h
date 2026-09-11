/**
 * @file GGYGOSquadComponent.h
 * @brief 一名玩家的队伍与出战角色切换
 *
 * 挂在 PlayerState 上，因为队伍是**玩家**的属性而不是角色的：
 * 出战角色会换，队伍成员名单不会随之改变。挂在角色上会让"我的队伍有谁"
 * 这个问题在换人瞬间失去答案。
 *
 * ## 名单里装的是位置，不是角色
 * 队伍由 N 个 `AGGYGOCharacterSlot` 组成，每个位置持有该角色的 ASC 与属性集
 * （决策 D1）。角色的实体是 Pawn，作为对应位置 ASC 的 Avatar。
 *
 * 这样"队伍成员"这件事就与"角色实体是否存在"解耦了：待命成员的冷却在走、
 * Buff 在计时、血量保留，这些状态都在位置上，与它的 Pawn 是否隐藏、
 * 甚至是否已生成都无关。
 *
 * ## 换人的方式：换 Controller 的附身目标
 * 换人时把 Controller 从当前 Pawn 转到目标 Pawn，非出战成员的 Pawn 留在场上但
 * 隐藏且不参与碰撞。不销毁重建 Pawn 的理由不再是"会丢状态"（状态在位置上，
 * 销毁 Pawn 不会丢），而是切人要即时响应，重新生成 Pawn 有可见延迟。
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
class AGGYGOCharacterSlot;
class UObject;

/** 出战角色变更。@param NewCharacter 新出战角色的 Pawn，可能为 nullptr。 */
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
	 * 登记一个队伍位置。仅服务器有效。
	 *
	 * 由装配流程调用：先为每份 PawnData 生成一个位置，再逐个登记，
	 * 之后才生成 Pawn。第一个登记的位置会自动成为出战位。
	 *
	 * 超过 `GGYGO_MAX_SQUAD_SIZE` 的登记会被拒绝 —— 每个位置带一个 ASC，
	 * 配置写错的代价是成倍的复制开销，宁可在装配阶段就拒绝。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	void RegisterSlot(AGGYGOCharacterSlot* Slot);

	/** 当前出战位置。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	AGGYGOCharacterSlot* GetActiveSlot() const;

	/** 指定序号的位置。越界返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	AGGYGOCharacterSlot* GetSlot(int32 SlotIndex) const;

	/** 当前出战角色的 Pawn。相机与 UI 用它。位置没有实体时为 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	AGGYGOCharacterBase* GetActiveCharacter() const;

	/** 队伍位置数量。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	int32 GetSlotCount() const { return Slots.Num(); }

	/**
	 * 切到指定序号的位置。仅服务器有效。
	 *
	 * @return 是否真的发生了切换。目标已出战、序号越界、目标已死亡或没有实体都返回 false。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool SwitchToSlot(int32 SlotIndex);

	/** 切到下一个可出战的位置。已死亡或无实体的位置会被跳过。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool SwitchToNextSlot();

	/** 切到上一个可出战的位置。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool SwitchToPreviousSlot();

	/** 出战角色变更时广播。UI 与相机据此更新。 */
	UPROPERTY(BlueprintAssignable, Category = "GGYGO|Squad")
	FGGYGOActiveCharacterChanged OnActiveCharacterChanged;

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 出战序号复制到达。客户端据此更新表现。 */
	UFUNCTION()
	void OnRep_ActiveSlotIndex();

	/** 把位置的 Pawn 设为出战：显示、开碰撞。 */
	void ActivateSlot(AGGYGOCharacterSlot* Slot);

	/** 把位置的 Pawn 设为待命：隐藏、关碰撞、停止移动。 */
	void DeactivateSlot(AGGYGOCharacterSlot* Slot);

	/** 位置是否可以出战（有实体且未死亡）。 */
	bool CanSlotBeActive(const AGGYGOCharacterSlot* Slot) const;

	/**
	 * 队伍位置。
	 *
	 * 复制给拥有者，UI 需要显示全队的血量与冷却。
	 * 用 TObjectPtr 持强引用：位置不在任何 Controller 名下，
	 * 弱引用会让它们被 GC 回收。
	 */
	UPROPERTY(Replicated)
	TArray<TObjectPtr<AGGYGOCharacterSlot>> Slots;

	/**
	 * 当前出战位置的序号。
	 *
	 * 复制它而不是复制位置指针：序号在客户端与服务器一致，
	 * 而指针在位置数组尚未复制完成时可能指向 null。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_ActiveSlotIndex)
	int32 ActiveSlotIndex = INDEX_NONE;
};
