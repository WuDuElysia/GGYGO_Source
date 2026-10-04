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
#include "UObject/WeakObjectPtrTemplates.h"

#include "GGYGOSquadComponent.generated.h"

namespace EEndPlayReason { enum Type : int; }

class APawn;
class APlayerController;
class AGGYGOCharacterBase;
class AGGYGOCharacterSlot;
class AGGYGOGameMode;
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
	 * 按原始名单数量校验容量，超过 `GGYGO_MAX_SQUAD_SIZE` 时整体拒绝，不截断。
	 * 名单里的空项被跳过；验证后至少有一名成员，才一次替换原名单。
	 *
	 * @return 是否被接受。已装配、非服务器、原始数量超限、空名单或全空项返回 false，原名单不变。
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
	 * 借用登记一个已初始化且 Avatar 绑定一致的队伍位置。仅服务器有效。
	 *
	 * 当前装配流程先生成并初始化 Slot，再生成 Pawn、设置 PawnData 和绑定 Avatar，
	 * 最后登记。此入口只接收成员关系，不取得 Slot 或 Pawn 的创建清理责任。
	 * 新成员默认待命；没有出战位时尝试激活第一个位置。
	 *
	 * 超过 `GGYGO_MAX_SQUAD_SIZE` 的登记会被拒绝 —— 每个位置带一个 ASC，
	 * 配置写错的代价是成倍的复制开销，宁可在装配阶段就拒绝。
	 * 已登记的同一位置返回 true，不重复激活，也不改变既有创建责任。
	 *
	 * @return 成员关系是否已被接收；不保证自动激活或 Possess 成功。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool RegisterSlot(AGGYGOCharacterSlot* Slot);

	/**
	 * 终止本组件的成员关系，并请求销毁显式接收的原创建 Actor。
	 * 借用 Actor 不销毁；原生接受只代表移交销毁生命周期，不保证物理清理已完成。
	 * 重复入口不重放控制/通知；拒绝且仍存活的资源保留给后续明确入口处理。
	 */
	void DestroySquad();

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
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
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

private:
	friend class AGGYGOGameMode;

	/** 原始创建对象的清理责任；不代表当前 Avatar 或另一份成员名单。 */
	struct FCreatedSlotResources
	{
		TWeakObjectPtr<AGGYGOCharacterSlot> Slot;
		TWeakObjectPtr<APawn> OriginalCreatedPawn;
	};

	/**
	 * 接收 GameMode 实际创建的 Slot 与原始 Pawn；仅 C++ 创建者调用。
	 * 已借用登记的位置不能升级为自创；相同原始身份对重复请求幂等。
	 * 提交责任后返回 true，即使自动激活失败；GameMode 实际创建交付仍须独立接线。
	 */
	bool RegisterCreatedSlot(AGGYGOCharacterSlot* Slot, APawn* OriginalCreatedPawn);

	/** 两种入口共用接收事务；所有身份验证先于名单和责任提交。 */
	bool RegisterSlotInternal(AGGYGOCharacterSlot* Slot, APawn* OriginalCreatedPawn, bool bAcceptCreatedResources);

	/** 当前组件必须是权威 PlayerState 的唯一队伍，且 Controller/World 对应。 */
	APlayerController* GetRegistrationController() const;

	/** 只读验证新成员的初始化、连接归属和 Slot/ASC/PawnExtension 绑定。 */
	bool HasValidSlotBinding(const AGGYGOCharacterSlot* Slot, const APlayerController* OwningController) const;

	/** 外调返回后只核对原表现对象与本组件终止边界，不认证 ASC 绑定。 */
	bool CanContinueSlotPresentation(const TWeakObjectPtr<AGGYGOCharacterSlot>& OriginalSlot,
		const TWeakObjectPtr<AGGYGOCharacterBase>& OriginalCharacter) const;

	/** 三个实际终止入口共用；只消费本组件的原 Actor 责任，不编排绑定清理。 */
	void ConsumeCreatedSlotResources(bool bFinalComponentDestruction);

	/** 仅记录已明确交付且尚未移交原生销毁的资源；消费栈临时取得唯一责任，不复制。 */
	TArray<FCreatedSlotResources> CreatedSlotResources;

	/** 只关闭本实例准入；不充当第二个装配状态，也不随 BeginPlay 重开。 */
	bool bSquadTerminationStarted = false;
	bool bConsumingCreatedSlotResources = false;
	bool bFinalComponentDestructionRequested = false;
};
