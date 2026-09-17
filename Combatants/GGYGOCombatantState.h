/**
 * @file GGYGOCombatantState.h
 * @brief 可替换 Pawn 的持久战斗状态宿主
 *
 * 本类只抽取外置 ASC 宿主共有的生命周期机制：ASC、基础属性集与 Avatar 绑定。
 * 玩家编队、Boss 阶段、AI、PawnData 与能力授予都属于派生类，不能下沉到这里。
 */
#pragma once

#include "AbilitySystemInterface.h"
#include "GameFramework/Info.h"

#include "GGYGOCombatantState.generated.h"

class APawn;
class AActor;
class UAbilitySystemComponent;
class UGGYGOAbilitySystemComponent;
class UGGYGOCombatSet;
class UGGYGOHealthSet;

/**
 * 外置 ASC 的最小宿主。
 *
 * OwnerActor 恒为本 Actor，AvatarActor 可以在运行时替换。派生类决定复制模式、
 * AbilitySet、PawnData 以及谁负责生成 Pawn。
 */
UCLASS(Abstract, meta = (ShortTooltip = "可替换 Pawn 的持久战斗状态宿主"))
class GGYGO_API AGGYGOCombatantState : public AInfo, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AGGYGOCombatantState(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	//~IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	//~End of IAbilitySystemInterface

	/** 类型化访问器。构造完成后始终非空。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combatant")
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponent() const { return AbilitySystemComponent; }

	/**
	 * 让 NewAvatar 成为本状态宿主的唯一 Avatar。仅服务器调用。
	 *
	 * 本函数是外部更换 Avatar 的唯一入口：它同时更新复制引用、PawnExtension 与
	 * AbilityActorInfo，避免生成方、Pawn 和状态宿主分别写一遍。
	 */
	void AttachAvatar(APawn* NewAvatar);

	/**
	 * 解除当前 Avatar。ExpectedAvatar 非空时仅在它仍是当前 Avatar 时执行。
	 * 这个比较让旧 Pawn 的延迟 EndPlay 不会误清掉已经接管的新 Pawn。
	 */
	void DetachAvatar(APawn* ExpectedAvatar = nullptr);

	/** 当前 Avatar；没有实体时为 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combatant")
	APawn* GetAvatarPawn() const { return AvatarPawn; }

protected:
	virtual void PostInitializeComponents() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 客户端收到 Avatar 复制后走与服务器相同的绑定路径。 */
	UFUNCTION()
	void OnRep_AvatarPawn();

	/** 服务器上的 Avatar 被销毁时及时清空复制引用，避免状态宿主留下悬空 Avatar。 */
	UFUNCTION()
	void HandleAvatarDestroyed(AActor* DestroyedActor);

	/** 把 AvatarPawn 与 ASC/PawnExtension 对齐；服务器与 OnRep 共用。 */
	void SynchronizeAvatarBinding();

	/** 本宿主的 ASC。派生类只配置复制模式，不替换实例。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Combatant", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOAbilitySystemComponent> AbilitySystemComponent;

	/** 基础生命与韧性属性；默认子对象保证早于任何 Pawn 就绪。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Combatant", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOHealthSet> HealthSet;

	/** 基础输出属性；生命周期与 ASC Owner 一致。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Combatant", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOCombatSet> CombatSet;

	/** 当前 Avatar。复制后客户端会重新建立本地 AbilityActorInfo。 */
	UPROPERTY(ReplicatedUsing = OnRep_AvatarPawn)
	TObjectPtr<APawn> AvatarPawn;
};
