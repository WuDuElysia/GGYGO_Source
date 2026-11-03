/** @file GGYGOPlayerComboAbility.h @brief 一个 GAS 能力管理逐段 Main→End 普攻 */
#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/Abilities/GGYGOComboTypes.h"
#include "AbilitySystem/GGYGOAbilityInputRequestTypes.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "GGYGOPlayerComboAbility.generated.h"

class UGGYGOAbilityTask_PlayMontageAndWaitForEvent;
class UGGYGOAbilityTask_WaitComboInput;
class UGGYGOAbilityTask_PlayNiagaraEffect;
class UGGYGOMeleeTraceComponent;
class USkeletalMeshComponent;
class UWorld;
class APawn;
class UAnimMontage;
class UGGYGOPlayerComboAbility;
class UGGYGOCharacterMovementComponent;
struct FGameplayAbilityTargetDataHandle;
struct FGGYGOPlayerComboLifecycleFixture;
struct FGGYGOMontageSectionFact;
struct FGGYGOMontageNotifyFact;
struct FGGYGOMontageWindowFact;
struct FGGYGOMontageWindowDeclaration;
struct FGGYGOMontageFXLocalStartReadyFact;
struct FGGYGOMontageSectionSnapshot;
enum class EGGYGOActionMotionReleaseReason : uint8;

/** 原 GA 消费者签发的派生事实；不授播放、输入、位移或结束权限。
 * 原弱对象可退役，Activation 只用于匹配原历史；布尔量分别来自各自的真实边界。 */
struct GGYGO_API FGGYGOPlayerComboObservationFacts
{
	FGGYGOAbilityActivationHandle Activation;
	TWeakObjectPtr<UGGYGOPlayerComboAbility> Ability;
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
	TWeakObjectPtr<APawn> Pawn;
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> Task;
	TWeakObjectPtr<UAnimMontage> Montage;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> Movement;
	uint64 StepToken = 0;
	int32 StepIndex = INDEX_NONE;
	int32 MontageInstanceId = INDEX_NONE;
	int32 MotionHandle = INDEX_NONE;
	bool bTaskNaturalEnded = false;
	bool bConsumerAccepted = false;
	bool bDrainRequestReturned = false;
	bool bDrainAccepted = false;
	bool bSourceCurrentAfterDrainReturn = false;
	bool bMotionCompleted = false;
	bool bCleanupObserved = false;
	bool bCleanupCancelled = false;
	EGGYGOAbilityTerminationRequestKind CleanupKind = EGGYGOAbilityTerminationRequestKind::None;
	FString DrainDiagnostic;
};

/** 只读的原段历史视图。GA 是唯一写入者；不持有资源、Task 或 Actor 的强引用。
 * 派生记录随原资源退役；调用方可保留原历史，Stop 时 Reset，不得用它重新捕获后继。 */
class GGYGO_API FGGYGOPlayerComboObservation
{
public:
	bool ReadHistory(FGGYGOPlayerComboObservationFacts& OutFacts) const;
	void Reset() { History.Reset(); }
private:
	struct FState;
	TSharedPtr<const FState> History;
	friend class UGGYGOPlayerComboAbility;
};

UCLASS(Blueprintable)
class GGYGO_API UGGYGOPlayerComboAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()
	struct FAuthorPointGate;
	struct FStepMotionResources;
	struct FStepEntryAttempt;
