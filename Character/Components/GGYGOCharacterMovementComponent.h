/**
 * @file GGYGOCharacterMovementComponent.h
 * @brief Locomotion 权威组件：步态、WalkRun 混合、起停/转身选择与原移动区间预测。
 *
 * 输入驱动的 Walk/Run 继续使用 CMC CalcVelocity；Stop 与 TurnBack 通过现有
 * FRootMotionSource_GGYGOCurve 提交 Override 速度。两条路径的曲线都由 MovementSet
 * 参数与 Animation 发布的原始源绑定纯求值，不反查 AnimInstance 当前姿态，也不建立第二个 Tick。
 * SavedMove 保存预测起点和原来源检查点，NetworkMoveData 携带该值及最小语义提示。
 * 来源检查点由原生 receipt 准入；服务端使用自己的 MovementSet 与动画源绑定
 * 重新模拟。位置校正通过自定义 response 带回权威相位与时间，
 * 未确认 move 从该基线连续重放，避免恢复旧的预测时钟。
 */
#pragma once

#include "Character/Data/GGYGOMovementTypes.h"
#include "Character/Data/GGYGOLocomotionSteeringTypes.h"
#include "Character/Data/GGYGOMotionExecutionTypes.h"
#include "Character/Data/GGYGORemoteMotionPoseTypes.h"
#include "Animation/Data/GGYGOLocomotionSourceBinding.h"
#include "Character/Components/GGYGOActionMotionExecution.h"
#include "Input/GGYGOMovementInputTypes.h"
#include "AbilitySystem/GGYGOAbilityInputRequestTypes.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Character/Components/GGYGOMovementPrediction.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Templates/SharedPointer.h"

#include "GGYGOCharacterMovementComponent.generated.h"

class AActor;
class ACharacter;
class APawn;
class AController;
class APlayerController;
class UNetConnection;
class UNetDriver;
class FSavedMove_Character;
class UGGYGOAbilitySystemComponent;
class UGGYGOMovementSet;
class UGGYGOActionMotionProfile;
class UObject;
class UGGYGOCharacterMovementComponent;
class UAnimInstance;
class USkeletalMeshComponent;
struct FRootMotionSource_GGYGOCurve;
struct FRootMotionSource_GGYGOActionCurve;
struct FGGYGOCurveRootMotionOrigin;
struct FGGYGOCurveRootMotionMoveInput;
struct FGGYGOLocomotionPreparedState;

enum class EGGYGOQualifiedMovementIntentQueryResult : uint8
{
	Unavailable = 0, Qualified, NotHeld, AwaitingPhysicalProof, WaitingForAdmission, ExecutionFailed
};

enum class EGGYGOQualifiedMovementIntentProvenance : uint8
{
	Unavailable = 0, LocalSourceHeld, AuthenticatedNativeRequest
};

/** 段首基础方向的显式来源；同段纠正只复用已接受基底，不再次读取输入。 */
enum class EGGYGOActionMotionStartFacingMode : uint8
{
	CurrentFacing,
	QualifiedInput,
	ReplayBasis
};

/** 当前原生源的 GT 只读值；不证明该源已经贡献平移或旋转。 */
enum class EGGYGOActionMotionNativeObservationStatus : uint8
{
	Unavailable, Available, NoActionSource
};

enum class EGGYGOActionMotionNativeSourceContainer : uint8
{
	Active, Pending
};

struct GGYGO_API FGGYGOActionMotionNativeSectionObservation
{
	FName Name;
	int32 Index = INDEX_NONE;
	float StartSeconds = 0.0f;
	float EndSeconds = 0.0f;
};

struct GGYGO_API FGGYGOActionMotionNativeSegmentObservation
{
	FString SequencePath;
	int32 Index = INDEX_NONE;
	float MontageStartSeconds = 0.0f;
	float MontageEndSeconds = 0.0f;
	float SequenceStartSeconds = 0.0f;
	float SequenceEndSeconds = 0.0f;
	float SourceSecondsPerMontageSecond = 0.0f;
};

/** 原 RMS 与 binding 的当栈值副本；不保活源、资产或本地 GA 资源。 */
struct GGYGO_API FGGYGOActionMotionNativeSourceObservation
{
	EGGYGOActionMotionNativeSourceContainer Container = EGGYGOActionMotionNativeSourceContainer::Active;
	FString ScriptStructPath;
	FName InstanceName;
	uint8 SourceMode = 0;
	/** 仅供本机同一观察内定位，不是跨端逻辑段身份。 */
	uint16 LocalId = 0;
	bool bNativeImported = false;
	bool bFinished = false;
	bool bMarkedForRemoval = false;
	bool bExplicitlyCancelled = false;
	bool bHasOriginalMontageBinding = false;
	bool bHasEntryActorRotation = false;
	FQuat EntryActorRotation = FQuat::Identity;
	float MontageStartSeconds = 0.0f;
	float NativeTimeSeconds = 0.0f;
	float DurationSeconds = 0.0f;
	float PlayRate = 0.0f;
	FString MontagePath;
	FName SlotName;
	/** binding 初选 Section，不冒充 native 当前所在 Section。 */
	FName BindingInitialSectionName;
	int32 TrackIndex = INDEX_NONE;
	int32 BindingInitialSectionIndex = INDEX_NONE;
	float BindingStartSeconds = 0.0f;
	float BindingEndSeconds = 0.0f;
	TArray<FGGYGOActionMotionNativeSectionObservation> Sections;
	TArray<FGGYGOActionMotionNativeSegmentObservation> Segments;
};

struct GGYGO_API FGGYGOActionMotionNativeObservation
{
	EGGYGOActionMotionNativeObservationStatus Status = EGGYGOActionMotionNativeObservationStatus::Unavailable;
	FString Diagnostic;
	uint64 Frame = 0;
	double WorldTimeSeconds = 0.0;
	FString WorldPath;
	FString CharacterPath;
	FString MovementPath;
	uint8 LocalRole = 0;
	TArray<FGGYGOActionMotionNativeSourceObservation> Sources;
};

/** Scope 属于调用方原 CMC；EntryActorRotation 仅供 ReplayBasis 消费。 */
struct GGYGO_API FGGYGOActionMotionStartFacing
{
	EGGYGOActionMotionStartFacingMode Mode = EGGYGOActionMotionStartFacingMode::CurrentFacing;
	FGGYGOMovementOwnerSyncScopeId Scope;
	FQuat EntryActorRotation = FQuat::Identity;
	/** 正常入场的原 owned 请求；CurrentFacing 也须提交，客户端数据不能签发 ReplayBasis。 */
	FGGYGOAbilityStepInputRequest InputRequest;
};

/** 同一入场栈的采样/提交凭据。不能跨帧保存或作为动作后续帧的输入许可。 */
struct GGYGO_API FGGYGOActionMotionStartInputProof
{
	FGGYGOAbilityStepInputRequest Request;
	FGGYGOMovementOwnerSyncScopeId Scope;
	FGGYGOMovementInputStepFact Fact;
	TWeakObjectPtr<ACharacter> OriginalCharacter;
	TWeakObjectPtr<APlayerController> OriginalController;
	uint64 Frame = 0;
};

struct GGYGO_API FGGYGOQualifiedMovementIntent
{
	FGGYGOMovementOwnerSyncScopeId Scope;
	EGGYGOQualifiedMovementIntentProvenance Provenance = EGGYGOQualifiedMovementIntentProvenance::Unavailable;
	uint64 BindingSerial = 0;
	uint64 SessionSerial = 0;
	uint64 RequestSerial = 0;
	uint64 ExecutionRequestSerial = 0;
	bool operator==(const FGGYGOQualifiedMovementIntent& Other) const;
};

struct GGYGO_API FGGYGOQualifiedMovementIntentObserverId
{
	bool IsSet() const { return Scope.IsSet() && ObserverSerial != 0; }
	const FGGYGOMovementOwnerSyncScopeId& GetScope() const { return Scope; }
	bool operator==(const FGGYGOQualifiedMovementIntentObserverId& Other) const
	{
		return Scope == Other.Scope && ObserverSerial == Other.ObserverSerial;
	}
private:
	friend class UGGYGOCharacterMovementComponent;
	FGGYGOMovementOwnerSyncScopeId Scope;
	uint64 ObserverSerial = 0;
};

DECLARE_DELEGATE_FourParams(FGGYGOQualifiedMovementIntentDelegate,
	const FGGYGOQualifiedMovementIntentObserverId&, EGGYGOQualifiedMovementIntentQueryResult,
	const FGGYGOQualifiedMovementIntent&, const FString&);

enum class EGGYGOMovementInitialRequestAdmissionResult : uint8
{
	Admitted, AlreadyAdmitted, Stale, Rejected, ExecutionFailed
};

