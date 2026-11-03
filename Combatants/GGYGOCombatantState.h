/**
 * @file GGYGOCombatantState.h
 * @brief 可替换 Pawn 的持久战斗状态宿主
 *
 * 本类只抽取外置 ASC 宿主共有的生命周期机制：ASC、基础属性集与 Avatar 绑定。
 * 玩家编队、Boss 阶段、AI、PawnData 与能力授予都属于派生类，不能下沉到这里。
 */
#pragma once

#include "AbilitySystemInterface.h"
#include "Character/Interfaces/GGYGOAvatarBindingHostInterface.h"
#include "GameFramework/Info.h"

#include "GGYGOCombatantState.generated.h"

class APawn;
class AActor;
class UAbilitySystemComponent;
class UGGYGOAbilitySystemComponent;
class UGGYGOCombatSet;
class UGGYGOHealthSet;
class UGGYGOMontageFXReplicationComponent;

/** 一次绑定请求的直接结果；调用方不需要再查询中间步骤。 */
enum class EGGYGOAvatarBindResult : uint8
{
	/** ASC 已绑定到新 Pawn，并已通知其 PawnExtension。 */
	Bound,
	/** 请求的 Pawn 已是当前 Avatar，未做任何事。 */
	AlreadyBound,
	/** 已释放旧 Avatar，ASC 只保留 Owner。 */
	Cleared,
	/** 绑定回调中再次请求同一宿主的绑定。 */
	Reentrant,
	AuthorityDenied,
	/** 宿主已 EndPlay／销毁。 */
	LifecycleClosed,
	/** 目标 Pawn 无效或正在销毁。 */
	InvalidPawn,
	/** ASC 处于 Dying／Dead，不接受新 Avatar。 */
	HostDying,
	/** 目标 Pawn 仍绑定在另一宿主的 ASC 上；跨宿主须先由原宿主释放。 */
	PawnBoundElsewhere
};

/**
 * 外置 ASC 的最小宿主（Lyra PlayerState 持有 ASC 的做法）。
 *
 * OwnerActor 恒为本 Actor，AvatarActor 可以在运行时替换。绑定流程：
 * 释放旧 Avatar（取消能力、清输入、移除 Cue、旧 Pawn 广播 Released）→
 * InitAbilityActorInfo(本宿主, 新 Pawn) → 新 Pawn 广播 Ready。
 * 服务器由 AttachAvatar 发起，客户端由 AvatarPawn 的复制回调发起。
 */
UCLASS(Abstract, meta = (ShortTooltip = "可替换 Pawn 的持久战斗状态宿主"))
class GGYGO_API AGGYGOCombatantState : public AInfo, public IAbilitySystemInterface,
	public IGGYGOAvatarBindingHostInterface
{
	GENERATED_BODY()

public:
	AGGYGOCombatantState(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	//~IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	//~End of IAbilitySystemInterface

	//~IGGYGOAvatarBindingHostInterface
	virtual void ReleaseAvatarBinding(APawn* ExpectedAvatar) override;
	//~End of IGGYGOAvatarBindingHostInterface

	/** 类型化访问器。构造完成后始终非空。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combatant")
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponent() const { return AbilitySystemComponent; }

	/** 仅服务器：让 NewAvatar 成为本宿主的唯一 Avatar；nullptr 表示解除当前 Avatar。 */
	EGGYGOAvatarBindResult AttachAvatar(APawn* NewAvatar);

	/** 仅服务器：ExpectedAvatar 为空或等于当前 Avatar 时解除绑定。 */
	void DetachAvatar(APawn* ExpectedAvatar = nullptr);

	/** 服务器选择的 Avatar（客户端为复制值）；没有实体时为 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combatant")
	APawn* GetAvatarPawn() const { return AvatarPawn; }

protected:
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void Destroyed() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 客户端收到 Avatar 复制后走与服务器相同的绑定流程。 */
	UFUNCTION()
	void OnRep_AvatarPawn();

	/** 当前 Avatar EndPlay 或销毁时解除绑定；已被替换的旧 Pawn 不处理。未 BeginPlay 的 Actor 销毁时只有 OnDestroyed。 */
	UFUNCTION()
	void HandleAvatarEndPlay(AActor* Actor, EEndPlayReason::Type EndPlayReason);
	UFUNCTION()
	void HandleAvatarDestroyed(AActor* DestroyedActor);

	/** 本宿主的 ASC。派生类只配置复制模式，不替换实例。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Combatant", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOAbilitySystemComponent> AbilitySystemComponent;

	/** 基础生命与韧性属性；默认子对象保证早于任何 Pawn 就绪。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Combatant", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOHealthSet> HealthSet;

	/** 基础输出属性；生命周期与 ASC Owner 一致。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Combatant", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOCombatSet> CombatSet;

	/** Montage/运动 FX 的发布宿主；随本 Actor 复制，与 ASC 同寿命、同相关性。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Combatant", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOMontageFXReplicationComponent> MontageFXReplication;

	/** 服务器选择的 Avatar；复制后客户端据此重建本地绑定。 */
	UPROPERTY(ReplicatedUsing = OnRep_AvatarPawn)
	TObjectPtr<APawn> AvatarPawn;

private:
	/** 释放旧 Avatar（若不同）并绑定 NewPawn；nullptr 只释放。权威端同步写 AvatarPawn。 */
	EGGYGOAvatarBindResult BindAvatar(APawn* NewPawn);
	/**
	 * 绑定流程内调用：取消能力、清输入、移除 Cue，解除 ActorInfo（宿主关闭时清空，否则只保留 Owner），
	 * 最后旧 Pawn 广播 Released。
	 */
	void ReleaseAvatar(APawn* OldPawn, bool bClearActorInfo);
	/** EndPlay／Destroyed：释放当前 Avatar 并清空 ActorInfo，关闭后续绑定。 */
	void CloseAvatarBinding();

	/** ASC 当前实际绑定的 Pawn。客户端的 AvatarPawn 是服务器选择，可能先于本地绑定变化。 */
	TWeakObjectPtr<APawn> BoundAvatar;
	/** 绑定流程进行中；其间的回调再次请求绑定会被拒绝。 */
	bool bBindingInProgress = false;
	/** EndPlay／Destroyed 关闭，真实 BeginPlay 重开。 */
	bool bAvatarBindingClosed = false;
};
