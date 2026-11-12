/** @file GGYGOAbilityTask_PlayNiagaraEffect.h @brief GAS 所持 Niagara 单元组合的创建与释放。 */
#pragma once

#include "Abilities/Tasks/AbilityTask.h"
#include "Engine/EngineTypes.h"
#include "FX/GGYGOFXFieldSettings.h"
#include "UObject/StrongObjectPtr.h"
#include "GGYGOAbilityTask_PlayNiagaraEffect.generated.h"

class UNiagaraComponent;
class UNiagaraSystem;
class USceneComponent;
class UGGYGOAbilityTask_PlayNiagaraEffect;
class UAbilitySystemComponent;
class AActor;
class UWorld;
class USkeletalMeshComponent;
class UGGYGOFXNiagaraComponent;
class UGGYGOMontageFXPublicationOwner;
struct FGGYGOMontageFXLocalCapability;
struct FGGYGOMontageFXLocalStartTicket;
struct FGGYGOMontageFXLocalStartReadyFact;
enum class EGGYGOMontageFXLocalStartResult : uint8;
enum class EGGYGOMontageFXPermitResult : uint8;

/** 同一动作窗内独立编译的实际单元；每个 System 只消费自己的场材质。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGONiagaraEffectUnit
{
	GENERATED_BODY()
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX") TObjectPtr<UNiagaraSystem> System = nullptr;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX|Field") FGGYGOFXFieldSettings FieldSettings;
	/** 仅用于 Attached；未启用时继承动作窗的 Socket 和相对变换。配置来自作者资产。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX|Attachment") bool bOverrideAttachment = false;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX|Attachment", meta = (EditCondition = "bOverrideAttachment"))
	FName SocketName = NAME_None;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX|Attachment", meta = (EditCondition = "bOverrideAttachment"))
	FTransform LocalTransform = FTransform::Identity;

	/** 本地创建与网络发布共用的纯配置解析；未启用覆盖时完整保留调用方输入。 */
	void ResolveAttachment(FName DefaultSocket, const FTransform& DefaultTransform,
		FName& OutSocket, FTransform& OutTransform) const;
};
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
DECLARE_MULTICAST_DELEGATE_TwoParams(FGGYGONiagaraEffectUnitNaturallyCompletedDelegate,
	UGGYGOAbilityTask_PlayNiagaraEffect*, UNiagaraComponent*);

/**
 * 一个 Task/cap/ticket 拥有自身生成的完整瞬态组件组。招式窗口、动作身份与取消原因由调用 GA 管理；
 * 全部单元原生完成才结束本 Task，不能作为动作完成事件。未显式交出的资源随 Task 销毁。
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
		USceneComponent* AttachComponent, FName SocketName, const FTransform& Transform,
		const FGGYGOFXFieldSettings& FieldSettings);

	/** 一个原 Task/cap/ticket 同时持有完整组合；OnSpawned 逐实际组件发出原生激活请求事实。 */
	static UGGYGOAbilityTask_PlayNiagaraEffect* PlayNiagaraEffects(UGameplayAbility* OwningAbility,
		FName TaskInstanceName, const TArray<FGGYGONiagaraEffectUnit>& Units, EGGYGONiagaraEffectSpace Space,
		USceneComponent* AttachComponent, FName SocketName, const FTransform& Transform);

	/** 显式释放原资源；Tail 成功交回后结束 Task，后继取消不会追收已交出的组件。 */
	UFUNCTION(BlueprintCallable, Category = "Ability|Tasks")
	void StopEffect(EGGYGONiagaraEffectStopMode Mode);

	/** 仅验证必需资产、明确空间及变换；World、实际 Socket 和原生就绪在 Activate 核验。 */
	static bool ValidateConfiguration(UNiagaraSystem* System, EGGYGONiagaraEffectSpace Space,
		const FTransform& Transform, FString& OutFailureReason);
	static bool ValidateConfigurations(const TArray<FGGYGONiagaraEffectUnit>& Units,
		EGGYGONiagaraEffectSpace Space, const FTransform& Transform, FString& OutFailureReason);
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
	/** 原组件已取得并请求 Activate；最终材质／编译／PSO 等待归原组件，不表示模拟或画面已完成。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGONiagaraEffectSpawnedDelegate OnSpawned;
	/** 原 owned 单元无失败地触发 OnSystemFinished 后、销毁前发出；显式 Stop/取消不发出。 */
	FGGYGONiagaraEffectUnitNaturallyCompletedDelegate OnUnitNaturallyCompleted;

	virtual void Activate() override;
	virtual void OnDestroy(bool AbilityEnded) override;

private:
	void ActivateNative();
	void StopNative(EGGYGONiagaraEffectStopMode Mode);
	void DestroyNative(bool AbilityEnded);
	void ResumeLegacyStart(const FGGYGOMontageFXLocalStartReadyFact& Fact);
	void FailAndEndTask(const FString& Reason);
	void HandleEffectFailed(UGGYGOFXNiagaraComponent* Component, const FString& Reason);
	TArray<TStrongObjectPtr<UNiagaraComponent>> ReleaseOwnedComponents();
	void UnbindOwnedComponent(UNiagaraComponent* Component);
	UFUNCTION()
	void HandleSystemFinished(UNiagaraComponent* Component);
	UFUNCTION()
	void HandleOriginalAvatarEndPlay(AActor* Actor, EEndPlayReason::Type Reason);
#if WITH_EDITOR
	void HandleSystemCompiled(UNiagaraSystem* System);
	TMap<UNiagaraSystem*, FDelegateHandle> SystemCompiledHandles;
#endif

	UPROPERTY()
	TArray<FGGYGONiagaraEffectUnit> RequestedUnits;
	UPROPERTY()
	TWeakObjectPtr<USceneComponent> RequestedAttachComponent;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UNiagaraComponent>> OwnedComponents;
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