/** 蓝图保存的本次转身观察值；不延长 Origin／源资产寿命，资格由原 CMC 校验。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOTurnBackRetriggerHandle
{
	GENERATED_BODY()
public:
	// 空／过期观察值相等仅为正常值相等；不代表同源或任何执行许可。
	bool operator==(const FGGYGOTurnBackRetriggerHandle& Other) const { return Origin == Other.Origin; }
private:
	friend class UGGYGOCharacterMovementComponent;
	TWeakPtr<const FGGYGOCurveRootMotionOrigin> Origin;
};

template<>
struct TStructOpsTypeTraits<FGGYGOTurnBackRetriggerHandle> : TStructOpsTypeTraitsBase2<FGGYGOTurnBackRetriggerHandle>
{
	enum { WithCopy = true, WithIdenticalViaEquality = true };
};

/** Local resource identity; only the original CMC issues and consumes this reference. */
struct FGGYGOCurveRootMotionOrigin
{
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> Owner;
	FGGYGOMovementInputConsumerBindingId Binding;
	FGGYGOMovementInputRequestIdentity InputRequest;
	uint64 ExecutionRequestSerial = 0;
	EGGYGOLocomotionMotionType MotionType = EGGYGOLocomotionMotionType::None;
	uint16 MotionSequence = 0;
	TWeakObjectPtr<const UGGYGOMovementSet> MovementSet;
	FGGYGOLocomotionSourceBindingPtr SourceBinding;
	/** 原源准入时声明，RMS 位移结束不退休旋转合同；未准入值不提供 SourceYaw 权限。 */
	EGGYGOMotionRotationMode RotationMode = EGGYGOMotionRotationMode::TranslationOnly;
	float SourceTimeOrigin = 0.0f;
	float MotionTimeOrigin = 0.0f;
	float BaseYaw = 0.0f;
	float RootMotionScale = 0.0f;
	bool bEndOnZeroSpeed = false;
private:
	friend class UGGYGOCharacterMovementComponent;
	/** 同一 Origin 的弱观察容器只创建一次；保留历史比较，不保活 Origin 或另签来源编号。 */
	mutable TSharedPtr<const TWeakPtr<const FGGYGOCurveRootMotionOrigin>> SourceProofObserver;
	/** 仅记录真实退休事实已携出，防同步重入重复派发；不参与运动准入或当前性判定。 */
	mutable bool bSourceRetirementFactIssued = false;
};

enum class EGGYGOCurveRootMotionPrepareResult : uint8
{
	Prepared, Finished, Stale, Failed, UnsupportedOrigin
};

/** Derived output for one original input and one native interval; never a clock or admission gate. */
struct FGGYGOCurveRootMotionPrepared
{
	TSharedPtr<const FGGYGOCurveRootMotionMoveInput> Input;
	TSharedPtr<const FGGYGOLocomotionPreparedState> State;
	float NativeStartTime = 0.0f;
	float NativeEndTime = 0.0f;
	float MotionStartTime = 0.0f;
	float MotionEndTime = 0.0f;
	float SimulationTime = 0.0f;
	float MovementTickTime = 0.0f;
	FGGYGOLocomotionCurveSample Sample;
	FVector OverrideVelocity = FVector::ZeroVector;
	/** 仅本原生区间实际从首段进入 RunOut；回放从实际起始 Candidate 派生。 */
	bool bEnteredTurnBackRunOut = false;
	EGGYGOCurveRootMotionPrepareResult Result = EGGYGOCurveRootMotionPrepareResult::Failed;
};
class UGGYGOPawnExtensionComponent;


/** Fact acknowledgement only; Recorded does not grant movement execution. */
enum class EGGYGOMovementInputConsumeResult : uint8
{
	Recorded,
	Duplicate,
	Stale,
	Rejected
};

