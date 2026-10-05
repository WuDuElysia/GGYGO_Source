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
#include "Animation/Data/GGYGOLocomotionSourceBinding.h"
#include "Animation/Data/GGYGOActionMotionSourceBinding.h"
#include "Input/GGYGOMovementInputTypes.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/CharacterMovementReplication.h"
// Movement-LocalASC-A includes begin.
#include "Templates/SharedPointer.h"
// Movement-LocalASC-A includes end.


#include "GGYGOCharacterMovementComponent.generated.h"

class AActor;
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
struct FGGYGOCurveRootMotionMoveInput;
struct FGGYGOLocomotionPreparedState;

/** CMC 原生拥有者同步状态；Ready 不代表来源或移动执行准入。 */
enum class EGGYGOMovementOwnerSyncState : uint8
{
	Waiting,
	Ready,
	Invalidated
};

/** 原 CMC/拥有者范围的身份值；IsSet 只检查已发行形状，不证明当前有效。 */
struct GGYGO_API FGGYGOMovementOwnerSyncScopeId
{
	bool IsSet() const;
	bool operator==(const FGGYGOMovementOwnerSyncScopeId& Other) const;
	bool operator!=(const FGGYGOMovementOwnerSyncScopeId& Other) const;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> GetConsumer() const;
	TWeakObjectPtr<APawn> GetOriginalPawn() const;
	TWeakObjectPtr<APlayerController> GetOriginalPlayerController() const;
	uint64 GetConsumerLifetimeSerial() const;
	uint64 GetOwnerContextSerial() const;
	uint64 GetScopeSerial() const;
	uint64 GetResponseNonce() const;
private:
	friend class UGGYGOCharacterMovementComponent;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> Consumer;
	TWeakObjectPtr<APawn> OriginalPawn;
	TWeakObjectPtr<APlayerController> OriginalPlayerController;
	uint64 ConsumerLifetimeSerial = 0;
	uint64 OwnerContextSerial = 0;
	uint64 ScopeSerial = 0;
	uint64 ResponseNonce = 0;
};

/** 精确订阅句柄；注销只作用于该记录，不会查找并替换后继 Scope。 */
struct GGYGO_API FGGYGOMovementOwnerSyncObserverId
{
	bool IsSet() const;
	const FGGYGOMovementOwnerSyncScopeId& GetScope() const;
	uint64 GetObserverSerial() const;
private:
	friend class UGGYGOCharacterMovementComponent;
	FGGYGOMovementOwnerSyncScopeId Scope;
	uint64 ObserverSerial = 0;
};

/** CMC 发布的原通知值；不包含 Press/Held/Neutral 或执行成功。 */
struct GGYGO_API FGGYGOMovementOwnerSyncNotice
{
	bool IsSet() const;
	const FGGYGOMovementOwnerSyncScopeId& GetScope() const;
	EGGYGOMovementOwnerSyncState GetState() const;
	uint64 GetNoticeSerial() const;
	uint64 GetServerOwnerGeneration() const;
	uint64 GetNativeResponseNonce() const;
	bool IsInitialSynchronizationEligible() const;
	FName GetReason() const;
private:
	friend class UGGYGOCharacterMovementComponent;
	FGGYGOMovementOwnerSyncScopeId Scope;
	EGGYGOMovementOwnerSyncState State = EGGYGOMovementOwnerSyncState::Invalidated;
	uint64 NoticeSerial = 0;
	uint64 ServerOwnerGeneration = 0;
	uint64 NativeResponseNonce = 0;
	bool bInitialSynchronizationEligible = false;
	FName Reason = NAME_None;
};

DECLARE_DELEGATE_TwoParams(FGGYGOMovementOwnerSyncDelegate,
	const FGGYGOMovementOwnerSyncObserverId&, const FGGYGOMovementOwnerSyncNotice&);

enum class EGGYGOActionMotionReleaseReason : uint8
{
	Completed, Cancelled, Replaced, OwnerInvalidated
};

