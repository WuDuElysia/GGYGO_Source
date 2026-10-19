/** @file GGYGOAbilityTask_PlayNiagaraEffect.h @brief GAS 所持单个 Niagara 表现资源的创建与释放。 */
#pragma once

#include "Abilities/Tasks/AbilityTask.h"
#include "Engine/EngineTypes.h"
#include "GGYGOAbilityTask_PlayNiagaraEffect.generated.h"

class UNiagaraComponent;
class UNiagaraSystem;
class USceneComponent;
class UGGYGOAbilityTask_PlayNiagaraEffect;
class UAbilitySystemComponent;
class AActor;
class UWorld;
class USkeletalMeshComponent;
class UGGYGOMontageFXPublicationOwner;
struct FGGYGOMontageFXLocalCapability;
struct FGGYGOMontageFXLocalStartTicket;
struct FGGYGOMontageFXLocalStartReadyFact;
enum class EGGYGOMontageFXLocalStartResult : uint8;
enum class EGGYGOMontageFXPermitResult : uint8;
DECLARE_DELEGATE_OneParam(FGGYGOMontageFXLocalStartReadyDelegate, const FGGYGOMontageFXLocalStartReadyFact&);

UENUM(BlueprintType)
enum class EGGYGONiagaraEffectSpace : uint8
{
	Unspecified,
	World,
	Attached
};

/** Tail 模式把停止生成后的组件交给 Niagara AutoDestroy；交回后不再属于 GA。 */
UENUM(BlueprintType)
enum class EGGYGONiagaraEffectStopMode : uint8
{
	Unspecified,
	DestroyImmediately,
	DeactivateKeepAttached,
	DeactivateDetachKeepWorld
};

/** 记录原启动调用的事实；请求接受不证明首粒子、首帧可见或模拟完成。 */
UENUM(BlueprintType)
enum class EGGYGONiagaraEffectStartResult : uint8
{
	NotRequested,
	SpawnAndActivationRequested,
	DedicatedServerNoPresentation,
	Failed
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FGGYGONiagaraEffectFailedDelegate,
	UGGYGOAbilityTask_PlayNiagaraEffect*, Task, const FString&, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FGGYGONiagaraEffectSpawnedDelegate,
	UGGYGOAbilityTask_PlayNiagaraEffect*, Task, UNiagaraComponent*, Component);

/**
 * 只拥有自身生成的一个瞬态组件。招式窗口、动作身份与取消原因由调用 GA 管理；
 * 原生完成仅结束本 Task，不能作为动作完成事件。未显式交出的资源随 Task 销毁。
 */
UCLASS()
class GGYGO_API UGGYGOAbilityTask_PlayNiagaraEffect : public UAbilityTask
{
	GENERATED_BODY()
public:
	/**
	 * 仅设置请求；调用方先保存 Task 并绑定委托，再 ReadyForActivation。
	 * @param Transform World 为最终世界变换；Attached 为原组件/Socket 的相对变换。
	 * @param AttachComponent World 必须为空；Attached 必须已注册且与原 Avatar 同 World。
	 * @return 原 Task；无有效活动 Ability 时显式拒绝并返回空。
	 */
	UFUNCTION(BlueprintCallable, Category = "Ability|Tasks", meta = (DisplayName = "Play Niagara Effect",
		HidePin = "OwningAbility", DefaultToSelf = "OwningAbility", BlueprintInternalUseOnly = "true"))
	static UGGYGOAbilityTask_PlayNiagaraEffect* PlayNiagaraEffect(UGameplayAbility* OwningAbility,
		FName TaskInstanceName, UNiagaraSystem* System, EGGYGONiagaraEffectSpace Space,
		USceneComponent* AttachComponent, FName SocketName, const FTransform& Transform);

	/** 显式释放原资源；Tail 成功交回后结束 Task，后继取消不会追收已交出的组件。 */
	UFUNCTION(BlueprintCallable, Category = "Ability|Tasks")
	void StopEffect(EGGYGONiagaraEffectStopMode Mode);