UCLASS(meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	UGGYGOCharacterMovementComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	//~UCharacterMovementComponent interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void StopMovementImmediately() override;
	virtual void SetUpdatedComponent(USceneComponent* NewUpdatedComponent) override;
	virtual void OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode) override;

	/** 保留 CMC 自身 Tick，只做 ActionMotion 资源回收；Locomotion 在 move 模拟入口推进。 */
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** 按当前步态返回速度上限。被 `Restriction.CantMove` 阻断时返回 0。 */
	virtual float GetMaxSpeed() const override;

	/** 地面最低模拟速度不得抬高本模块解算出的上限，包括合法的零速度。 */
	virtual float GetMinAnalogSpeed() const override;

	/** 未准入的地面请求不消费输入/RequestedMove；原生物理结果仅继续原生制动。 */
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;

	/** native RMS 最后应用帧也须经过地面准入；独立动作/实际动画 RootMotion 保持原生执行。 */
	virtual void ApplyRootMotionToVelocity(float DeltaTime) override;

	/** PerformMovement 的直接 Override 已应用；在胶囊移动前拒绝未准入的贡献。 */
	virtual void UpdateVelocityBeforeMovement(float DeltaSeconds) override;

	/** 每次 move（含回放）前解算步态。 */
	virtual void UpdateCharacterStateBeforeMovement(float DeltaSeconds) override;

	/** 在原生物理结束处保存同一胶囊的实际速度结果，不发布输入准入或动作请求。 */
	virtual void UpdateCharacterStateAfterMovement(float DeltaSeconds) override;

	/** 从压缩标志位取回 ForceWalk 输入请求；步态与动作段由服务端按自身配置重算。 */
	virtual void UpdateFromCompressedFlags(uint8 Flags) override;

	/** 转身的曲线接管段朝向由动画曲线驱动，其余情况交回基类。 */
	virtual void PhysicsRotation(float DeltaTime) override;
	virtual FRotator ComputeOrientToMovementRotation(const FRotator& CurrentRotation, float DeltaTime, FRotator& DeltaRotation) const override;
	virtual void OnMovementUpdated(float DeltaSeconds, const FVector& OldLocation, const FVector& OldVelocity) override;
	virtual void ClientHandleMoveResponse(const FCharacterMoveResponseDataContainer& MoveResponse) override;
	virtual bool ClientUpdatePositionAfterServerUpdate() override;
	/** 原生样本呈现入口；先执行原生平滑，再只读记录该次样本键与变换供远端姿态配对。 */
	virtual void SmoothCorrection(const FVector& OldLocation, const FQuat& OldRotation, const FVector& NewLocation, const FQuat& NewRotation) override;
	virtual void ServerMove_PerformMovement(const FCharacterNetworkMoveData& MoveData) override;
	virtual void MoveAutonomous(float ClientTimeStamp, float DeltaTime, uint8 CompressedFlags, const FVector& NewAccel) override;

	/** 提供我们自己的预测数据类型。 */
	virtual FNetworkPredictionData_Client* GetPredictionData_Client() const override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~End of UCharacterMovementComponent interface

	/**
	 * 由 PawnExtension 注入移动参数，仅有效非空配置返回 true。
	 *
	 * 每次绑定请求先解除旧绑定、恢复组件基线并清理自有 Locomotion。
	 * 非空无效配置保留 ValidateMovementSet 原因并输出拒绝诊断。
	 * nullptr 是合法解绑，返回 false、清空 OutError，不输出错误。
	 * 成功后原样应用已校验参数并清空 OutError，不影响 GA ActionMotion 所有权。
	 * 绑定期移动参数须保持只读，变更须重绑；动画来源随其原生命周期独立发布。
	 * 未绑定有效配置或曲线模式缺有效源时普通地面执行被拒绝。
	 */
	bool SetMovementSet(const UGGYGOMovementSet* InMovementSet, FString* OutError = nullptr);

	/** Animation's original lifecycle publication. Does not grant an input request or start a move. */
	bool PublishLocomotionSourceBinding(const FGGYGOLocomotionSourceBinding& Binding, FString& OutError);
	void RetireLocomotionSourceBinding(UAnimInstance* OriginalProducer, uint64 OriginalConfigurationGeneration);

	bool GetMovementOwnerSyncScope(FGGYGOMovementOwnerSyncScopeId& OutScope, FString& OutError) const;
	/** 先安装原记录并写 OutObserver，再同步回放；须直接传长期原 Scope 成员。 */
	bool SubscribeMovementOwnerSync(const FGGYGOMovementOwnerSyncScopeId& OriginalScope,
		FGGYGOMovementOwnerSyncDelegate Observer, FGGYGOMovementOwnerSyncObserverId& OutObserver, FString& OutError);
	bool UnsubscribeMovementOwnerSync(const FGGYGOMovementOwnerSyncObserverId& OriginalObserver,
		FName Reason, FString& OutError);
	/** M3a 原首次 Waiting 的一次准入；调用方先核对 Source 真实 Held，Ready 不重发 Started／执行号。 */
	EGGYGOMovementInitialRequestAdmissionResult TryAdmitInitialMovementInputRequest(
		const FGGYGOMovementOwnerSyncObserverId& OriginalObserver,
		const FGGYGOMovementInputConsumerBindingId& OriginalBinding,
		const FGGYGOMovementInputRequestIdentity& OriginalSourceRequest,
		const FGGYGOMovementOwnerSyncNotice& OriginalReadyNotice, FString& OutError);

	/** CMC lifetime serial; initialization captures this before registering a receiver. */
	uint64 GetMovementInputBindingSerial() const { return MovementInputBindingSerial; }
	bool BindMovementInputSession(const FGGYGOMovementInputSessionIdentity& Session,
		uint64 ExpectedConsumerBindingSerial,
		FGGYGOMovementInputConsumerBindingId& OutBinding, FString& OutError);
	EGGYGOMovementInputConsumeResult ConsumeMovementInputFact(
		const FGGYGOMovementInputConsumerBindingId& Binding,
		const FGGYGOMovementInputFact& Fact, FString& OutError);

	/** Receiver-owned teardown, including a failed Attach; does not assert physical release. */
	bool InvalidateMovementInputSession(const FGGYGOMovementInputConsumerBindingId& Binding,
		FName Reason, FString& OutError);

	/** Authority 为原角色取得独占 Flying 模式。句柄跨动画段由调用方持有，模式被外部接管即退役。 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "GGYGO|Movement|Action Motion")
	bool AcquireFlyingMovementMode(ACharacter* OriginalCharacter, int32& OutHandle, FString& OutError);

	/** 先结束/取消原动作再归还模式。bEnterFalling 显式进入 Falling，否则恢复原模式，原地面模式须有真实可走地面。
	 *  已退役的原句柄可重复归还；归还不改变后继模式，不证明已经落地。 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "GGYGO|Movement|Action Motion")
	bool ReleaseFlyingMovementMode(int32 OriginalHandle, FString& OutError, bool bEnterFalling = false);

	/** Authority-only ground action. Returns an owner token, or INDEX_NONE; rejects overlapping actions. */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Movement|Action Motion")
	int32 BeginActionMotion(const UGGYGOActionMotionProfile* Profile, float PlayRate = 1.f);

	/** Only the current token can remove its source. Safe after natural completion or repeated cleanup. */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Movement|Action Motion")
	void EndActionMotion(int32 Handle);

	/**
	 * 原 Montage 完整区间（可从 End 段开始）的位移；碰撞与落地由原生物理负责。
	 * 真正新段可采当前有效输入；纠正以原基底恢复，返回实际接受的整段基础方向。
	 * 不需要朝向策略的调用方传默认 Facing（CurrentFacing）。
	 */
	bool BeginMontageActionMotion(const FGGYGOActionMotionSourceBindingPtr& OriginalSource,
		int32 OriginalMontageInstanceId, float MontagePositionSeconds, float EffectiveMontagePlayRate,
		float TranslationScale, const FGGYGOActionMotionStartFacing& Facing,
		int32& OutHandle, FQuat& OutEntryActorRotation, FString& OutError);
	/** 只采样/提交原 owned 请求，不安装 RMS 或更换动作；供原 gate 拒绝的真实 attempt 上报。 */
	EGGYGOMovementInputEvaluationState PrepareActionMotionStartInput(const FGGYGOActionMotionStartFacing& Facing,
		FGGYGOActionMotionStartInputProof& OutProof, FString& OutError) const;
	/** 同栈外调返栈后核原来源/评价/Yaw，绝不重采或改写已提交事实。 */
	bool ValidateActionMotionStartInput(const FGGYGOActionMotionStartInputProof& Proof, FString& OutError) const;
	/** 同段权威纠偏只重基原 RMS，不重建播放、时间或窗口；仅对齐已消费的源 Yaw。 */
	bool RebaseMontageActionMotion(const FGGYGOMovementOwnerSyncScopeId& OriginalScope,
		int32 OriginalHandle, const FQuat& EntryActorRotation, FString& OutError);
	/** Completed accepts the original Task's natural completion and drains the finite native tail.
	 *  Acceptance is not motion completion; observe the consumed final interval separately. */
	bool ReleaseMontageActionMotion(int32 OriginalHandle, EGGYGOActionMotionReleaseReason Reason, FString& OutError);
	bool CancelMontageActionMotionForMovement(int32 OriginalHandle,
		const FGGYGOQualifiedMovementIntent& OriginalIntent, FString& OutError);
	/** Sole original action failure recipient for either explicit Profile or Montage mode. */
	bool ObserveActionMotionFailure(int32 OriginalHandle,
		FGGYGOActionMotionFailureDelegate Callback, FString& OutError);
	/** Montage-specific compatibility entry; still rejects Profile handles. */
	bool ObserveMontageActionMotionFailure(int32 OriginalHandle,
		FGGYGOActionMotionFailureDelegate Callback, FString& OutError);
	/** Sole original recipient, called after native physics consumes the final interval; exact late replay is allowed. */
	bool ObserveMontageActionMotionCompletion(int32 OriginalHandle,
		FGGYGOActionMotionCompletionDelegate Callback, FString& OutError);
	/** Authenticated original Task non-interrupted BlendOut fact; permits only its configured finite tail. */
	bool NotifyMontageActionNaturalBlendOut(int32 OriginalHandle, int32 OriginalMontageInstanceId, FString& OutError);
	EGGYGOQualifiedMovementIntentQueryResult QueryQualifiedMovementIntent(
		const FGGYGOMovementOwnerSyncScopeId& OriginalScope,
		FGGYGOQualifiedMovementIntent& OutIntent, FString& OutError) const;
	/** Writes the original handle before synchronous replay; replay may retire it. */
	bool SubscribeQualifiedMovementIntent(const FGGYGOMovementOwnerSyncScopeId& OriginalScope,
		FGGYGOQualifiedMovementIntentDelegate Observer,
		FGGYGOQualifiedMovementIntentObserverId& OutObserver, FString& OutError);
	bool UnsubscribeQualifiedMovementIntent(const FGGYGOQualifiedMovementIntentObserverId& OriginalObserver,
		FName Reason, FString& OutError);

	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement|Action Motion")
	bool HasActiveActionMotion() const;

	/** 显式强制步行优先于自动升 Run 与下一次移动直接 Run。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Movement")
	void SetForceWalkRequested(bool bRequested);

	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	bool IsForceWalkRequested() const { return bForceWalkRequested; }

	/** 已接受的移动参数；未绑定或绑定被拒绝时为 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	const UGGYGOMovementSet* GetMovementSet() const { return MovementSet; }

	/** 本帧解算出的步态。动画层读它决定走跑混合。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	EGGYGOGait GetResolvedGait() const { return ResolvedGait; }

	/** WalkRun BlendSpace1D 的唯一混合输入：0=Walk，1=Run。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	float GetWalkRunBlendAlpha() const { return WalkRunBlendAlpha; }

	/** GT-only, original native interval qualification and lifecycle validation owned here. */
	FGGYGOLocomotionSteeringSnapshot GetLocomotionSteeringSnapshot() const;
	/** 原 Hero 在 AddMovementInput 后记录实际使用的投影基底；只供表现，不签发移动许可。 */
	bool RecordLocomotionInputProjection(const FGGYGOMovementInputConsumerBindingId& OriginalBinding,
		const FGGYGOMovementInputRequestIdentity& OriginalRequest,
		const FGGYGOMovementInputEvaluationToken& OriginalEvaluation, float ProjectionControlYaw, FString& OutError);
	/** GT-only，原生平移／旋转结束后的完成区间；不是当前 pose 或执行许可。 */
	FGGYGOMotionExecutionSnapshot GetMotionExecutionSnapshot() const;
	/** GT 当栈读取 active/pending action RMS 原值，含 SimulatedProxy imported 源；不授予执行权。 */
	FGGYGOActionMotionNativeObservation GetActionMotionNativeObservation() const;
	/**
	 * GT：对照原真实完成区间签出来源观察值，不签发运动或播放权。
	 * @param Interval 原 Character 移动回调中取得的完整单 Sequence 区间。
	 * @param OutProof 成功时原来源的弱观察值；失败清空。
	 * @param OutError 拒绝原因；成功清空。
	 * @return 该值仍是本 issuer 可认证的原完成区间。
	 */
	bool TryGetLocomotionSourceProof(const FGGYGOMotionExecutionSnapshot& Interval,
		FGGYGOLocomotionSourceProof& OutProof, FString& OutError) const;
	/**
	 * GT：原 Origin／请求／段／绑定与 owner 当前性，不以完成帧期限替代来源寿命。
	 * @param Proof 已由原 issuer 签出的观察值；历史相等不提供当前资格。
	 * @param OutError 原身份、生命周期、角色或配置失效的原因。
	 * @return 原来源仍属于本机当前语义段；RMS 归还后的同 Origin 尾段仍可成立。
	 */
	bool IsLocomotionSourceProofCurrent(const FGGYGOLocomotionSourceProof& Proof, FString& OutError) const;
	/**
	 * GT：认证原完成事件的 World 采样，不认证 cue 内部连续或亚帧映射。
	 * @param Proof 与该原区间同源的观察值。
	 * @param CompletedIntervalSerial 原完成区间编号，不能用末端秒数替代。
	 * @param CompletedFrame 原本机 GFrameCounter 域的生产帧，不是网络样本键。
	 * @param OutWorld 原生产 World；失败为 nullptr。
	 * @param OutWorldTimeSeconds 原生产事件采样；失败清零但不代表可用 Age0。
	 * @param OutError 认证失败原因。
	 * @return 原 World／完成序号／帧及采样均可认证；接收端不得补 Now。
	 */
	bool TryGetLocomotionCompletionWorldTime(const FGGYGOLocomotionSourceProof& Proof,
		uint64 CompletedIntervalSerial, uint64 CompletedFrame, UWorld*& OutWorld,
		double& OutWorldTimeSeconds, FString& OutError) const;
	/** 原订阅者保存标准 FDelegateHandle，并向同一原 CMC 退订；回调不得重取后继作为关闭对象。 */
	FGGYGOLocomotionSourceRetiredDelegate& OnLocomotionSourceRetired() { return LocomotionSourceRetired; }
	/**
	 * GT-only，远端观察端的权威完成区间姿态：SimulatedProxy，或 listen server 上由远端客户端控制的角色。
	 * 其它角色返回 NotRemote，调用方继续使用 GetMotionExecutionSnapshot。
	 * Current：可进入段并使用 SequenceSeconds；Held：只可保持已进入同段的 CertifiedSequenceSeconds；
	 * bEnded 只来自原片自然末端，终点姿态不授予播放许可。listen server 调用方须取快照相位，
	 * 本 CMC 的实时相位 getter 仍是权威执行值。不执行位移或 Yaw，不推进时间。
	 */
	FGGYGORemoteMotionPoseSnapshot GetRemoteMotionPoseSnapshot() const;

	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	EGGYGOStopMotionType GetStopMotionType() const { return StopMotionType; }

	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	EGGYGOLocomotionMotionType GetLocomotionMotionType() const { return LocomotionMotionType; }

	/** 本帧的曲线运动量。位移与方向分量处于动画段起点坐标系，轴序为 UE 局部空间（X 前、Y 右）。 */
	const FGGYGOLocomotionCurveSample& GetCurveMotion() const { return CurveMotion; }

	/**
	 * 是否有曲线位移源在场（刹停或转身）。
	 *
	 * 为 true 表示这一帧的速度完全由曲线决定，`CalcVelocity` 被引擎跳过，
	 * 玩家输入不参与位移。
	 */
	bool HasCurveRootMotionSource() const;

	/** 成功的原动画源样本是否提供速度来源；合法零速/零Scale仍有资格，活动Action不参与。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	bool IsCurveDrivingSpeed() const;

	// ===== 急停转身 =====

	/**
	 * 当前转身相位。
	 *
	 * 本地执行端返回本 CMC 解算的相位。模拟代理返回已接受远端姿态记录中的权威完成区间相位，
	 * 与 GetRemoteMotionPoseSnapshot 的姿态出自同一条记录；尚无记录或不在单 Sequence 段时为 `None`。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|TurnBack")
	EGGYGOTurnBackPhase GetTurnBackPhase() const;

	/** 转身是否已进入交还输入的 `RunOut` 段；模拟代理同样读取已接受远端记录的相位。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|TurnBack")
	bool IsTurnBackRunOut() const;

	/** 在本次转身入场后保存 handle；后续通知须携带此值，不能在通知时重取后继身份。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|TurnBack")
	bool TryGetTurnBackRetriggerHandle(FGGYGOTurnBackRetriggerHandle& OutHandle) const;

	/**
	 * 仅开放 handle 所属转身的再次触发资格，不切相位。旧段、模拟代理及历史回放拒绝。
	 * 作用于调用端；Authority／Autonomous 须各自由同源作者信号调用，此入口不向服务器转发。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|TurnBack")
	bool OpenTurnBackRetrigger(const FGGYGOTurnBackRetriggerHandle& Handle);

	/**
	 * 请求下一次移动直接进入 Run，跳过走跑计时。
	 *
	 * 为后续 GAS 动作收尾预留；当前 GA_Dodge 仍为占位，没有接入调用方。
	 * 后续闪避正常结束且仍有移动输入时可调用，跳过 WalkToRunHoldSeconds 等待。
	 *
	 * 契约只消费一次。若闪避后玩家松手了，契约在下一次"无移动输入"帧被丢弃。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Movement")
	void RequestRunOnNextMove();

	/** Native CurveRMS callback; the original resource and group must authenticate this interval. */
	EGGYGOCurveRootMotionPrepareResult PrepareLocomotionCurveRootMotion(
		const FRootMotionSource_GGYGOCurve& Source, float SimulationTime, float MovementTickTime,
		TSharedPtr<const FGGYGOCurveRootMotionPrepared>& OutPrepared, FString& OutError);
	bool ReportLocomotionCurveRootMotionFailure(
		const TSharedPtr<const FGGYGOCurveRootMotionOrigin>& Origin, const FString& Reason);

	/** 是否正被 `Restriction.CantMove` 或 `State.SwitchReturnPending` 禁止 locomotion 移动。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	bool IsMovementBlockedByTag() const;

	/**
	 * 本帧是否有移动意图。
	 *
	 * 动画层用它决定进出移动状态。用意图而不是实际速度，是因为起步第一帧
	 * 速度还是 0，按速度判定会让起步动画晚一帧，玩家能感觉到输入迟滞。
	 *
	 * 松手当帧即返回 false，没有防抖窗口。快速点按方向键会让动画在
	 * 起步与停止之间抖动，抑制它需要输入层持有一个短的方向保持窗口。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	bool HasMoveInput() const;

	// ===== 供动画层读取的派生量 =====
	// 做成即时计算的 getter 而不是每帧缓存，是因为它们全都是 Velocity 的纯函数，
	// 缓存只会多出一份可能与 Velocity 不同步的状态。

	/** 水平速度大小（cm/s）。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	float GetHorizontalSpeed() const;

	/** 是否正在移动。阈值 10 cm/s，用于过滤碰撞挤压等微小残余速度。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	bool IsMovingHorizontally() const;

	/** 水平速度的世界单位方向。静止时返回零向量。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	FVector GetHorizontalVelocityDirection() const;

	/**
	 * 水平速度相对角色朝向的角度（度）：0 为正前，+90 为正右。
	 *
	 * 静止时返回 0。注意这与 `GetHorizontalVelocityDirection` 不同，
	 * 后者是世界空间，本函数是角色局部空间 —— 动画 BlendSpace 要的是后者。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	float GetLocalVelocityAngle() const;

	/** 标准 UE 局部速度轴，均已归一化：OutForward=X 前，OutRight=Y 右。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement")
	void GetLocalVelocityAxes(float& OutForward, float& OutRight) const;

	/** 已有蓝图的兼容包装；新代码使用标准轴接口。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Movement", meta = (DeprecatedFunction, DeprecationMessage = "Use GetLocalVelocityAxes; presentation axis mapping belongs to Animation."))
	void GetLocalVelocityBlend(float& OutBlendX, float& OutBlendY) const;

private:
	/** Authority CMC 唯一模式借用。保存原模式与角色直至精确归还或原生模式/组件退役，句柄不复用。 */
	int32 LastFlyingMovementModeHandle = 0;
	int32 ActiveFlyingMovementModeHandle = INDEX_NONE;
	EMovementMode PreviousFlyingMovementMode = MOVE_None;
	TWeakObjectPtr<ACharacter> FlyingMovementModeCharacter;
	void RetireFlyingMovementMode();

	struct FMovementOwnerSyncObserverRecord
	{
		FGGYGOMovementOwnerSyncObserverId Id;
		FGGYGOMovementOwnerSyncDelegate Callback;
		bool bClosed = false;
	};
	struct FMovementOwnerSyncContext
	{
		FGGYGOMovementOwnerSyncScopeId Scope;
		FGGYGOMovementOwnerSyncNotice Notice;
		TWeakObjectPtr<UNetConnection> Connection;
		TWeakObjectPtr<UNetDriver> NetDriver;
		TMap<uint64, TSharedPtr<FMovementOwnerSyncObserverRecord>> Observers;
		uint64 ObservedBindingSerial = 0;
		uint64 ExpectedServerGeneration = 0;
		bool bConnectionCaptured = false;
		bool bOwnerPairCaptured = false;
		bool bObservedBindingActive = false;
		bool bRequiresNativeResponse = false;
		bool bInitialLocalScope = false;
		bool bRetired = false;
	};
	/**
	 * Stack-owned publication resource. Input: the exact original Context/records;
	 * output: cancellable pending callbacks, never a gameplay state or notice queue.
	 * Nested frames borrow Previous only until return. Terminal cleanup seals pending
	 * records on every exit, unlinks this frame before releasing callback captures,
	 * and retains no historical lookup. EndPlay detaches the active chain.
	 */
	struct FMovementOwnerSyncDispatchFrame
	{
		FMovementOwnerSyncDispatchFrame(UGGYGOCharacterMovementComponent* InOwner,
			const TSharedPtr<FMovementOwnerSyncContext>& InContext, bool bInTerminal);
		~FMovementOwnerSyncDispatchFrame();
		FMovementOwnerSyncDispatchFrame(const FMovementOwnerSyncDispatchFrame&) = delete;
		FMovementOwnerSyncDispatchFrame& operator=(const FMovementOwnerSyncDispatchFrame&) = delete;
		void ClosePendingRecords();
		TWeakObjectPtr<UGGYGOCharacterMovementComponent> Owner;
		TSharedPtr<FMovementOwnerSyncContext> Context;
		TArray<TSharedPtr<FMovementOwnerSyncObserverRecord>> Records;
		FMovementOwnerSyncDispatchFrame* Previous = nullptr;
		uint64 OwnerLifetimeSerial = 0;
		bool bTerminal = false;
		bool bClosing = false;
	};
	FMovementOwnerSyncDispatchFrame* ActiveMovementOwnerSyncDispatch = nullptr;
	/** 单个原 PendingAdjustment 的来源附记；不是队列、执行器或第二输入状态。 */
	struct FMovementOwnerSyncNativeReceipt
	{
		TWeakObjectPtr<APawn> Pawn;
		TWeakObjectPtr<APlayerController> Controller;
		TWeakObjectPtr<UNetConnection> Connection;
		uint64 OwnerGeneration = 0;
		uint64 ClientOwnerGeneration = 0;
		uint64 Nonce = 0;
		float TimeStamp = 0.0f;
		bool bInitialOwnerGeneration = false;
	};
	struct FMovementOwnerSyncNativeMove
	{
		const FCharacterNetworkMoveData_GGYGO* Move = nullptr;
		FGGYGOMovementInputSourceCheckpoint OriginalSourceCheckpoint;
		uint64 OriginalNonce = 0;
		uint64 OriginalClientGeneration = 0;
		float OriginalTimeStamp = 0.0f;
		uint8 OriginalFlags = 0;
		bool bOriginalHasCameraProjection = false;
		float OriginalCameraProjectionYaw = 0.0f;
		bool bHasOriginalSync = false;
		bool bOriginalNewMove = false;
		FMovementOwnerSyncNativeReceipt Receipt;
		bool bEnteredNativeSimulation = false;
		/** Applies to this original native interval only; never changes request admission. */
		bool bSourceExecutionApplicable = false;
	};
	/** Borrowed authenticated original source of the sole CMC request; no Producer/Binding is minted. */
	struct FMovementInputNativeSource
	{
		FMovementOwnerSyncNativeReceipt Receipt;
		FGGYGOMovementOwnerSyncScopeId OwnerScope;
		FGGYGOMovementInputSourceCheckpoint StartCheckpoint;
		uint64 ExecutionRequestSerial = 0;
	};
	TSharedPtr<const FMovementInputNativeSource> MovementInputNativeSource;
	bool ResolveActionMotionStartFacing(const FGGYGOActionMotionStartFacing& Facing,
		FQuat& OutEntryActorRotation, FGGYGOActionMotionStartInputProof& OutInputProof, FString& OutError) const;
	/** Consume authenticated original Source values into the sole CMC request; no physical observation. */
	bool ConsumeNativeMovementInputCheckpoint(const FMovementOwnerSyncNativeMove& OriginalMove);
	UFUNCTION()
	void HandleMovementOwnerControllerChanged(APawn* Pawn, AController* OldController, AController* NewController);
	/** One M2 lifetime, initialized by native BeginPlay or the first live Source binding. */
	bool EnsureMovementOwnerSyncLifetime();
	void RefreshMovementOwnerSyncContext();
	void OpenMovementOwnerSyncScope(APawn* Pawn, APlayerController* Controller, UNetConnection* Connection,
		UNetDriver* Driver, bool bInitialLocalScope, uint64 ExpectedServerGeneration = 0);
	void RetireMovementOwnerSyncScope(FName Reason);
	void RetireServerMovementOwner(FName Reason);
	bool IsMovementOwnerSyncContextCurrent(const TSharedPtr<FMovementOwnerSyncContext>& Context) const;
	void PublishMovementOwnerSyncNotice(const TSharedPtr<FMovementOwnerSyncContext>& Context,
		EGGYGOMovementOwnerSyncState State, uint64 Generation, uint64 Nonce, bool bInitialEligible, FName Reason);
	bool IsMovementOwnerSyncReceiptCurrent(const FMovementOwnerSyncNativeReceipt& Receipt) const;
	void ReportMovementOwnerSyncOnce(FName Reason, const FString& Detail);
	TSharedPtr<FMovementOwnerSyncContext> MovementOwnerSyncContext;
	TWeakObjectPtr<APawn> MovementOwnerObservedPawn;
	TWeakObjectPtr<APawn> ServerMovementOwnerPawn;
	TWeakObjectPtr<APlayerController> ServerMovementOwnerController;
	TWeakObjectPtr<UNetConnection> ServerMovementOwnerConnection;
	TWeakObjectPtr<UNetDriver> ServerMovementOwnerNetDriver;
	FMovementOwnerSyncNativeReceipt MovementOwnerSyncPendingReceipt;
	/** 栈内原调用标记；客户端 replay 的 CurrentNetworkMoveData 不建立此标记。 */
	FMovementOwnerSyncNativeMove* ActiveMovementOwnerSyncNativeMove = nullptr;
	TSet<FName> MovementOwnerSyncReportedReasons;
	uint64 MovementOwnerSyncLifetimeSerial = 0;
	uint64 MovementOwnerSyncLastObserverSerial = 0;
	uint64 MovementOwnerSyncLastNoticeSerial = 0;
	uint64 ServerMovementOwnerGeneration = 0;
	bool bServerMovementOwnerActive = false;
	bool bServerMovementInitialOwnerGeneration = false;
	bool bMovementOwnerSyncEverOpened = false;
	bool bServerMovementOwnerEverOpened = false;
	bool bMovementOwnerSyncClosed = false;

	enum class ELocomotionRequestAdmission : uint8
	{
		None, Admitted, Released, Revoked, Failed, Waiting
	};

	bool IsMovementInputBindingCurrent(const FGGYGOMovementInputConsumerBindingId& Binding) const;
	/** Historical value capture only, after the existing consumer has returned Recorded. */
	void RecordMovementInputSourceCheckpoint(const FGGYGOMovementInputFact& Fact);
	bool IsMovementInputRequestBlocked() const;
	/** No source request is invented for an unmigrated local curve execution. */
	bool ShouldRejectUnownedCurveGroundLocomotion() const;
	bool ShouldRejectMovementInputGroundLocomotion() const;
	/** Same-entry request facts; native passive momentum is supplied by the caller. */
	bool HasGroundMovementRequest(bool bNativeMomentum, const FRootMotionSource* UnsupportedSource) const;
	void EnforceMovementInputLocomotionAdmission(const TCHAR* Entry);
	void RevokeMovementInputRequest();
	void CancelMovementInputLocomotion();
	/** Only the current execution request may fail; failure is cleared only by a new RequestStarted. */
	bool FailLocomotionRequest(uint64 ExpectedRequestSerial, const FString& Reason);
	bool RejectLocomotionEvaluation(uint64 ExpectedRequestSerial, const FString& Reason);

	/** Proposal for the existing fields; saved copies are original move inputs or derived results. */
	struct FLocomotionUpdateCandidate
	{
		EGGYGOGait Gait;
		float WalkHoldSeconds;
		bool bWantsRun;
		bool bPreviousInput;
		bool bPreviousBlocked;
		EGGYGOLocomotionMotionType MotionType;
		EGGYGOStopMotionType StopType;
		float MotionTime;
		float CyclePhase;
		uint16 MotionSequence;
		float BlendAlpha;
		EGGYGOTurnBackPhase TurnPhase;
		float TurnElapsed;
		float TurnEntryYaw;
		bool bTurnInputLatched;
		FGGYGOLocomotionCurveSample Motion;
	};

	FLocomotionUpdateCandidate CaptureLocomotionCandidate() const;
	void CommitLocomotionCandidate(const FLocomotionUpdateCandidate& Candidate);
	bool TryUpdateLocomotion(float DeltaSeconds, bool bResolveFrameGait,
		bool bHadMoveInput, EGGYGOGait PreviousGait);
	void ResolveGait(float DeltaSeconds, FLocomotionUpdateCandidate& Candidate) const;
	void UpdateWalkHoldTimer(EGGYGOGait FrameGait, bool bHasMoveInput, bool bBlocked,
		bool bOnGround, bool bMoveInputRising, bool bBlockReleased, float DeltaSeconds, float& Timer) const;
	void SetLocomotionMotion(EGGYGOLocomotionMotionType NewType, FLocomotionUpdateCandidate& Candidate) const;
	bool UpdateWalkRunBlend(float DeltaSeconds, FLocomotionUpdateCandidate& Candidate, FString& OutError) const;
	bool EvaluateLocomotionSource(float DeltaSeconds, FLocomotionUpdateCandidate& Candidate, FString& OutError);
	bool EvaluateWalkRunSource(float DeltaSeconds, FLocomotionUpdateCandidate& Candidate, FString& OutError);
	void BeginMotionExecutionInterval();
	void PrepareOrdinaryMotionExecution(const FLocomotionUpdateCandidate& Candidate,
		const FGGYGOLocomotionSourceBindingPtr& Binding, float StartTime, float EndTime,
		float StartCycle = 0.0f, float EndCycle = 0.0f);
	bool PrepareNativeMotionExecutionRotation();
	bool ApplyOrdinaryMotionCurveYaw();
	/** RunOut 切段对齐当前位移，后续按既有角速度准备源旋转基准。 */
	bool PrepareTurnBackRunOutFacing(float DeltaSeconds);
	void CompleteMotionExecutionInterval(float DeltaSeconds, const FVector& OldLocation);
	FGGYGOLocomotionSourceProof MakeLocomotionSourceProof(const TSharedPtr<const FGGYGOCurveRootMotionOrigin>& Origin) const;
	bool IsLocomotionSourceProofContextCurrent(const FGGYGOLocomotionSourceProof& Proof,
		const TSharedPtr<const FGGYGOCurveRootMotionOrigin>& Origin) const;
	void QueueLocomotionSourceRetirement(const TSharedPtr<const FGGYGOCurveRootMotionOrigin>& Origin,
		EGGYGOMotionExecutionEndReason EndReason, FName Cause, const FString& Diagnostic = FString(),
		bool bLifecycleRetirement = false);
	void QueueCurrentLocomotionSourceRetirement(EGGYGOMotionExecutionEndReason EndReason,
		FName Cause, const FString& Diagnostic = FString(), bool bLifecycleRetirement = false);
	void DispatchLocomotionSourceRetirements(bool bNativeSafeExit = false);
	struct FLocomotionSourceRetirementScope;
	bool GetLocomotionSourceBinding(FGGYGOLocomotionSourceBindingPtr& OutBinding, FString& OutError,
		bool bValidateSourceConfiguration = true) const;
	bool IsLocomotionSourceBindingCurrent(const FGGYGOLocomotionSourceBindingPtr& Binding, FString& OutError,
		bool bValidateSourceConfiguration = true) const;
	bool IsCurrentMotionFinished(const FLocomotionUpdateCandidate& Candidate,
		bool& bOutFinished, FString& OutError) const;
	void BeginTurnBack(FLocomotionUpdateCandidate& Candidate) const;
	void AdvanceTurnBackPhase(float DeltaSeconds, FLocomotionUpdateCandidate& Candidate, bool bNativeInterval = false) const;
	void ResetTurnBack(FLocomotionUpdateCandidate& Candidate) const;
	bool IsReverseRunInput(EGGYGOGait Gait) const;

	bool StageLocomotionCurveRootMotion(float MovementTickTime,
		const FLocomotionUpdateCandidate& Candidate, FString& OutError);
	bool BeginLocomotionCurveReplay(const FSavedMove_GGYGO& Move, FString& OutError);
	void FinishLocomotionCurveReplayPreparation();
	void EndLocomotionCurveReplay();
	void RetireLocomotionCurveRootMotion();
	/** Returns true only when this original interval was consumed, including an idempotent repeat. */
	bool ConsumeLocomotionCurvePrepared(const TSharedPtr<const FGGYGOCurveRootMotionPrepared>& Prepared);
	bool HasCurrentLocomotionCurveOrigin(const TSharedPtr<const FGGYGOCurveRootMotionOrigin>& Origin) const;
	bool IsLocomotionCurveOriginForCandidate(const TSharedPtr<const FGGYGOCurveRootMotionOrigin>& Origin,
		const FLocomotionUpdateCandidate& Candidate, const FGGYGOLocomotionSourceBindingPtr& SourceBinding) const;
	/** 当前地面原源已挂接但未消费，只保留其旋转权，不代表已执行。 */
	bool HasPendingLocomotionCurveSource() const;

	/** Actual local resource ownership and bounded saved-move context; no held/FAILED/time authority. */
	TSharedPtr<const FGGYGOCurveRootMotionOrigin> LocomotionCurveOrigin;
	TSharedPtr<const FGGYGOCurveRootMotionOrigin> CompletedLocomotionCurveOrigin;
	TSharedPtr<const FGGYGOCurveRootMotionMoveInput> PendingLocomotionCurveInput;
	TSharedPtr<const FGGYGOCurveRootMotionPrepared> LastLocomotionCurvePrepared;
	TSharedPtr<const FGGYGOCurveRootMotionPrepared> ConsumedLocomotionCurvePrepared;
	TSharedPtr<const FGGYGOCurveRootMotionMoveInput> ReplayLocomotionCurveInput;
	TSharedPtr<const FGGYGOCurveRootMotionPrepared> ReplayLocomotionCurvePrepared;
	FGGYGOLocomotionSourceBindingPtr LocomotionSourceBinding;
	FGGYGOLocomotionSourceBindingPtr ReplayLocomotionSourceBinding;
	TWeakObjectPtr<UAnimInstance> LastLocomotionSourceProducer;
	uint64 LastLocomotionSourceConfigurationGeneration = 0;
	bool bReplayPreparedFromAuthority = false;
	const FRootMotionSourceGroup* PreparingLocomotionCurveReplayGroup = nullptr;
	bool bLocomotionCurveReplayRejected = false;
	TOptional<FLocomotionUpdateCandidate> LocomotionCurveReplayEntryState;
	uint64 LocomotionCurveReplayEntryRequestSerial = 0;
	/** Bounded original move capture, installed before native Prep and discarded at replay return. */
	struct FMovementInputReplayCapture
	{
		FSavedMove_GGYGO::FMovementInputRequestCapture Request;
		FGGYGOMovementOwnerSyncScopeId OwnerScope;
		uint64 OwnerGeneration = 0;
		FGGYGOMovementInputSourceCheckpoint Checkpoint;
	};
	TOptional<FMovementInputReplayCapture> MovementInputReplayCapture;

	friend struct FGGYGOCurveRootMotionMoveInput;
	friend struct FGGYGOLocomotionPreparedState;

	// Receiver authority and read-only source provenance; never reset by configuration/ASC resets.
	uint64 MovementInputBindingSerial = 0;
	FGGYGOMovementInputConsumerBindingId MovementInputBinding;
	bool bMovementInputBindingActive = false;
	bool bMovementInputSessionOpened = false;
	/** Consumed session value and one-start window only; Input retains physical qualification. */
	EGGYGOMovementInputSessionMode ConsumedMovementInputSessionMode = EGGYGOMovementInputSessionMode::Invalid;
	bool bMovementInputColdStartWindowOpen = false;
	/** Consumed proof stage only; no key/axis observation or physical state is inferred here. */
	bool bMovementInputNeutralConsumed = false;
	bool bMovementInputRequestOpen = false;
	FGGYGOMovementInputFact LastMovementInputFact;
	/** Derived original-fact values; local admission never reads them, native consumption validates them before use. */
	FGGYGOMovementInputSourceCheckpoint MovementInputSourceCheckpoint;
	uint64 LastMovementInputRequestSerial = 0;
	FGGYGOMovementInputRequestIdentity MovementInputRequest;
	/** Captured by the original Started only; borrows M2 identity, never issues an owner generation or Held fact. */
	FGGYGOMovementOwnerSyncScopeId MovementInputRequestOwnerScope;
	uint64 LocomotionRequestSerial = 0;
	ELocomotionRequestAdmission LocomotionRequestAdmission = ELocomotionRequestAdmission::None;
	FString LocomotionRequestFailureReason;
	bool bMovementInputAdmissionDiagnosticReported = false;

	/**
	 * Derived native physics result, never a request, speed source or executor. Captured after
	 * admitted CMC/action/native mode physics, before external movement-update delegates, or
	 * an actually accepted native correction. Only the same character/component and unchanged
	 * incoming ground velocity may continue it. Before/After bound the interval; an explicit
	 * stop, component/owner change or EndPlay invalidates it. This completed-result cache
	 * never restores velocity; the pre-root-motion snapshot below belongs only to the current interval.
	 */
	struct FNativeMovementVelocityResult
	{
		TWeakObjectPtr<ACharacter> Character;
		TWeakObjectPtr<USceneComponent> Component;
		FVector Velocity = FVector::ZeroVector;
	};
	TOptional<FNativeMovementVelocityResult> NativeMovementVelocityResult;
	TWeakObjectPtr<ACharacter> NativeVelocityIntervalCharacter;
	TWeakObjectPtr<USceneComponent> NativeVelocityIntervalComponent;
	TOptional<FVector> NativeVelocityBeforeRootMotion;
	bool bNativeVelocityIntervalOpen = false;
	bool bNativeVelocityIntervalCanRetain = false;
	bool HasNativePassiveGroundVelocity() const;
	void BeginNativeMovementVelocityInterval();
	void MarkNativeMovementVelocityResult();
	void StoreNativeMovementVelocityResult();

	/**
	 * 远端姿态发布（authority，非 standalone）。候选只来自 OnMovementUpdated 内原完成区间；
	 * 键在本 CMC 原生 Tick 返回后补上，仅当其后没有新的 PerformMovement 入口（入口序号相同）
	 * 且胶囊仍等于原生写时间戳时的 LastUpdate 变换。不另建 Tick、计时器或时钟。
	 */
	struct FRemoteMotionPoseCandidate
	{
		FGGYGOMotionExecutionSnapshot Snapshot;
		FGGYGOLocomotionSteeringSnapshot Steering;
		uint64 MoveEntrySerial = 0;
		uint32 PublisherEpoch = 0;
		bool bFinalized = false;
	};
	/** 决定原生时间戳来源与样本归属的发布端身份；任一变化即开启新 Epoch 并关闭旧段。 */
	struct FRemoteMotionPoseOwnerIdentity
	{
		TWeakObjectPtr<ACharacter> Character;
		TWeakObjectPtr<USceneComponent> Component;
		TWeakObjectPtr<AController> Controller;
		ENetRole LocalRole = ROLE_None;
		ENetRole RemoteRole = ROLE_None;
		bool operator==(const FRemoteMotionPoseOwnerIdentity& Other) const;
	};
	FRemoteMotionPoseOwnerIdentity CaptureRemoteMotionPoseOwnerIdentity() const;
	TOptional<FRemoteMotionPoseOwnerIdentity> RemoteMotionPosePublisherIdentity;
	uint32 RemoteMotionPosePublisherEpoch = 0;
	/** 发布端身份变化时开启新 Epoch；返回 false 表示发布已关闭。 */
	bool RefreshRemoteMotionPosePublisherIdentity();
	struct FRemoteMotionPoseSegmentKey
	{
		uint64 ExecutionRequestSerial = 0;
		uint16 MotionSequence = 0;
		EGGYGOLocomotionMotionType MotionType = EGGYGOLocomotionMotionType::None;
		FGGYGOLocomotionSourceBindingPtr Binding;
		bool Matches(const FGGYGOMotionExecutionSnapshot& Snapshot) const;
	};
	TOptional<FRemoteMotionPoseCandidate> RemoteMotionPoseCandidate;
	TOptional<FRemoteMotionPoseSegmentKey> RemoteMotionPoseSegment;
	uint64 RemoteMotionPoseMoveEntrySerial = 0;
	uint32 RemoteMotionPoseLastSegmentSerial = 0;
	bool bRemoteMotionPosePublicationClosed = false;
	bool bRemoteMotionPosePublisherDiagnosticReported = false;
	bool IsRemoteMotionPosePublisher() const;
	void CaptureRemoteMotionPoseCandidate();
	void FinalizeRemoteMotionPosePublication();
	void PublishRemoteMotionPoseRecord(FGGYGORemoteMotionPoseRecord Content);
	/** 段资源在 move 之外退役（重置、换绑、组件替换、EndPlay）；只关闭未自然结束的已发布段。 */
	void CloseRemoteMotionPoseSegment(EGGYGORemoteMotionPoseReason Reason);
	/** 本地 getter 与远端发布共用的原 Locomotion 请求/段/绑定成员资格；不含帧窗口与角色判断。 */
	EGGYGOMotionExecutionStatus QualifyLocomotionExecutionMembership(
		const FGGYGOMotionExecutionSnapshot& Snapshot, FString& OutError) const;

	/**
	 * 远端姿态观察端（SimulatedProxy 或 listen server 远端控制角色）。原生呈现样本只在
	 * SmoothCorrection 实参被证明正是当前原生复制字段的呈现时可认证；不合成样本、不排序样本键。
	 */
	struct FRemoteMotionPoseNativeSample
	{
		uint64 EventSerial = 0;
		float Key = 0.0f;
		bool bCertifiable = false;
		bool bRelativeTransform = false;
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float LocationTolerance = 0.0f;
		float RotationTolerance = 0.0f;
		FString Diagnostic;
	};
	struct FRemoteMotionPoseAccepted
	{
		FGGYGORemoteMotionPoseRecord Record;
		/** 该记录配对到的原生呈现事件；与最新事件相同才是 Current。 */
		uint64 MatchedEventSerial = 0;
		EGGYGORemoteMotionPoseIdentity Identity = EGGYGORemoteMotionPoseIdentity::None;
		FGGYGOLocomotionSourceBindingPtr Binding;
		uint64 AcceptedFrame = 0;
	};
	enum class ERemoteMotionPoseSourceCheck : uint8 { Verified, Awaiting, Invalid };
	TOptional<FGGYGORemoteMotionPoseRecord> RemoteMotionPosePending;
	TOptional<FRemoteMotionPoseAccepted> RemoteMotionPoseAccepted;
	/** 同 Epoch 同段、键不同的上一已配对样本，仅供原生 Linear 显示坐标插值。 */
	TOptional<FRemoteMotionPoseAccepted> RemoteMotionPosePrevious;
	FRemoteMotionPoseNativeSample RemoteMotionPoseNativeSample;
	uint64 RemoteMotionPoseNativeEventSerial = 0;
	TOptional<FRemoteMotionPoseOwnerIdentity> RemoteMotionPoseConsumerIdentity;
	EGGYGORemoteMotionPoseWaitReason RemoteMotionPoseWaitReason = EGGYGORemoteMotionPoseWaitReason::None;
	FString RemoteMotionPoseDiagnostic;
	uint32 RemoteMotionPoseLastSeenSerial = 0;
	bool bRemoteMotionPoseSeen = false;
	bool IsRemoteMotionPoseConsumer() const;
	/** 角色/组件/控制者/网络角色任一变化都清空观察端状态；返回 false 表示当前不是观察端。 */
	bool EnsureRemoteMotionPoseConsumerIdentity();
	void ResetRemoteMotionPoseConsumer();
	void ReceiveRemoteMotionPoseRecord(const FGGYGORemoteMotionPoseRecord& Record);
	void CaptureRemoteMotionPoseNativeSample(const FVector& NewLocation, const FQuat& NewRotation);
	bool DoesRemoteMotionPoseMatchNativeSample(const FGGYGORemoteMotionPoseRecord& Record) const;
	void TryAcceptRemoteMotionPose();
	void RefreshRemoteMotionPoseConsumer();
	/** 本端当前动画源与记录源身份比对；内容摘要重算，区分合法未就绪与已证明失败。 */
	ERemoteMotionPoseSourceCheck VerifyRemoteMotionPoseSource(const FGGYGORemoteMotionPoseRecord& Record,
		FGGYGOLocomotionSourceBindingPtr& OutBinding, EGGYGORemoteMotionPoseIdentity& OutIdentity,
		EGGYGORemoteMotionPoseWaitReason& OutWait, FString& OutError) const;
	/** 查询时的轻量资格：同一不可变发布、原生生命周期仍当前、配置未变；内容摘要由原生 Tick 重验。 */
	ERemoteMotionPoseSourceCheck CheckRemoteMotionPoseBindingCurrent(const FRemoteMotionPoseAccepted& Accepted,
		const FGGYGOLocomotionSequenceSource*& OutSource, EGGYGORemoteMotionPoseWaitReason& OutWait, FString& OutError) const;
	UFUNCTION()
	void OnRep_RemoteMotionPoseRecord();

	/** Derived observation/scratch for the original native interval; never saved-move authority. */
	FGGYGOLocomotionSteeringSnapshot LocomotionSteeringSnapshot;
	mutable FGGYGOMotionExecutionSnapshot MotionExecutionSnapshot;
	/** 仅原同步修改栈／当前 native move 携出的退休义务；在原安全出口排空，无独立调度。 */
	TArray<FGGYGOLocomotionSourceRetirementFact> PendingLocomotionSourceRetirements;
	int32 LocomotionSourceRetirementScopeDepth = 0;
	/** 客户端校正／历史移动调用栈；与输入观察者的事实回放深度是不同寿命。 */
	int32 HistoricalLocomotionSourceRetirementScopeDepth = 0;
	FGGYGOLocomotionSourceRetiredDelegate LocomotionSourceRetired;
	FGGYGOMotionExecutionSnapshot PendingMotionExecution;
	uint64 MotionExecutionIntervalSerial = 0;
	bool bMotionExecutionIntervalOpen = false;
	bool bHasPendingMotionExecution = false;
	TWeakObjectPtr<const UGGYGOMovementSet> LocomotionSteeringOriginalSet;
	FGGYGOLocomotionSourceBindingPtr LocomotionSteeringOriginalSource;
	uint64 LocomotionSteeringSourceEpoch = 1;
	uint64 LocomotionSteeringIntervalSerial = 0;
	float LocomotionSteeringEntryYaw = 0.0f;
	float LocomotionSteeringCompletedYaw = 0.0f;
	FVector LocomotionSteeringCompletedVelocity = FVector::ZeroVector;
	bool bLocomotionSteeringIntervalOpen = false;
	bool bLocomotionSteeringIntervalOrdinary = false;
	bool bLocomotionSteeringIntervalExcluded = false;
	FString LocomotionSteeringIntervalError;
	TOptional<float> LocomotionSteeringRotationBudget;
	/** 原 AddMovementInput 到下一 native 区间的投影来源；只含身份/Yaw，不保存运动方向。 */
	struct FLocomotionInputProjectionReceipt
	{
		FGGYGOMovementInputConsumerBindingId Binding;
		FGGYGOMovementInputRequestIdentity Request;
		FGGYGOMovementInputEvaluationToken Evaluation;
		TWeakObjectPtr<APlayerController> Controller;
		uint64 Frame = 0;
		float ControlYaw = 0.0f;
		bool bRemoteNative = false;
		bool bHasProjection = false;
		uint64 BindingSerial = 0;
		uint64 SessionSerial = 0;
		FMovementOwnerSyncNativeReceipt NativeReceipt;
		bool bAmbiguous = false;
		FString Diagnostic;
	};
	FLocomotionInputProjectionReceipt PendingLocomotionInputProjection;
	FLocomotionInputProjectionReceipt IntervalLocomotionInputProjection;
	/** 原 scope/controller 的镜头派生残差；不参与实际转向，不随健康步态/Sequence 切换重置。 */
	FGGYGOMovementOwnerSyncScopeId LocomotionCameraScope;
	uint64 LocomotionCameraBindingSerial = 0;
	uint64 LocomotionCameraSessionSerial = 0;
	TWeakObjectPtr<APlayerController> LocomotionCameraController;
	TWeakObjectPtr<USceneComponent> LocomotionCameraComponent;
	float LocomotionCameraPreviousYaw = 0.0f;
	float LocomotionCameraRemainingYaw = 0.0f;
	bool bLocomotionCameraBaselineValid = false;
	void ResetLocomotionCameraTurn();
	void CompleteLocomotionCameraTurn(FGGYGOLocomotionSteeringSnapshot& Completed);
	bool CaptureLocomotionInputProjectionForNativeMove(float& OutYaw) const;
	void ResetLocomotionSteeringObservation();
	void BeginLocomotionSteeringInterval();
	bool CanExecuteOrdinaryLocomotionSteering() const;
	void RejectLocomotionSteering(const FString& Error);

	/** 只从已接受配置指针派生，不代表原动画源本帧求值成功。 */
	bool HasAcceptedMovementSet() const;
	/** Presentation-only startup qualification; never permits ground execution without an accepted Set. */
	bool IsOriginalMovementSetInitializationPending() const;
	/** Original Set entry has made a decision; resets/stops cannot reopen this startup window. */
	bool bMovementSetConfigurationSubmitted = false;
	FString MovementSetConfigurationError;

	/** 实际动画 RootMotion 或符合已有提交契约的 ActionCurve；未知 RMS 不构成豁免。 */
	bool HasIndependentGroundRootMotion() const;

	/** 未结束 Current/Pending ActionCurve；结束的 Current 须 Prepared 且 native Override 有效；无本地 token 依赖。 */
	bool HasRegisteredActionCurveSource() const;

	/** 首个不符合现有 ActionCurve/自有 Locomotion 提交契约的实际 RMS，仅读取、不持有。 */
	const FRootMotionSource* GetUnsupportedGroundRootMotionSource() const;

	/** 只判断缺少配置的普通地面执行，不合并曲线求值失败状态。 */
	bool ShouldRejectUnconfiguredGroundLocomotion() const;

	/** 拒绝请求/无来源执行结果；原生动量单独收束，不成为输入准入。 */
	void EnforceGroundLocomotionAdmission(const TCHAR* Entry);

	/**
	 * 仅诊断去重，不参与准入。初始 false，每次 Set 请求复位；C14 非空拒绝或
	 * 首个普通地面拒绝诊断置 true，后续有效绑定/合法解绑开启新周期。无资源句柄。
	 */
	bool bGroundAdmissionDiagnosticReported = false;
	bool bLocomotionSteeringDiagnosticReported = false;