enum class EGGYGOQualifiedMovementIntentQueryResult : uint8
{
	Unavailable = 0, Qualified, NotHeld, AwaitingPhysicalProof, WaitingForAdmission, ExecutionFailed
};

enum class EGGYGOQualifiedMovementIntentProvenance : uint8
{
	Unavailable = 0, LocalSourceHeld, AuthenticatedNativeRequest
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

DECLARE_DELEGATE_TwoParams(FGGYGOActionMotionFailureDelegate, int32, const FString&);

/** Immutable original action identity; native RMS owns time and per-move release state. */
struct FGGYGOActionMotionResource
{
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> Owner;
	TWeakObjectPtr<ACharacter> Character;
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	TWeakObjectPtr<UAnimInstance> AnimInstance;
	FGGYGOActionMotionSourceBindingPtr Source;
	int32 MontageInstanceId = INDEX_NONE;
	int32 Handle = INDEX_NONE;
	float InstancePlayRate = 0.0f;
};

enum class EGGYGOMovementInitialRequestAdmissionResult : uint8
{
	Admitted, AlreadyAdmitted, Stale, Rejected, ExecutionFailed
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
	float SourceTimeOrigin = 0.0f;
	float MotionTimeOrigin = 0.0f;
	float BaseYaw = 0.0f;
	float RootMotionScale = 0.0f;
	bool bEndOnZeroSpeed = false;
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
	EGGYGOCurveRootMotionPrepareResult Result = EGGYGOCurveRootMotionPrepareResult::Failed;
};
// Movement-LocalASC-A forward declarations begin.
class UGGYGOPawnExtensionComponent;
struct FGGYGOPawnASCLocalNotice;
// Movement-LocalASC-A forward declarations end.


/** Fact acknowledgement only; Recorded does not grant movement execution. */
enum class EGGYGOMovementInputConsumeResult : uint8
{
	Recorded,
	Duplicate,
	Stale,
	Rejected
};

/**
 * Fixed original source values for one move; never grants execution or observes keys.
 * Written only by successful local binding, Recorded facts and exact receiver teardown.
 * Begin-time release/neutral anchors survive later facts. SourceUnresolved is retained
 * independently of the latest fact; receiver teardown is not a physical release.
 * No UObject/Origin/Prepared, execution state, curve, velocity, reason string or ACK state.
 * Wire v1: absent=9 bits; present<=913 bits (14 full uint64 + 9 enum/flag bits + version).
 */
struct FGGYGOMovementInputSourceCheckpoint
{
	static constexpr uint8 WireVersion = 1;
	static constexpr uint32 SerialFieldCount = 14;
	static constexpr uint32 MaxSerializedBits = 8 + 1 + SerialFieldCount * 64 + 1 + 3 + 2 + 2;

	bool bPresent = false;
	bool bConsumerInvalidated = false;
	uint64 BindingSerial = 0;
	uint64 ConsumerFenceSerial = 0;
	uint64 SessionSerial = 0;
	uint64 SessionOpenedEventSerial = 0;
	uint64 EventSerial = 0;
	uint64 FactRequestSerial = 0;
	uint64 RequestSerial = 0;
	uint64 RequestStartedEventSerial = 0;
	uint64 StartReleaseRequestSerial = 0;
	uint64 StartReleaseEventSerial = 0;
	uint64 StartNeutralEventSerial = 0;
	uint64 RequestReleasedEventSerial = 0;
	uint64 NeutralEventSerial = 0;
	uint64 SourceUnresolvedEventSerial = 0;
	EGGYGOMovementInputFactKind LastFactKind = EGGYGOMovementInputFactKind::Invalid;
	EGGYGOMovementInputSessionMode SessionMode = EGGYGOMovementInputSessionMode::Invalid;
	EGGYGOMovementInputStartProof StartProof = EGGYGOMovementInputStartProof::Invalid;

