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

namespace EEndPlayReason { enum Type : int; }

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
class GGYGO_API AGGYGOCombatantState : public AInfo, public IAbilitySystemInterface,
	public IGGYGOAvatarBindingHostInterface
{
	GENERATED_BODY()

public:
	AGGYGOCombatantState(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	//~IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	//~End of IAbilitySystemInterface

	/** Synchronous native request; results are operation history, never current ownership. */
	virtual FGGYGOAvatarBindingHostResult RequestAvatarBinding(
		const FGGYGOAvatarBindingHostRequest& Request) override;

	/** 类型化访问器。构造完成后始终非空。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combatant")
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponent() const { return AbilitySystemComponent; }

	/**
	 * 让 NewAvatar 成为本状态宿主的唯一 Avatar。仅服务器调用。
	 *
	 * 兼容选择入口：捕获旧资源，先 Release，再用该次提交 Context 请求 Initialize。
	 * 跨宿主移交由外层显式执行旧宿主 Release → 核对 → 新宿主 Initialize。
	 */
	void AttachAvatar(APawn* NewAvatar);

	/**
	 * 解除宿主持有的原资源。ExpectedAvatar 只筛选目标，清理权限来自原 H/Context。
	 */
	void DetachAvatar(APawn* ExpectedAvatar = nullptr);

	/** 当前 Avatar；没有实体时为 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combatant")
	APawn* GetAvatarPawn() const { return AvatarPawn; }

protected:
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void Destroyed() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 客户端收到 Avatar 复制后走与服务器相同的绑定路径。 */
	UFUNCTION()
	void OnRep_AvatarPawn();

	/** 服务器上的 Avatar 被销毁时及时清空复制引用，避免状态宿主留下悬空 Avatar。 */
	UFUNCTION()
	void HandleAvatarDestroyed(AActor* DestroyedActor);

	/** Capture the selected/replicated target once and use the same request chain. */
	void SynchronizeAvatarBinding();

	/** Release only the original Host-held resource for this Pawn. */
	void ClearLocalAvatarBinding(APawn* AvatarToClean);

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

private:
	/** Close admission once, then clean only the captured original binding before native callbacks. */
	void CloseAvatarBindingLifecycle(const TCHAR* EntryPoint);
	FGGYGOAvatarBindingHostResult InitializeAvatarBinding(const FGGYGOAvatarBindingHostRequest& Request);
	FGGYGOAvatarBindingHostResult ReleaseAvatarBinding(const FGGYGOAvatarBindingHostRequest& Request);
	FGGYGOAvatarBindingHostResult RefreshAvatarBinding(const FGGYGOAvatarBindingHostRequest& Request);
	FGGYGOAvatarBindingHostResult CoordinateAvatarSelection(APawn* DesiredAvatar);
	FGGYGOAvatarBindingHostRequest MakeAvatarResourceRequest(
		EGGYGOAvatarBindingHostOperation Operation) const;
	bool IsOriginalAvatarResource(const FGGYGOAvatarBindingHostRequest& Request) const;
	bool InitializeOwnerActorInfo();
	void RetireHostAvatarResource(const FGGYGOPawnASCResourceHandle& OriginalResource);
	FGGYGOAvatarBindingResult PublishAvatarResources(
		const FGGYGOAvatarBindingPublicationReceipt& Publication,
		const FGGYGOAvatarBindingContext& CommittedContext,
		const FGGYGOPawnASCResourceHandle& OriginalResource,
		UGGYGOPawnExtensionComponent* OriginalExtension, bool bReleased,
		FGGYGOAvatarBindingHostResult& OutHistory);

	/** Borrowed Extension resource and ASC-issued context; no identity issuer or binding authority. */
	FGGYGOPawnASCResourceHandle AvatarResource{};
	FGGYGOAvatarBindingContext AvatarResourceContext{};
	TWeakObjectPtr<UGGYGOPawnExtensionComponent> AvatarResourceExtension{};

	/** 只读取本次生命周期准入与原生销毁状态，不限制必要清理。 */
	bool IsAvatarBindingPermitted() const;

	/** 非空绑定调用边界的校验与明确诊断；不提供绑定成功结果。 */
	bool ValidateAvatarBinding(APawn* AvatarToBind, const TCHAR* EntryPoint) const;

	/** 首次 PreBegin 可绑定；Destroyed/EndPlay 先关闭，仅真实BeginPlay重开；不是清理成功状态。 */
	bool bAvatarBindingPermitted = true;
};
