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
class UGGYGOPawnData;
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
	 * 设置本局的编队名单。仅服务器有效，且**必须在装配之前**调用。
	 *
	 * 名单决定这一局有哪几个角色、按什么顺序排列。装配（生成位置与实体）
	 * 之后不再接受修改 —— 那属于"局内换人"，与本项目的编成语义不同：
	 * 已授予的能力与已生效的 Buff 无法干净地对应到另一份 PawnData，
	 * 而位置上的属性集是默认子对象，换角色时数值会残留。
	 * 要换编队就在下一局开始前换。
	 *
	 * 超出 `GGYGO_MAX_SQUAD_SIZE` 的部分会被截断并报错。名单里的空项被跳过。
	 *
	 * @return 是否被接受。已装配、非服务器、名单为空都返回 false。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool SetRoster(const TArray<UGGYGOPawnData*>& InRoster);

	/** 本局的编队名单。未设置时为空，此时装配方应回落到玩法配置里的默认编队。 */
	const TArray<TObjectPtr<const UGGYGOPawnData>>& GetRoster() const { return Roster; }

	/**
	 * 是否已经装配过。
	 *
	 * 判据是"有没有位置"而不是单独的标记位：位置数组是复制属性，
	 * 客户端据此也能得到一致答案，多一个标记位就多一处可能与它不同步的状态。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	bool IsSquadAssembled() const { return Slots.Num() > 0; }

	/**
	 * 登记一个队伍位置。仅服务器有效。
	 *
	 * 由装配流程调用：先为名单里的每份 PawnData 生成一个位置，再逐个登记，
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
	 * 本局的编队名单：这一局带哪几个角色，按什么顺序。
	 *
	 * 与 `Slots` 的区别是**名单是意图，位置是结果**。名单先于装配存在，
	 * 由局外的编成流程（菜单选择、存档读取）填入；位置是照名单造出来的运行时对象。
	 *
	 * 分开两者而不是直接改 `Slots`，是因为"玩家想带谁"这件事在没有关卡、
	 * 没有 GameMode 的时候就该能表达，而位置必须有世界才能生成。
	 *
	 * 复制给拥有者：UI 在装配完成前就要显示编队预览。
	 */
	UPROPERTY(Replicated)
	TArray<TObjectPtr<const UGGYGOPawnData>> Roster;

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