	bool operator==(const FGGYGOMovementInputSourceCheckpoint& Other) const;
	bool operator!=(const FGGYGOMovementInputSourceCheckpoint& Other) const { return !(*this == Other); }
	bool IsValid(FString* OutError = nullptr) const;
	bool Serialize(FArchive& Ar, FString* OutError = nullptr);
};

/**
 * 本项目的 SavedMove。
 *
 * `FSavedMove_Character` 的职责是"把一次 move 所依赖的全部状态存下来，
 * 以便客户端回放和服务器重演"。任何影响移动结果又不在基类里的状态，
 * 都必须在这里保存，否则回放结果会与首次执行不同。
 *
 * 本类需要额外保存两项：步态（决定速度上限）与走跑计时器（决定何时升档）。
 */
class FSavedMove_GGYGO : public FSavedMove_Character
{
public:
	using Super = FSavedMove_Character;

	/** 清空以便对象复用。CMC 的 SavedMove 走对象池，不清会带上一次的残留。 */
	virtual void Clear() override;

	/** 把 CMC 的当前状态存进本 move。在 move 执行**前**调用。 */
	virtual void SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, class FNetworkPredictionData_Client_Character& ClientData) override;

	/** 把本 move 的状态写回 CMC。回放前调用，是 `SetMoveFor` 的逆操作。 */
	virtual void PrepMoveFor(ACharacter* C) override;
	virtual void PostUpdate(ACharacter* C, EPostUpdateMode PostUpdateMode) override;

	/**
	 * 能否与下一个 move 合并发送。
	 *
	 * 合并是带宽优化，但只有状态完全一致才能合并 —— 步态不同的两帧合并后，
	 * 服务器只会看到一个步态，另一帧的速度就错了。
	 */
	virtual bool CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const override;
	virtual bool IsImportantMove(const FSavedMovePtr& LastAckedMovePtr) const override;

	/** 仅把 ForceWalk 输入请求编码进压缩标志位。 */
	virtual uint8 GetCompressedFlags() const override;

	/** 本次 move 生效的步态。 */
	EGGYGOGait SavedGait = EGGYGOGait::None;

	/** 本次 move 解算后的结果，仅用于检测本地 move 合并边界。 */
	EGGYGOGait NetworkGait = EGGYGOGait::None;

	/** 本次 move 结束时是否进入 TurnBack 曲线段，仅用于禁止错误合并。 */
	bool bNetworkTurnBackCurveDriven = false;

	/** 本次 move 开始时的走跑计时器读数。 */
	float SavedWalkHoldTimer = 0.0f;
	bool bSavedForceWalkRequested = false;
	bool bSavedPreviousHasMoveInput = false;
	bool bSavedPreviousMovementBlocked = false;

	/** 本次 move 开始时是否持有"下次移动直接进 Run"的契约。 */
	bool bSavedWantsRunOnNextMove = false;

	/** 本次 move 开始时的 Locomotion 模拟状态。曲线量由原动画源和这些区间重新求值。 */
	EGGYGOLocomotionMotionType SavedLocomotionMotionType = EGGYGOLocomotionMotionType::None;
	EGGYGOStopMotionType SavedStopMotionType = EGGYGOStopMotionType::None;
	float SavedLocomotionMotionTime = 0.0f;
	float SavedWalkRunCyclePhase = 0.0f;
	float SavedWalkRunBlendAlpha = 0.0f;
	uint16 SavedLocomotionMotionSequence = 0;

	/** move 执行后的最小网络提示。服务端只校验，不接受客户端曲线或速度。 */
	EGGYGOLocomotionMotionType NetworkLocomotionMotionType = EGGYGOLocomotionMotionType::None;
	EGGYGOStopMotionType NetworkStopMotionType = EGGYGOStopMotionType::None;
	EGGYGOTurnBackPhase NetworkTurnBackPhase = EGGYGOTurnBackPhase::None;
	uint16 NetworkLocomotionMotionSequence = 0;