protected:
	/** 同轴落到不可走角色胶囊时，在原物理区间内扫掠滑离；其余碰撞继续原生处理。 */
	virtual void HandleImpact(const FHitResult& Impact, float TimeSlice = 0.0f, const FVector& MoveDelta = FVector::ZeroVector) override;

	/** 订阅原 Extension 的身份通知；真实 Ready/Released 维护唯一派生 ASC 缓存。 */
	void CacheAbilitySystemComponent();

	/** 配置/ASC 绑定边界清理 Locomotion，包含转身锁存与复制标记；不清 GA ActionMotion。 */
	void ResetLocomotionState();


	/** 将已接受配置的参数原样写进 CMC 对应字段。 */
	void ApplyMovementSetToComponent();

	/** 取成功来源经RootMotionScale缩放的真实速度；无资格时返回0，须以IsCurveDrivingSpeed区分合法零值与无资格。 */
	float GetScaledCurveSpeed() const;

	/** 首次装配 MovementSet 前记录组件/蓝图基线，置空或切换时只恢复本模块覆盖的字段。 */
	void CaptureComponentDefaults();
	void RestoreComponentDefaults();

	/** CMC 内唯一 Locomotion 状态推进与原动画源求值入口。 */
	bool UpdateLocomotionMotion(float DeltaSeconds, bool bHadMoveInput, EGGYGOGait PreviousGait);
	void ValidateClientLocomotionHint() const;

	/** Remove inactive owned segment sources; only Stage installs them and Commit publishes the phase. */
	void RemoveInactiveLocomotionCurveSources();

	/**
	 * 挂一个曲线位移源。
	 *
	 * 仅接受本 CMC 发出的原始请求与输入。只复用同一 Origin 的有效 source；
	 * 同名的其它资源明确拒绝，已退场的本地资源可由新请求接替。
	 *
	 * `BaseYaw` 必须由调用方给出而不是在这里取当前朝向：转身段的基准是进入相位时
	 * 记下的 `TurnBackEntryYaw`，那个值在整段转身里不变，而角色朝向一直在转。
	 */
	bool ApplyCurveRootMotionSource(FName InstanceName, uint16 Priority, float BaseYaw, bool bEndOnZeroSpeed);

	/** 转身状态整体复位。 */
	void ResetTurnBack();

	/** Release the owner token and locomotion gates when the RMS naturally expires or is removed. */
	void CleanupFinishedActionMotion();

	friend class FGGYGOActionMotionExecution;
	friend struct FRootMotionSource_GGYGOActionCurve;
	void SuspendLocomotionForAction();
	bool ValidateMontageActionRuntime(const FRootMotionSource_GGYGOActionCurve& Source,
		float SimulationTime, FString& OutError) const;
	bool ValidateProfileActionRuntime(const FRootMotionSource_GGYGOActionCurve& Source, FString& OutError) const;
	void FailProfileActionMotion(const TSharedPtr<const FGGYGOActionMotionResource>& OriginalResource, const FString& Error);
	void FailMontageActionMotion(const TSharedPtr<const FGGYGOActionMotionResource>& OriginalResource, const FString& Error);
	void PublishMontageActionMotionCompletion();
	void ResumeLocomotionAfterAction();
	void PublishQualifiedMovementIntent();
	void RetireQualifiedMovementIntent(const FGGYGOMovementOwnerSyncScopeId& OriginalScope, FName Reason);
	struct FQualifiedMovementIntentObserver;
	TArray<TSharedPtr<FQualifiedMovementIntentObserver>> QualifiedMovementIntentObservers;
	uint64 QualifiedMovementIntentLastObserverSerial = 0;
	uint64 QualifiedMovementIntentPublicationSerial = 0;
	int32 QualifiedMovementIntentReplayDepth = 0;
	/** Bind the sole action slot on every native constructor path, including the vtable helper. */
	FGGYGOActionMotionExecution ActionMotionExecution{*this};

	/**
	 * 曲线是否正在接管转身的朝向与位移方向（`Turning` 或 `Braking`）。
	 *
	 * 这两段合成一个判定是因为它们的移动行为完全一致：方向都取曲线在入口坐标系里的
	 * 值，朝向都由转角增量累加。区别只在转角还在不在变，而那是表现层的事。
	 * 模拟代理经 GetTurnBackPhase 读取已认证远端姿态记录的相位。
	 */
	bool IsTurnBackCurveDriven() const;

