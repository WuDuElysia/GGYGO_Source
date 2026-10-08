/**
 * @file GGYGOMovementPrediction.h
 * @brief Movement native owner protocol, source checkpoints and saved-move transport.
 */
#pragma once

#include "Character/Data/GGYGOMovementTypes.h"
#include "Animation/Data/GGYGOLocomotionSourceBinding.h"
#include "Input/GGYGOMovementInputTypes.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/CharacterMovementReplication.h"

class UGGYGOCharacterMovementComponent;
class APawn;
class APlayerController;
struct FGGYGOCurveRootMotionMoveInput;
struct FGGYGOCurveRootMotionPrepared;

namespace GGYGOMovementPrediction
{
	/** Only ForceWalk is client input; locomotion outcomes are recomputed by the server. */
	constexpr uint8 ForceWalkFlag = FSavedMove_Character::FLAG_Custom_3;
}

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
 * 保存原输入、执行前相位与原生曲线结果；网络提示不授予服务端执行资格。
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