	/** 本次 move 的转身相位。 */
	EGGYGOTurnBackPhase SavedTurnBackPhase = EGGYGOTurnBackPhase::None;

	/** 本次 move 的转身计时。 */
	float SavedTurnBackElapsed = 0.0f;

	/** 本次 move 的转身入口朝向（度）。位移方向的基准，必须还原。 */
	float SavedTurnBackEntryYaw = 0.0f;

	/** 本次 move 的反向输入闩状态。影响能否触发下一次转身。 */
	bool bSavedTurnBackInputLatched = false;

	/** Original local input/result, including a source first issued during this move. */
	TSharedPtr<const FGGYGOCurveRootMotionMoveInput> SavedCurveRootMotionInput;
	TSharedPtr<const FGGYGOCurveRootMotionPrepared> SavedCurveRootMotionPrepared;
	FGGYGOLocomotionSourceBindingPtr SavedLocomotionSourceBinding;

	/** Original SetMoveFor value; sending and PostUpdate_Replay must not replace it. */
	FGGYGOMovementInputSourceCheckpoint SavedMovementInputSourceCheckpoint;
	/** 原 SetMoveFor 的同步身份；PostUpdate/回放/响应不得补写。不是来源认证。 */
	FGGYGOMovementOwnerSyncScopeId SavedMovementOwnerSyncScope;
	uint64 SavedMovementOwnerGeneration = 0;

private:
	/** Original SetMoveFor applicability; a replay snapshot, never request authority or physical Held. */
	struct FMovementInputRequestCapture
	{
		FGGYGOMovementInputConsumerBindingId Binding;
		FGGYGOMovementInputRequestIdentity Request;
		uint64 ExecutionRequestSerial = 0;
		uint8 Admission = 0;
		bool bExecutionEligible = false;
	};
	FMovementInputRequestCapture SavedMovementInputRequest;
	friend class UGGYGOCharacterMovementComponent;
	friend struct FCharacterNetworkMoveData_GGYGO;
};

/** 客户端预测数据。唯一职责是让 CMC 分配出我们自己的 SavedMove 类型。 */
class FNetworkPredictionData_Client_GGYGO : public FNetworkPredictionData_Client_Character
{
public:
	using Super = FNetworkPredictionData_Client_Character;

	explicit FNetworkPredictionData_Client_GGYGO(const UCharacterMovementComponent& ClientMovement);

	virtual FSavedMovePtr AllocateNewMove() override;
};

/** 原来源值与Owner元数据运输；服务端经原生receipt消费，动画/曲线/速度使用自身源绑定求值。 */
struct FCharacterNetworkMoveData_GGYGO : public FCharacterNetworkMoveData
{
	/** 固定 9/137 位扩展，原生 owning actor RPC 提供实际 PC/Connection 身份。 */
	static constexpr uint8 OwnerSyncWireVersion = 2;
	bool bHasMovementOwnerSync = false;
	uint64 MovementOwnerSyncNonce = 0;
	uint64 MovementOwnerGeneration = 0;
	FGGYGOMovementInputSourceCheckpoint MovementInputSourceCheckpoint;
	EGGYGOLocomotionMotionType LocomotionMotionType = EGGYGOLocomotionMotionType::None;
	EGGYGOStopMotionType StopMotionType = EGGYGOStopMotionType::None;
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;
	uint16 LocomotionMotionSequence = 0;

	virtual void ClientFillNetworkMoveData(const FSavedMove_Character& ClientMove, ENetworkMoveType MoveType) override;
	virtual bool Serialize(UCharacterMovementComponent& CharacterMovement, FArchive& Ar, UPackageMap* PackageMap, ENetworkMoveType MoveType) override;
};

struct FCharacterNetworkMoveDataContainer_GGYGO : public FCharacterNetworkMoveDataContainer
{
	FCharacterNetworkMoveDataContainer_GGYGO();
	FCharacterNetworkMoveData_GGYGO MoveData[3];
};