public:
	UGGYGOPlayerComboAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	/** ASC 只向匹配本次激活预测键的实例转发服务器纠正。 */
	void CorrectPredictedStep(int32 Revision, int32 RequestId, int32 ServerStep, float Position,
		bool bWindowOpen, bool bWindowClosed, bool bAccepted);
	virtual void ReceiveAbilityCorrection(const FGameplayAbilityTargetDataHandle& Correction) override;
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combo")
	int32 GetCurrentComboStep() const { return CurrentStep; }
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combo")
	bool IsComboWindowOpen() const { return Window.bOpen; }
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combo")
	bool HasPendingComboRequest() const { return Window.PendingRequestId > 0; }
	/** 保留的反射查询：当前段攻击接续门是否已开；与 IsAttackContinuationOpen 同一事实，不含移动打断。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combo")
	bool IsInterruptionOpen() const;
	/** 当前段攻击接续门是否已由其原实例作者点打开。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combo")
	bool IsAttackContinuationOpen() const;
	/** 当前段移动打断门是否已由其原实例作者点及 Main 真实末端打开。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combo")
	bool IsMovementInterruptionOpen() const;
	bool ValidateComboConfiguration(FString& OutError) const;
	/** 捕获已 Ready 的原自有段及真实 Task 快照；失败清空输出。
	 * 仅按需创建有来源的派生观察记录，不改变原执行资源或注册回调。 */
	bool CaptureOriginalStepObservation(FGGYGOPlayerComboObservation& OutObservation,
		FGGYGOMontageSectionSnapshot& OutSnapshot, FString& OutError);
	/** 只读同一原段的实时快照；结束、换段、换源及后继均拒绝，失败清空输出。
	 * 原观察的历史仍可 ReadHistory，不将历史重解释为当前播放。
	 * @param OutResourceSourceCurrent 可选原 GA 自有资源认证事实；原 Task 退役而 CMC 正在 drain 时可为 true，
	 * 与实时快照是否可读分开，不代表 CMC 已接受或完成。 */
	bool ReadOriginalStepObservation(const FGGYGOPlayerComboObservation& Observation,
		FGGYGOMontageSectionSnapshot& OutSnapshot, FString& OutError, bool* OutResourceSourceCurrent = nullptr) const;
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif

