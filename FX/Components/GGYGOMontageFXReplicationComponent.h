/** @file GGYGOMontageFXReplicationComponent.h @brief CombatantState 上的 FX 发布宿主：复制状态、终止多播与发布入口。 */
#pragma once

#include "Components/ActorComponent.h"
#include "FX/GGYGOMontageFXPublication.h"
#include "GGYGOMontageFXReplicationComponent.generated.h"

class UAbilitySystemComponent;
class UGGYGOAbilitySystemComponent;
class UGGYGOAbilityTask_PlayMontageAndWaitForEvent;
class UGGYGOCharacterMovementComponent;
class UGGYGOGameplayAbility;
class FGGYGOAbilityActivationHandle;
struct FGGYGOMontageWindowFact;
struct FGGYGOMotionExecutionSnapshot;

/**
 * 与 ASC 挂在同一 CombatantState 上，随它复制与相关，晚加入或重获相关性时收到当前全部活动记录。
 * 只签发发布并返回 lease；关闭由 lease 持有者在释放自身资源时执行，本组件不扫描、不代关。
 */
UCLASS(ClassGroup = (GGYGO))
class GGYGO_API UGGYGOMontageFXReplicationComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGGYGOMontageFXReplicationComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 取 ASC 所在 Actor 上的宿主；没有时返回 nullptr，发布方须明确拒绝，不改为本地直接播放。 */
	static UGGYGOMontageFXReplicationComponent* FindForASC(const UAbilitySystemComponent* ASC);

	/** 当前注册的发行者；未注册时为 nullptr。 */
	UGGYGOMontageFXPublicationOwner* GetPublicationOwner() const { return PublicationOwner; }

	/**
	 * 权威端发布原 GA/Task 的同步 Begin 事实。要求原 GA 的 ASC 与本组件同属一个 Owner、
	 * 原激活仍是当前激活、Task 属于该 GA 且 Begin 事实来自它，Mesh 与 Avatar 一致。
	 */
	bool PublishMontageFX(UGGYGOGameplayAbility* OriginalAbility,
		const FGGYGOAbilityActivationHandle& OriginalActivation,
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* OriginalTask,
		const FGGYGOMontageWindowFact& BeginFact, const FGGYGOMontageFXRecord& Request,
		FGGYGOMontageFXLease& OutLease, FString& OutDiagnostic);

	/**
	 * 权威端发布原 CMC 认证的完成区间。BeginServerTime 只取原 producer 事件时间，不消费调用方填值。
	 * 成功只证明发布并返回原 lease，不证明本地 Niagara 已启动或完成。
	 */
	bool PublishLocomotionFX(UGGYGOCharacterMovementComponent* OriginalMovement,
		const FGGYGOMotionExecutionSnapshot& CompletedInterval, const FGGYGOMontageFXRecord& Request,
		FGGYGOMontageFXLease& OutLease, FString& OutDiagnostic);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

private:
	/** 同一 Owner 上的项目 ASC；Locomotion 发布用它的绑定 Context 与 ActorInfo 认证 Avatar。 */
	UGGYGOAbilitySystemComponent* GetOwnerAbilitySystem() const;
	bool IsLocomotionAvatarCurrent(const UGGYGOAbilitySystemComponent* ASC,
		const UGGYGOCharacterMovementComponent* OriginalMovement,
		const AActor* OriginalAvatar, const USkeletalMeshComponent* OriginalMesh) const;
	void SendTerminal(const FGGYGOMontageFXTerminal& Terminal);
	UFUNCTION()
	void OnRep_State();
	UFUNCTION(NetMulticast, Reliable)
	void MulticastTerminal(const FGGYGOMontageFXTerminal& Terminal);

	/** 当前注册的强别名；跨重注册的稳定资源拥有者由 State.LocalOwner 保持。 */
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOMontageFXPublicationOwner> PublicationOwner;
	UPROPERTY(ReplicatedUsing = OnRep_State)
	FGGYGOMontageFXState State;
};