/** 位置校正同时带回服务端 Locomotion 基线，随后从该基线重放未确认 move。 */
struct FCharacterMoveResponseDataContainer_GGYGO : public FCharacterMoveResponseDataContainer
{
	/** ACK/correction 均运输；nonce 只来自原 NewMove 的原生调用，不从当前客户端重取。 */
	static constexpr uint8 OwnerSyncWireVersion = 2;
	bool bHasMovementOwnerSync = false;
	uint64 MovementOwnerSyncNonce = 0;
	uint64 ServerOwnerGeneration = 0;
	uint64 AdjustmentOwnerGeneration = 0;
	bool bMovementOwnerActive = false;
	bool bInitialSynchronizationEligible = false;
	EGGYGOLocomotionMotionType LocomotionMotionType = EGGYGOLocomotionMotionType::None;
	EGGYGOStopMotionType StopMotionType = EGGYGOStopMotionType::None;
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;
	EGGYGOGait Gait = EGGYGOGait::None;
	float LocomotionMotionTime = 0.0f;
	float WalkRunCyclePhase = 0.0f;
	float WalkRunBlendAlpha = 0.0f;
	float WalkHoldTimer = 0.0f;
	float TurnBackElapsed = 0.0f;
	float TurnBackEntryYaw = 0.0f;
	uint16 LocomotionMotionSequence = 0;
	bool bForceWalkRequested = false;
	bool bPreviousHasMoveInput = false;
	bool bPreviousMovementBlocked = false;
	bool bWantsRunOnNextMove = false;
	bool bTurnBackInputLatched = false;

	virtual void ServerFillResponseData(const UCharacterMovementComponent& CharacterMovement, const FClientAdjustment& PendingAdjustment) override;
	virtual bool Serialize(UCharacterMovementComponent& CharacterMovement, FArchive& Ar, UPackageMap* PackageMap) override;
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

	/** 保留 CMC 自身 Tick，只做 ActionMotion 资源回收；Locomotion 在 move 模拟入口推进。 */
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** 按当前步态返回速度上限。被 `Restriction.CantMove` 阻断时返回 0。 */
	virtual float GetMaxSpeed() const override;

	/** 地面最低模拟速度不得抬高本模块解算出的上限，包括合法的零速度。 */
	virtual float GetMinAnalogSpeed() const override;

	/** 未准入的普通地面移动不消费输入/RequestedMove，也不保留旧平面速度。 */
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;

	/** native RMS 最后应用帧也须经过地面准入；独立动作/实际动画 RootMotion 保持原生执行。 */
	virtual void ApplyRootMotionToVelocity(float DeltaTime) override;

	/** 每次 move（含回放）前解算步态。 */
	virtual void UpdateCharacterStateBeforeMovement(float DeltaSeconds) override;

	/** 从压缩标志位取回 ForceWalk 输入请求；步态与动作段由服务端按自身配置重算。 */
	virtual void UpdateFromCompressedFlags(uint8 Flags) override;

	/** 转身的曲线接管段朝向由动画曲线驱动，其余情况交回基类。 */
	virtual void PhysicsRotation(float DeltaTime) override;
	virtual void ClientHandleMoveResponse(const FCharacterMoveResponseDataContainer& MoveResponse) override;
	virtual bool ClientUpdatePositionAfterServerUpdate() override;
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

	/** Authority-only ground action. Returns an owner token, or INDEX_NONE; rejects overlapping actions. */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Movement|Action Motion")
	int32 BeginActionMotion(const UGGYGOActionMotionProfile* Profile, float PlayRate = 1.f);

	/** Only the current token can remove its source. Safe after natural completion or repeated cleanup. */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Movement|Action Motion")
	void EndActionMotion(int32 Handle);