	/** 仅验证必需资产、明确空间及变换；World、实际 Socket 和原生就绪在 Activate 核验。 */
	static bool ValidateConfiguration(UNiagaraSystem* System, EGGYGONiagaraEffectSpace Space,
		const FTransform& Transform, FString& OutFailureReason);
	static bool IsStopModeValid(EGGYGONiagaraEffectStopMode Mode);
	/** 同一编译/VM 结构校验供远端实例复用；合法编译暂态交原生组件等待。 */
	static bool ValidateCompiledSystem(UNiagaraSystem* System, FString& OutFailureReason);
	/** 调用者先摘准确资源；返回前完成销毁或合法尾部转交，外调后核原 owner。 */
	static bool StopComponent(UNiagaraComponent* Component, EGGYGONiagaraEffectStopMode Mode,
		TFunctionRef<bool()> IsOriginalOwnerAlive);

	EGGYGOMontageFXLocalStartResult RequestMontageFXLocalStart(UAbilitySystemComponent* OriginalIssuer,
		AActor* OriginalAvatar, USkeletalMeshComponent* OriginalMesh, UWorld* OriginalWorld,
		FGGYGOMontageFXLocalStartTicket& OutTicket, FString& OutError);
	bool ObserveMontageFXLocalStartReady(const FGGYGOMontageFXLocalStartTicket& Ticket,
		FGGYGOMontageFXLocalStartReadyDelegate Callback);
	EGGYGOMontageFXPermitResult TryConsumeMontageFXLocalStartPermit(const FGGYGOMontageFXLocalStartTicket& Ticket);
	void CancelMontageFXLocalStartRequest(const FGGYGOMontageFXLocalStartTicket& Ticket);

	UFUNCTION(BlueprintPure, Category = "Ability|Tasks")
	EGGYGONiagaraEffectStartResult GetStartResult() const { return StartResult; }

	UPROPERTY(BlueprintAssignable)
	FGGYGONiagaraEffectFailedDelegate OnFailed;
	/** 原组件已取得并调用原生 Activate；编译／PSO 等待由组件处理，不表示模拟或画面已完成。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGONiagaraEffectSpawnedDelegate OnSpawned;

	virtual void Activate() override;
	virtual void OnDestroy(bool AbilityEnded) override;

private:
	void ActivateNative();
	void StopNative(EGGYGONiagaraEffectStopMode Mode);
	void DestroyNative(bool AbilityEnded);
	void ResumeLegacyStart(const FGGYGOMontageFXLocalStartReadyFact& Fact);
	void FailAndEndTask(const FString& Reason);
	UNiagaraComponent* ReleaseOwnedComponent();
	UFUNCTION()
	void HandleSystemFinished(UNiagaraComponent* Component);
	UFUNCTION()
	void HandleOriginalAvatarEndPlay(AActor* Actor, EEndPlayReason::Type Reason);
#if WITH_EDITOR
	void HandleSystemCompiled(UNiagaraSystem* System);
	FDelegateHandle SystemCompiledHandle;
#endif

	UPROPERTY()
	TObjectPtr<UNiagaraSystem> RequestedSystem;
	UPROPERTY()
	TWeakObjectPtr<USceneComponent> RequestedAttachComponent;
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> OwnedComponent;
	FName RequestedSocketName;
	FTransform RequestedTransform = FTransform::Identity;
	EGGYGONiagaraEffectSpace RequestedSpace = EGGYGONiagaraEffectSpace::Unspecified;
	EGGYGONiagaraEffectStartResult StartResult = EGGYGONiagaraEffectStartResult::NotRequested;
	UPROPERTY(Transient) TObjectPtr<UGGYGOMontageFXPublicationOwner> ResourceOwner;
	TSharedPtr<FGGYGOMontageFXLocalCapability> LocalCapability;
	TWeakObjectPtr<UAbilitySystemComponent> OriginalIssuer;
	TWeakObjectPtr<AActor> OriginalAvatar;
	TWeakObjectPtr<UWorld> OriginalWorld;
	/** 不完全类型存储在 cpp；h 不反向 include publication。 */
	TSharedPtr<FGGYGOMontageFXLocalStartTicket> LocalStartTicket;
};
