/**
 * @file GGYGOAnimationStateFrame.h
 * @brief C++ 权威层向动画蓝图发布的一帧只读语义事实
 */
#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Character/Data/GGYGOMovementTypes.h"
#include "Character/Data/GGYGOLocomotionSteeringTypes.h"
#include "Character/Data/GGYGOMotionExecutionTypes.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "GGYGOAnimationStateFrame.generated.h"

class UAnimSequence;
class UAnimMontage;

/** 原 Steering 区间的编辑器只读数值视图；不携带 weak 身份或新增执行状态。 */
USTRUCT()
struct FGGYGOAnimationSteeringFrame
{
	GENERATED_BODY()

	/** EGGYGOLocomotionSteeringStatus 的原始 enum 值，仅用于原生诊断。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Steering")
	uint8 Status = static_cast<uint8>(EGGYGOLocomotionSteeringStatus::Initial);

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Steering")
	uint64 SourceEpoch = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Steering")
	uint64 CompletedIntervalSerial = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Steering")
	uint64 CompletedFrame = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Steering")
	float NativeDeltaSeconds = 0.0f;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Steering")
	float DesiredDirectionError = 0.0f;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Steering")
	float ActualSignedYawRate = 0.0f;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Steering")
	float ActualSignedVelocityYawRate = 0.0f;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Steering")
	bool bHasVelocityYawRate = false;
};

/** 原 CMC 完成区间的表现视图；无 weak 身份、binding、RMS 或动作资源执行权。 */
USTRUCT(BlueprintType)
struct FGGYGOAnimationMotionFrame
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	EGGYGOMotionExecutionStatus Status = EGGYGOMotionExecutionStatus::Initial;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	EGGYGOMotionExecutionSource SourceKind = EGGYGOMotionExecutionSource::None;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	EGGYGOMotionRotationMode RotationMode = EGGYGOMotionRotationMode::TranslationOnly;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	EGGYGOMotionExecutionEndReason EndReason = EGGYGOMotionExecutionEndReason::None;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	EGGYGOLocomotionMotionType MotionType = EGGYGOLocomotionMotionType::None;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;

	/** 仅保留用于姿态求值的资产引用；本帧重置及原生命周期清理时释放。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	TObjectPtr<UAnimSequence> SingleSequence = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	TObjectPtr<UAnimMontage> Montage = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	bool bHasSingleSequenceTime = false;

	/** 已经是准确原 Sequence 坐标，消费者不能再次乘 RateScale。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	double SequenceStartSeconds = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	double SequenceEndSeconds = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	float MotionStartSeconds = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	float MotionEndSeconds = 0.0f;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Motion")
	float NativeDeltaSeconds = 0.0f;

	/** 未折叠曲线请求与真实 Quaternion 分列，不从最短角反推绕转次数。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	float RequestedYawDeltaDegrees = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	bool bCurveYawApplied = false;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	FQuat RotationBefore = FQuat::Identity;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	FQuat RotationAfter = FQuat::Identity;

	/** RMS 交还不等于源动画片段结束。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	bool bNativeSourceFinished = false;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	bool bEnded = false;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	FString Diagnostic;

	// 原完成区间派生关联；不签发播放/执行许可，也不在 Animation 累计。
	// 非 Blueprint 数值类型仅供原生/编辑器准确读回，不转换或截短原 serial。
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Motion")
	uint64 ExecutionRequestSerial = 0;
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Motion")
	uint16 MotionSequence = 0;
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Motion")
	int32 ActionHandle = INDEX_NONE;
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Motion")
	uint64 CompletedIntervalSerial = 0;
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Motion")
	uint64 CompletedFrame = 0;
	/** 复合 Montage 原区间数值，不压成一个 SequenceTime。 */
	TArray<FGGYGOActionMotionSourceInterval> SourceIntervals;
};

/**
 * 动画表现层的一帧输入。
 *
 * 这里只描述角色在本帧真实发生了什么，不描述 AnimBP 有哪些状态、节点或过渡线。
 * 数据在游戏线程集中抓取，随后只读地交给动画线程与 AnimGraph 使用。
 */
USTRUCT(BlueprintType)
struct FGGYGOAnimationStateFrame
{
	GENERATED_BODY()

	/** 角色当前世界速度。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	FVector WorldVelocity = FVector::ZeroVector;

	/** 本帧 CMC 使用的世界加速度。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	FVector WorldAcceleration = FVector::ZeroVector;

	/** 水平速度大小，单位 cm/s。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	float HorizontalSpeed = 0.0f;

	/** 水平速度的世界空间单位方向。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	FVector WorldVelocityDirection = FVector::ZeroVector;

	/** 实际水平速度相对角色朝向的角度：0 为前，+90 为右。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	float LocalVelocityAngle = 0.0f;

	/** CMC 标准局部速度轴：X 为前，Y 为右；兼容 AnimBP 的轴交换在旧快照适配处完成。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	FVector2D LocalVelocityBlend = FVector2D::ZeroVector;

	/** Movement 权威的 WalkRun BlendSpace 混合值：0 = Walk，1 = Run。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	float WalkRunBlendAlpha = 0.0f;

	/** False before a Character context exists; this is not a completed zero-rate sample. */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	bool bLocomotionSteeringCaptured = false;

	/** Native immutable interval fact. Only GT interprets weak identity; graph workers read values. */
	FGGYGOLocomotionSteeringSnapshot LocomotionSteering;

	/** 同次 GT Capture 中从 LocomotionSteering 提取；随本帧及原生命周期一起清理。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|Steering")
	FGGYGOAnimationSteeringFrame SteeringExecution;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion")
	FGGYGOAnimationMotionFrame MotionExecution;

	/** GT 从同一角色胶囊及 Mesh 变换换算的姿态基准；不授予朝向写入权。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	bool bCapsuleMeshBasisValid = false;

	/** 正方向误差向胶囊右侧倾身时的旋转轴，已经换算到 Mesh component space。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	FVector LeanAxisCS = FVector::ZeroVector;

	/** 胶囊上方向，已经换算到 Mesh component space。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	FVector CapsuleUpCS = FVector::ZeroVector;

	/** Movement 权威的停止动作语义，供旧 AnimBP 分支索引适配。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	EGGYGOStopMotionType StopMotionType = EGGYGOStopMotionType::None;

	/** CMC 已解算的步态。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	EGGYGOGait Gait = EGGYGOGait::None;

	/** 引擎移动模式。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	TEnumAsByte<EMovementMode> MovementMode = MOVE_None;

	/** CMC 权威转身动作的当前相位。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;

	/** 当前是否存在可预测的移动输入。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	bool bHasMoveInput = false;

	/** 水平速度是否超过移动判定阈值。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	bool bMovingHorizontally = false;

	/** 角色是否处于地面移动模式。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	bool bGrounded = true;

	/** GAS 是否禁止普通移动。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	bool bMovementBlocked = false;

	/** 转身是否已经进入把方向交回输入的 RunOut 段。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	bool bTurnBackRunOut = false;

	/** ASC 在本帧拥有的状态 Tag，只供表现层查询，不允许反向写入。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|State")
	FGameplayTagContainer OwnedStateTags;
};