	/** Original Montage XYZ trajectory; native Walking/NavWalking/Falling own collision and landing. */
	bool BeginMontageActionMotion(const FGGYGOActionMotionSourceBindingPtr& OriginalSource,
		int32 OriginalMontageInstanceId, float MontagePositionSeconds, float EffectiveMontagePlayRate,
		float TranslationScale, int32& OutHandle, FString& OutError);
	bool ReleaseMontageActionMotion(int32 OriginalHandle, EGGYGOActionMotionReleaseReason Reason, FString& OutError);
	bool CancelMontageActionMotionForMovement(int32 OriginalHandle,
		const FGGYGOQualifiedMovementIntent& OriginalIntent, FString& OutError);
	/** Sole original GA failure recipient; release removes it before external cleanup. */
	bool ObserveMontageActionMotionFailure(int32 OriginalHandle,
		FGGYGOActionMotionFailureDelegate Callback, FString& OutError);
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
	 * 模拟代理上只区分"曲线是否正在接管移动"：相位的精确取值在本地控制端解算，
	 * 复制给其它客户端的只有一个 bool，所以那里只能返回 `Turning` 或 `None`。
	 * 动画层只判断是否 `None`，不区分具体相位，这个精度足够。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|TurnBack")
	EGGYGOTurnBackPhase GetTurnBackPhase() const;

	/**
	 * 转身是否已进入交还输入的 `RunOut` 段。
	 *
	 * 模拟代理恒为 false：那一端只知道"曲线是否在接管"，而 `RunOut` 段的移动
	 * 与普通移动无异，模拟代理按普通移动表现即可，不需要这个标记。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|TurnBack")
	bool IsTurnBackRunOut() const;

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

	/** 是否正被 `Restriction.CantMove` 禁止移动。 */
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
	void EnforceMovementInputLocomotionAdmission();
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
	bool EvaluateLocomotionSource(float DeltaSeconds, FLocomotionUpdateCandidate& Candidate, FString& OutError) const;
	bool EvaluateWalkRunSource(float DeltaSeconds, FLocomotionUpdateCandidate& Candidate, FString& OutError) const;
	bool GetLocomotionSourceBinding(FGGYGOLocomotionSourceBindingPtr& OutBinding, FString& OutError) const;
	bool IsLocomotionSourceBindingCurrent(const FGGYGOLocomotionSourceBindingPtr& Binding, FString& OutError) const;
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
	void ConsumeLocomotionCurvePrepared(const TSharedPtr<const FGGYGOCurveRootMotionPrepared>& Prepared);
	bool HasCurrentLocomotionCurveOrigin(const TSharedPtr<const FGGYGOCurveRootMotionOrigin>& Origin) const;

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

	/** 只从已接受配置指针派生，不代表原动画源本帧求值成功。 */
	bool HasAcceptedMovementSet() const;

	/** 实际动画 RootMotion 或符合已有提交契约的 ActionCurve；未知 RMS 不构成豁免。 */
	bool HasIndependentGroundRootMotion() const;

	/** 未结束 Current/Pending ActionCurve；结束的 Current 须 Prepared 且 native Override 有效；无本地 token 依赖。 */
	bool HasRegisteredActionCurveSource() const;

	/** 首个不符合现有 ActionCurve/自有 Locomotion 提交契约的实际 RMS，仅读取、不持有。 */
	const FRootMotionSource* GetUnsupportedGroundRootMotionSource() const;

	/** 只判断缺少配置的普通地面执行，不合并曲线求值失败状态。 */
	bool ShouldRejectUnconfiguredGroundLocomotion() const;

	/** 拒绝时清平面速度；首次真实请求输出一次诊断，无请求的未绑定暂态不日志。 */
	void EnforceGroundLocomotionAdmission();

	/**
	 * 仅诊断去重，不参与准入。初始 false，每次 Set 请求复位；C14 非空拒绝或
	 * 首个普通地面拒绝诊断置 true，后续有效绑定/合法解绑开启新周期。无资源句柄。
	 */
	bool bGroundAdmissionDiagnosticReported = false;

protected:
	/** 订阅原 Extension 的身份通知；真实 Ready/Released 维护唯一派生 ASC 缓存。 */
	void CacheAbilitySystemComponent();