protected:
	virtual bool CanActivateAbilityAdditional(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags,
		FGameplayTagContainer* OptionalRelevantTags) const override;
	virtual void NativeOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const override;
	virtual void InitializeAbilityActivation(const FGGYGOAbilityActivationHandle& Original) override;
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context) override;
	bool IsStepPlayable(int32 Index) const;
	bool StartStep(const FGGYGOAbilityActivationHandle& Original, int32 Index, float Position = 0.0f, int32 SourceRequestId = 0,
		bool bCorrectionReplay = false, const FQuat* ReplayEntryActorRotation = nullptr,
		TSharedPtr<FStepEntryAttempt> EntryAttempt = {});
	void OpenTraceWindow(const TSharedPtr<FStepMotionResources>& Resource, const FGGYGOMontageWindowDeclaration& Declaration);
	void ReleaseTraceWindow(const FGGYGOAbilityActivationHandle& Original);
	void RejectRequest(const FGGYGOAbilityActivationHandle& Original, int32 RequestId);
	void SendAuthoritativeStep(const FGGYGOAbilityActivationHandle& Original, int32 RequestId, bool bAccepted);
	void HandleWatchdog(const FGGYGOAbilityActivationHandle& Original, uint64 ExpectedStepToken);
	bool IsActivationCurrent(const FGGYGOAbilityActivationHandle& Original) const;
	bool IsStepCurrent(const FGGYGOAbilityActivationHandle& Original, uint64 ExpectedStepToken,
		const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* ExpectedTask) const;
	bool IsMotionResourceCurrent(const TSharedPtr<FStepMotionResources>& Resource) const;
	bool InitializeStepMotion(const TSharedPtr<FStepMotionResources>& Resource);
	/** 原 press/激活身份与尚未执行的入场尝试；等待只恢复同一请求，不重评攻击门。 */
	bool IsStepEntryCurrent(const TSharedPtr<FStepEntryAttempt>& Attempt) const;
	void ResumeStepEntry(const TSharedPtr<FStepEntryAttempt>& Attempt);
	void FailStepEntry(const TSharedPtr<FStepEntryAttempt>& Attempt, const FString& Diagnostic);
	void ReleaseStepEntry(const TSharedPtr<FStepEntryAttempt>& Attempt);
	void ReleasePendingStepEntries();
	void HandleMontageSection(const TSharedPtr<FStepMotionResources>& Resource, const FGGYGOMontageSectionFact& Fact);
	void HandleMontageNotify(const TSharedPtr<FStepMotionResources>& Resource, const FGGYGOMontageNotifyFact& Fact);
	/** 只开攻击接续门；不查询、订阅或取消 CMC 移动。 */
	void OpenAttackContinuation(const TSharedPtr<FStepMotionResources>& Resource);
	/** 以移动点原实例实时快照判定开门；位置未达作者 Main 末端时保留 Pending 并返回。 */
	void TryOpenMovementInterruption(const TSharedPtr<FStepMotionResources>& Resource);
	/** 移动门已开后的唯一 CMC 消费：原认证 Intent 重查→Cancel→返栈复核→GA End。 */
	void BeginMovementInterruption(const TSharedPtr<FStepMotionResources>& Resource);
	void TryCompleteStepMotion(const TSharedPtr<FStepMotionResources>& Resource);
	void FailStepMotion(const TSharedPtr<FStepMotionResources>& Resource, const FString& Reason);
	void ReleaseStepMotion(EGGYGOActionMotionReleaseReason Reason);
	void HandleStepInputRequest(const FGGYGOAbilityActivationHandle& Original, const FGGYGOAbilityStepInputRequest& Request);
	/** 只接原 Task 已认证的 Initial 不可提供通知；已接受动作保持原取消权限。 */
	void HandleInitialInputUnavailable(const FGGYGOAbilityActivationHandle& Original,
		const UGGYGOAbilityTask_WaitComboInput* OriginalInputTask);
	void HandleMontageWindow(const TSharedPtr<FStepMotionResources>& Resource, const FGGYGOMontageWindowFact& Fact);
	void HandleMontageCompleted(const TSharedPtr<FStepMotionResources>& Resource);
	void HandleMontageInterrupted(const FGGYGOAbilityActivationHandle& Original, FGameplayTag EventTag, FGameplayEventData EventData);
	void HandleMontageBlendOut(const FGGYGOAbilityActivationHandle& Original, FGameplayTag EventTag, FGameplayEventData EventData);
	void HandleMeleeHit(const FGGYGOAbilityActivationHandle& Original, uint64 ExpectedStepToken,
		const FGGYGOMeleeTraceWindowHandle& OriginalWindow, AActor* HitActor, const FHitResult& HitResult);
	void CorrectPredictedStepForActivation(const FGGYGOAbilityActivationHandle& Original,
		int32 Revision, int32 RequestId, int32 ServerStep, float Position,
		bool bWindowOpen, bool bWindowClosed, bool bAccepted,
		int32 ServerStepRequestId = INDEX_NONE, const FQuat* EntryActorRotation = nullptr);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo")
	TArray<FGGYGOComboStep> ComboSteps;
	/** 历史缓冲配置存值；作者信号模式仅接受开门后真实新请求，不缓存或重放开门前输入。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "历史存值；作者信号模式不缓存开门前攻击。"))
	float InputBufferSeconds = 0.35f;
	/** 实际原生播放与混出预算以外的完成宽限；仅用于现有 watchdog，不决定动画混合。
	 *  原 CMC 最后区间或原 Task 完成事实缺失时，截止后明确中止原激活。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo", meta = (ClampMin = "0.0"))
	float MontageCompletionGraceSeconds = 2.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo")
	TSubclassOf<UGameplayEffect> DamageEffect;
	/** 仅DamageEffect为空时选择共享预载GE；默认false为明确无GE、仍播放命中Cue的模式。
	 *  非空DamageEffect始终优先；必需GE在准入或提交前不可用/非法时拒绝激活。
	 *  命中期间必需GE无效或载荷构造失败时，中止原激活且不发送该命中的Cue。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo")
	bool bUseSharedDamageEffectWhenUnset = false;
	/** 保留既有资产和蓝图读取引脚的历史存值，仅供作者显式迁移。
	 *  运行只读取 ComboSteps[i].HitCueTag，本字段不选择、补齐或追加命中 Cue。 */
	UPROPERTY(VisibleDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo|Migration",
		meta = (Categories = "GameplayCue.Hit", DisplayName = "历史命中 Cue（仅迁移）"))
	FGameplayTag HitCueTag;

