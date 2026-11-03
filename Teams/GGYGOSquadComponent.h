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
 * 换人时把 Controller 从当前 Pawn 转到目标 Pawn。不销毁重建 Pawn 的理由不再是"会丢状态"
 * （状态在位置上，销毁 Pawn 不会丢），而是切人要即时响应，重新生成 Pawn 有可见延迟。
 *
 * ## 队员状态（仅服务器）
 * - Benched：隐藏、无碰撞、移动停用。
 * - Active：被玩家控制。切回仍在续放的队员时加 State.SwitchReturnPending，续放全部结束才解除。
 * - Lingering：换出时有能力选择 ContinueOnField，留在场上放完；可见、可动、不可受击（State.OffField）。
 * - Exiting：向 ASC 发 Event.Squad.SwitchOut，等被激活的退场能力结束后转 Benched。
 * 同一时刻一个 Active，任意数量 Lingering／Exiting；可以切回留场或退场中的队员。
 * 订阅各槽位 ASC 的 OnAbilityEnded 推进状态；不设计时器，也不复制状态本身——
 * 可见性、碰撞与移动由 Pawn 原生复制，OffField／ReturnPending 由复制的 loose tag 承载。
 *
 * ## 为什么不做成能力
 * 换人牵动 Controller 归属、相机、输入绑定这些跨 Actor 的关系，
 * 而能力的作用域是单个 ASC 的 Avatar。做成能力会让它在换人途中
 * 因为 Avatar 变更而被取消。
 */
#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "Components/GameFrameworkComponent.h"
#include "GameplayAbilitySpecHandle.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "GGYGOSquadComponent.generated.h"

namespace EEndPlayReason { enum Type : int; }

class APawn;
class APlayerController;
class AGGYGOCharacterBase;
class AGGYGOCharacterSlot;
class AGGYGOGameMode;
class UCharacterMovementComponent;
class UGGYGOAbilitySystemComponent;
class UGGYGOPawnData;
class UObject;
struct FAbilityEndedData;