	/** 配置/ASC 绑定边界清理 Locomotion，包含转身锁存与复制标记；不清 GA ActionMotion。 */
	void ResetLocomotionState();

	/**
	 * 解算本帧步态。
	 *
	 * 只在本地控制端与服务器权威端调用；模拟代理消费服务器复制的结果。
	 */
	void ResolveGait(float DeltaSeconds);

	/** 推进或归零走跑计时器。 */
	void UpdateWalkHoldTimer(EGGYGOGait FrameGait, bool bHasMoveInput, bool bBlocked, bool bOnGround, bool bMoveInputRising, bool bBlockReleased, float DeltaSeconds);

	/** 将已接受配置的参数原样写进 CMC 对应字段。 */
	void ApplyMovementSetToComponent();

	/** 取成功来源经RootMotionScale缩放的真实速度；无资格时返回0，须以IsCurveDrivingSpeed区分合法零值与无资格。 */
	float GetScaledCurveSpeed() const;

	/** 首次装配 MovementSet 前记录组件/蓝图基线，置空或切换时只恢复本模块覆盖的字段。 */
	void CaptureComponentDefaults();
	void RestoreComponentDefaults();

	/** CMC 内唯一 Locomotion 状态推进与原动画源求值入口。 */
	bool UpdateLocomotionMotion(float DeltaSeconds, bool bHadMoveInput, EGGYGOGait PreviousGait);
	void UpdateWalkRunBlend(float DeltaSeconds);
	void SetLocomotionMotion(EGGYGOLocomotionMotionType NewType);
	void ValidateClientLocomotionHint() const;

	/**
	 * 推进转身相位机。
	 *
	 * 相位边界全部由本帧原动画源区间求值的 `CurveMotion` 决定。
	 */
	/** 当前输入是否构成"要转身"（跑动中输入接近反向）。 */
	bool IsReverseRunInput() const;

	/**
	 * 推进刹停的曲线接管。
	 *
	 * 判据是"没有移动输入，但曲线还在给速度"。除了 `_End` 这类刹停动画，
	 * 没有别的动画会在无输入时给出非零速度，所以不需要额外的下降沿检测。
	 *
	 * 已由 StageLocomotionCurveRootMotion 挂载的片段在原生 Prepare 中按实际区间求值；
	 * 本方法仅整理普通移动路径中不再使用的刹停源。
	 */
	void UpdateCurveBrake();

	/**
	 * 同步转身段的曲线位移源。
	 *
	 * 本地预测端与服务器权威端执行；模拟代理使用服务器复制的变换与
	 * `bReplicatedTurnBackCurveDriven` 表现语义，不在本地重复挂 RootMotionSource。
	 *
	 * 只覆盖 `Turning` / `Braking`。`RunOut` 段方向取玩家输入 —— 曲线在那一段给出的
	 * 方向恰好是入口朝向的反方向（角色转身后的正前方），玩家不改输入时两者一致，
	 * 改了就该跟输入走。
	 */
	void UpdateTurnBackRootMotion();

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