private:
	friend struct FGGYGOPlayerComboLifecycleFixture;
	/** 验证本次选择的GE依赖，可输出同一次解析的成功结果；成功时空结果仅用于显式无GE模式。
	 *  不加载、重试、缓存或记录激活状态；失败清空输出。 */
	bool ValidateDamageEffectDependency(FString& OutError, TSubclassOf<UGameplayEffect>* OutResolvedEffect = nullptr) const;
	/** 只消费公共 Task 已认证的本段窗口；粒子完成不授予段或能力完成权限。 */
	void HandleStepFXWindow(const TSharedPtr<FStepMotionResources>& Resource,
		const FGGYGOMontageWindowFact& Fact);
	void HandleStepFXLocalStartReady(const TSharedPtr<FStepMotionResources>& Resource,
		const FGGYGOMontageFXLocalStartReadyFact& Fact);
	void ReleaseStepFX(const TSharedPtr<FStepMotionResources>& Resource, bool bNormalExit);
	void FailStepFX(const TSharedPtr<FStepMotionResources>& Resource, const FString& Reason);
	UFUNCTION()
	void HandleStepFXFailed(UGGYGOAbilityTask_PlayNiagaraEffect* Sender, const FString& Reason);

	/** 读本段原 Task 对该门名字的原实例实时点快照，并核对原 Montage/Mesh/实例/触发位置。
	 *  失败清空输出；只认原 Task 已认证的名字，不自行解析 Montage 声明。 */
	bool ReadAuthorPointSnapshot(const TSharedPtr<FStepMotionResources>& Resource, const FAuthorPointGate& Gate,
		FGGYGOMontageNotifyFact& OutFact, float& OutPositionSeconds, FString& OutError) const;
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* GetMontageTask() const;
	UGGYGOAbilityTask_PlayNiagaraEffect* GetStepFXTask() const;
	bool HasMontageCallbackRegistration() const;
	FTimerHandle GetWatchdogHandle() const;
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOAbilityTask_WaitComboInput> InputTask;
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOMeleeTraceComponent> TraceComponent;
	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> ActiveMesh;
	/** Copies of issuer-owned identity and our resources, never another activation/end state machine. */
	FGGYGOAbilityActivationHandle ResourceActivation;
	/** ASC 签发的原 Initial 运输路由；Initialize 早取，原终止清理一次归还。 */
	FGGYGOAbilityInitialInputRoute InitialInputRoute;
	/** 本段原 CMC 资源、来源及 FX 请求/退出事实；不复制引擎运行状态或时钟。 */
	TSharedPtr<FStepMotionResources> StepMotionResources;
	/** 原请求各自拥有完成订阅；门资格只取 press 一次，段退役即撤销，不作为接续缓冲。 */
	TArray<TSharedPtr<FStepEntryAttempt>> StepEntryAttempts;
	FDelegateHandle InputCallbackRegistration;
	FGGYGOMeleeTraceWindowHandle TraceWindow;
	FDelegateHandle TraceHitSubscription;
	TWeakObjectPtr<UWorld> OriginalWorld;
	FGGYGOComboWindowState Window;
	int32 CurrentStep = INDEX_NONE;
	int32 LastRequestId = 0;
	/** 本段实际起播的原输入请求序号；段号可回接，旧服务器确认按请求顺序隔离。 */
	int32 CurrentStepRequestId = 0;
	int32 StepSyncRevision = 0;
	bool bChangedMeshTick = false;
	bool bAddedMeshPrerequisite = false;
	bool bSavedUpdateRateOptimizations = false;
	uint64 StepTokenCounter = 0;
	uint64 CurrentStepToken = 0;
	EVisibilityBasedAnimTickOption SavedMeshTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
};