/** 出战角色变更。@param NewCharacter 新出战角色的 Pawn，可能为 nullptr。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FGGYGOActiveCharacterChanged, AGGYGOCharacterBase*, NewCharacter);

/** 队员在编队中的场上状态。见文件头"队员状态"。 */
UENUM(BlueprintType)
enum class EGGYGOSquadMemberState : uint8
{
	Benched,
	Active,
	Lingering,
	Exiting
};

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
	 * 1. 旧队员的每个活动能力经 OnAvatarSwitchOut 决定取消或留场；任一 Cancel 决定的能力
	 *    不可取消时，在任何取消之前整次拒绝。
	 * 2. 留场集合登记后取消其余能力，补发按住输入的真实释放，通知留场能力失去控制，
	 *    退役 Hero 输入并解除附身。
	 * 3. 附身新队员（镜头随原生 ViewTarget 立即切换）。待命队员显示后发 SwitchIn；
	 *    留场／退场中的队员取消退场、解除 OffField，仍有续放时加 ReturnPending，不播进场。
	 * 4. 旧队员加 OffField：有续放进入 Lingering，否则直接退场。
	 * 不清除任何 Slot 的 GE、冷却或属性。外调改变原布局时停止并记录错误，已发生的效果不恢复。
	 * @return 是否完成切换。目标是当前出战、无 Avatar、已死亡、同步重入或附身失败均返回 false 并诊断。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool SwitchToSlot(int32 SlotIndex, EGGYGOSwitchSource Source = EGGYGOSwitchSource::Script);

	/**
	 * 客户端请求切换。服务器只校验序号范围，其余由 SwitchToSlot 校验与诊断。
	 * 在服务器上调用时直接执行。
	 */
	UFUNCTION(Server, Reliable, WithValidation, BlueprintCallable, Category = "GGYGO|Squad")
	void ServerRequestSwitchToSlot(int32 SlotIndex);

	/** 服务器上的队员状态；越界或客户端上返回 Benched。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	EGGYGOSquadMemberState GetMemberState(int32 SlotIndex) const;

	/** 切到下一个可出战的位置。已死亡或无实体的位置会被跳过。仅服务器有效。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool SwitchToNextSlot();

	/** 切到上一个可出战的位置。仅服务器有效。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool SwitchToPreviousSlot();

	/**
	 * 客户端请求按队伍顺序切到相邻的可出战位置：Direction 为 -1 上一个、1 下一个。
	 * 目标由服务器按权威的出战序号与存活状态选择，来源记为 PlayerRequest；在服务器上调用时直接执行。
	 */
	UFUNCTION(Server, Reliable, WithValidation, BlueprintCallable, Category = "GGYGO|Squad")
	void ServerRequestSwitchToAdjacentSlot(int32 Direction);

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

	/** 把位置的 Pawn 设为出战：显示、开碰撞、恢复骨架更新。 */
	void ActivateSlot(AGGYGOCharacterSlot* Slot);

	/** 把位置的 Pawn 设为待命：隐藏、关碰撞、停骨架更新、停止移动。 */
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

	/** 从出战位置起按 Direction（±1）循环找第一个可出战的位置并切换；只试一次真实切换。 */
	bool SwitchToAdjacentSlot(int32 Direction, EGGYGOSwitchSource Source);

	/** 两种入口共用接收事务；所有身份验证先于名单和责任提交。 */
	bool RegisterSlotInternal(AGGYGOCharacterSlot* Slot, APawn* OriginalCreatedPawn, bool bAcceptCreatedResources);

	/** 当前组件必须是权威 PlayerState 的唯一队伍，且 Controller/World 对应。 */
	APlayerController* GetRegistrationController() const;

	/** 以 Slot 验证持久连接归属及实际 ASC/Avatar/Extension Ready；Pawn Owner 不表示队伍归属。 */
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
	/** 仅由一次同步 Switch 栈持有，退出即归还；不是第二份出战状态或延期切换队列。 */
	bool bSwitchRequestInProgress = false;

	/** 一个槽位的场上状态与本组件施加的资源；与 Slots 同序，仅服务器。 */
	struct FSlotRuntime
	{
		EGGYGOSquadMemberState State = EGGYGOSquadMemberState::Benched;
		/** 选择 ContinueOnField、仍在执行的能力。 */
		TArray<FGameplayAbilitySpecHandle> Continuing;
		/** 本次 SwitchOut 事件激活、仍在执行的能力。 */
		TArray<FGameplayAbilitySpecHandle> ExitAbilities;
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ObservedASC;
		FDelegateHandle AbilityEndedHandle;
		/** 施加 OffField 时改过 bRunPhysicsWithNoController 的移动组件及原值。 */
		TWeakObjectPtr<UCharacterMovementComponent> OffFieldMovement;
		bool bSavedRunPhysicsWithNoController = false;
		bool bOffField = false;
		bool bReturnPending = false;
	};
	TArray<FSlotRuntime> SlotRuntimes;

	/** 槽位能力结束：更新续放／退场集合，按状态推进退场、完成退场或解除 ReturnPending。 */
	void HandleSlotAbilityEnded(const FAbilityEndedData& Data, TWeakObjectPtr<AGGYGOCharacterSlot> Slot);
	/** 外调后核对：组件未终止，Index 仍是原 Slot。 */
	bool IsSlotCurrent(int32 Index, const AGGYGOCharacterSlot* Slot) const;
	/** 失去控制的队员离场：加 OffField，有续放进入 Lingering，否则 BeginExit。 */
	void MoveOffField(int32 Index);
	/** 发 SwitchOut；有能力被激活且仍在执行时等待它们结束，否则立即 FinishExit。 */
	void BeginExit(int32 Index);
	/** 解除 OffField 后隐藏、关碰撞、停用移动，转 Benched。 */
	void FinishExit(int32 Index);
	/** OffField：复制的 State.OffField、仅服务器的伤害免疫，及无 Controller 时继续执行移动。 */
	void SetOffField(int32 Index, bool bOffField);
	void SetReturnPending(int32 Index, bool bPending);
	/** 向槽位 ASC 发 Squad 事件，返回这次事件同步激活的能力。 */
	TArray<FGameplayAbilitySpecHandle> SendSquadEvent(AGGYGOCharacterSlot* Slot, FGameplayTag EventTag);
	/** 终止时退订并移除本组件施加的 Tag 与移动开关。 */
	void ReleaseSlotRuntimes();
};