	friend struct FRootMotionSource_GGYGOActionCurve;
	bool ValidateMontageActionRuntime(const FRootMotionSource_GGYGOActionCurve& Source, FString& OutError) const;
	bool IsMontageActionMovementModeSupported() const;
	void FailMontageActionMotion(const TSharedPtr<const FGGYGOActionMotionResource>& OriginalResource, const FString& Error);
	void NeutralizeMontageActionSource(const TSharedPtr<const FGGYGOActionMotionResource>& OriginalResource);
	void ResumeLocomotionAfterAction();
	void PublishQualifiedMovementIntent();
	void RetireQualifiedMovementIntent(const FGGYGOMovementOwnerSyncScopeId& OriginalScope, FName Reason);
	struct FQualifiedMovementIntentObserver;
	TArray<TSharedPtr<FQualifiedMovementIntentObserver>> QualifiedMovementIntentObservers;
	uint64 QualifiedMovementIntentLastObserverSerial = 0;
	uint64 QualifiedMovementIntentPublicationSerial = 0;
	int32 QualifiedMovementIntentReplayDepth = 0;
	TSharedPtr<const FGGYGOActionMotionResource> ActiveMontageActionResource;
	/** At most the last completed resource, retained for its GA's End cancellation; no historical lookup. */
	TSharedPtr<const FGGYGOActionMotionResource> CompletedMontageActionResource;
	float ActionSkippedMovementTickTime = 0.0f;
	FGGYGOActionMotionFailureDelegate ActionMotionFailureCallback;
	TSharedPtr<const FGGYGOActionMotionResource> ActionMotionFailureResource;
	int32 MontageActionFailurePreparationDepth = 0;
	/** Exact last terminal token for idempotent release, without retaining a historical resource. */
	int32 LastRetiredMontageActionHandle = INDEX_NONE;

	/**
	 * 曲线是否正在接管转身的朝向与位移方向（`Turning` 或 `Braking`）。
	 *
	 * 这两段合成一个判定是因为它们的移动行为完全一致：方向都取曲线在入口坐标系里的
	 * 值，朝向都由转角增量累加。区别只在转角还在不在变，而那是表现层的事。
	 * 网络侧也只需要同步这一个事实，所以压缩标志位只发一个 bit。
	 */
	bool IsTurnBackCurveDriven() const;

protected:
	/** Authority-side ownership; source time is held only by the RMS. */
	int32 ActiveActionMotionHandle = INDEX_NONE;
	int32 NextActionMotionHandle = 1;
	uint16 ActionMotionSourceID = 0;

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

	/**
	 * 反向输入已被本次转身消费。
	 *
	 * 转身结束后玩家往往还按着同一个方向键，而那个方向此时已经是角色的正前方，
	 * 不该再触发一次转身。必须等输入离开反向阈值才允许下一次触发。
	 */
	bool bTurnBackInputLatched = false;

	/** 进入转身时的 Actor yaw（度）。曲线的段起点方向转世界方向的基准。 */
	float TurnBackEntryYaw = 0.0f;

	/**
	 * "曲线正在接管转身"的复制标志。
	 *
	 * SavedMove / MoveData 只在"客户端→服务器"方向传递，不会转发给其它客户端，
	 * 所以模拟代理无法从那条路径得知转身状态。服务器按自身动画源解算后写入本属性，
	 * 再由属性复制发给其它客户端。
	 *
	 * 只复制这一个 bool 而不是完整相位：`Turning` 与 `Braking` 的移动行为一致，
	 * `RunOut` 的移动与普通移动一致，所以三个相位对接收端只有两种含义。
	 */
	UPROPERTY(Replicated)
	bool bReplicatedTurnBackCurveDriven = false;

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
// Movement-LocalASC-A declarations begin.
protected:
	/** 生产 Cache 只订阅原 Extension；返回成功表示订阅成立，不表示资源 Ready。 */
	bool PrepareLocalAbilitySystemSubscription(UGGYGOPawnExtensionComponent* Extension, FString& OutError);
	/** Retire only this component's original subscription and consumed local resource. */
	void ReleaseLocalAbilitySystemSubscription();
	/** Derived query of the original ready resource; never grants movement execution. */
	const UGGYGOAbilitySystemComponent* GetReadyLocalAbilitySystemComponent() const;

private:
	struct FLocalAbilitySystemSubscription;
	void ConsumeLocalAbilitySystemNotice(
		const TSharedPtr<FLocalAbilitySystemSubscription>& ExpectedSubscription,
		const FGGYGOPawnASCLocalNotice& Notice);
	/** Actual delegate ownership only; no Binding allocator, Ready fact or movement gate. */
	TSharedPtr<FLocalAbilitySystemSubscription> LocalAbilitySystemSubscription;
// Movement-LocalASC-A declarations end.

};