protected:
	/** 已通过绑定校验的只读配置；指针是配置准入的唯一来源，变更须重绑。 */
	UPROPERTY(Transient)
	TObjectPtr<const UGGYGOMovementSet> MovementSet;

	/** 原 opaque 资源的派生 ASC 缓存；CantMove 仅通过 const Ready Getter 查询。 */
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOAbilitySystemComponent> AbilitySystemComponent;

	/** 本帧步态。 */
	UPROPERTY(Replicated)
	EGGYGOGait ResolvedGait = EGGYGOGait::None;

	UPROPERTY(Replicated)
	EGGYGOLocomotionMotionType LocomotionMotionType = EGGYGOLocomotionMotionType::None;

	UPROPERTY(Replicated)
	EGGYGOStopMotionType StopMotionType = EGGYGOStopMotionType::None;

	UPROPERTY(Replicated)
	float WalkRunBlendAlpha = 0.0f;

	/** 非循环段时间（秒）与 WalkRun 共享规范化循环相位。 */
	float LocomotionMotionTime = 0.0f;
	float WalkRunCyclePhase = 0.0f;
	uint16 LocomotionMotionSequence = 0;

	/** 走跑计时器（秒）。只在 Walk 且持续移动时累加。 */
	float WalkHoldTimer = 0.0f;

	/** "下次移动直接进 Run" 契约。 */
	bool bWantsRunOnNextMove = false;

	/** 本地输入请求；SavedMove 的 Custom_3 同步给服务器。 */
	bool bForceWalkRequested = false;

	/** 上一帧是否有移动输入。用于识别起步上升沿。 */
	bool bPreviousHasMoveInput = false;

	/** 上一帧是否被禁止移动。用于识别解禁沿。 */
	bool bPreviousMovementBlocked = false;

	// ===== 转身状态 =====

	/** 当前相位。 */
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;

	/** 本次转身已进行的时长（秒）。只用于兜底超时，相位推进不看它。 */
	float TurnBackElapsed = 0.0f;

	/** 本次转身已消费触发资格；仅原段的独立信号重新开放，请求取消时清理。 */
	bool bTurnBackInputLatched = false;

	/** 进入转身时的 Actor yaw（度）。曲线的段起点方向转世界方向的基准。 */
	float TurnBackEntryYaw = 0.0f;

	/**
	 * 权威完成区间的远端姿态记录（COND_SimulatedOnly）。只由 authority 在原生 Tick 返回后写入；
	 * 观察端只在同键原生样本到达后接受，listen server 远端控制角色在本地直接走同一接受路径。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_RemoteMotionPoseRecord)
	FGGYGORemoteMotionPoseRecord RemoteMotionPoseRecord;

	/**
	 * 本帧的曲线运动量。
	 *
	 * 普通段在 Before 中求值；CurveRMS 段由原 CMC 在实际 native Prepare 区间求值。
	 * SavedMove 保留原输入和区间结果，不从当前平铺样本恢复历史。
	 */
	FGGYGOLocomotionCurveSample CurveMotion;

	FCharacterNetworkMoveDataContainer_GGYGO NetworkMoveDataContainer;
	FCharacterMoveResponseDataContainer_GGYGO MoveResponseDataContainer;
	bool bHasPendingAuthoritativeLocomotionState = false;
	bool bReplayLocomotionFromAuthority = false;

	bool bComponentDefaultsCaptured = false;
	float DefaultMaxAcceleration = 0.0f;
	float DefaultBrakingDecelerationWalking = 0.0f;
	float DefaultGroundFriction = 0.0f;
	bool bDefaultOrientRotationToMovement = false;
	FRotator DefaultRotationRate = FRotator::ZeroRotator;

	friend class FSavedMove_GGYGO;
	friend struct FCharacterNetworkMoveData_GGYGO;
	friend struct FCharacterMoveResponseDataContainer_GGYGO;
protected:
	/** 订阅本 Pawn 的 ASC 就绪／释放；返回成功表示订阅成立，不表示已就绪。 */
	bool PrepareLocalAbilitySystemSubscription(UGGYGOPawnExtensionComponent* Extension, FString& OutError);
	/** 退订；已消费资源时清 ASC 缓存并复位移动状态。 */
	void ReleaseLocalAbilitySystemSubscription();
	/** 订阅已消费且仍就绪的 ASC；不授予移动执行权。 */
	const UGGYGOAbilitySystemComponent* GetReadyLocalAbilitySystemComponent() const;

private:
	void ConsumeLocalAbilitySystemNotice(const FGGYGOPawnASCNotice& Notice);
	/** 只持有订阅；ASC 缓存仍是派生值，不是移动准入。 */
	FGGYGOLocalASCSubscription LocalASCSubscription;

};
