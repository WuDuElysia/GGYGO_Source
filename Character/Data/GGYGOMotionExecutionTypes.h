/**
 * @file GGYGOMotionExecutionTypes.h
 * @brief CMC 完成原生运动区间后发布的只读事实；不授予执行权或推进时钟。
 */
#pragma once

#include "CoreMinimal.h"
#include "Character/Data/GGYGOMovementTypes.h"
#include "Animation/Data/GGYGOActionMotionSourceBinding.h"
#include "Animation/Data/GGYGOLocomotionSourceBinding.h"

#include "GGYGOMotionExecutionTypes.generated.h"

class ACharacter;
class USceneComponent;
class UGGYGOCharacterMovementComponent;
struct FRootMotionSource;
struct FGGYGOActionMotionResource;
struct FGGYGOCurveRootMotionOrigin;

/** 原完整源明确支持的旋转合同；无 Yaw 的完整源是正常平移模式。 */
UENUM(BlueprintType)
enum class EGGYGOMotionRotationMode : uint8
{
	TranslationOnly, SourceYaw
};

UENUM(BlueprintType)
enum class EGGYGOMotionExecutionStatus : uint8
{
	Initial, Valid, NotApplicable, Invalid
};

UENUM(BlueprintType)
enum class EGGYGOMotionExecutionSource : uint8
{
	None, Locomotion, Montage
};

UENUM(BlueprintType)
enum class EGGYGOMotionExecutionEndReason : uint8
{
	None, Completed, Cancelled, Failed
};

/**
 * 游戏线程查询结果。原绑定与资源只供 GT 核验，Animation 必须提取值后交给 worker。
 * Valid 仅指原完成区间仍属于同一来源、当前或上一帧且实际结果未被改写。
 * 曲线请求角度保留未折叠度数；真实 Quaternion 不证明一帧内的绕转次数。
 */
struct GGYGO_API FGGYGOMotionExecutionSnapshot
{
	EGGYGOMotionExecutionStatus Status = EGGYGOMotionExecutionStatus::Initial;
	EGGYGOMotionExecutionSource SourceKind = EGGYGOMotionExecutionSource::None;
	EGGYGOMotionRotationMode RotationMode = EGGYGOMotionRotationMode::TranslationOnly;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> OriginalMovement;
	TWeakObjectPtr<ACharacter> OriginalCharacter;
	TWeakObjectPtr<USceneComponent> OriginalUpdatedComponent;
	FGGYGOLocomotionSourceBindingPtr OriginalLocomotionBinding;
	FGGYGOActionMotionSourceBindingPtr OriginalActionBinding;
	TSharedPtr<const FGGYGOCurveRootMotionOrigin> OriginalLocomotionOrigin;
	TSharedPtr<const FGGYGOActionMotionResource> OriginalActionResource;
	TSharedPtr<const FRootMotionSource> OriginalNativeSource;
	TWeakObjectPtr<const UAnimSequence> OriginalSequence;
	uint64 ExecutionRequestSerial = 0;
	uint16 MotionSequence = 0;
	int32 ActionHandle = INDEX_NONE;
	EGGYGOLocomotionMotionType MotionType = EGGYGOLocomotionMotionType::None;
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;
	uint64 CompletedIntervalSerial = 0;
	uint64 CompletedFrame = 0;
	float NativeDeltaSeconds = 0.0f;
	/** 只有原 RMS 区间具有绝对 native source 坐标；普通 CMC 不伪造 RMS 时钟。 */
	bool bHasNativeSourceInterval = false;
	float NativeStartSeconds = 0.0f;
	float NativeEndSeconds = 0.0f;
	/** 单段为原运动时间，Montage 为原绝对 Montage 时间。 */
	float MotionStartSeconds = 0.0f;
	float MotionEndSeconds = 0.0f;
	bool bHasSingleSequenceTime = false;
	double SequenceStartSeconds = 0.0;
	double SequenceEndSeconds = 0.0;
	/** 复合 Montage 原分段及 loop 坐标，不能压成一个 SequenceTime。 */
	TArray<FGGYGOActionMotionSourceInterval> SourceIntervals;
	float StartCyclePosition = 0.0f;
	float EndCyclePosition = 0.0f;
	float RequestedYawDeltaDegrees = 0.0f;
	bool bCurveYawApplied = false;
	/** 旋转执行入口前后实际 UpdatedComponent 朝向；位置为完整移动区间前后。 */
	FQuat RotationBefore = FQuat::Identity;
	FQuat RotationAfter = FQuat::Identity;
	FVector LocationBefore = FVector::ZeroVector;
	FVector LocationAfter = FVector::ZeroVector;
	FVector WorldVelocity = FVector::ZeroVector;
	/** RMS 交还不等于原动画源结束。 */
	bool bNativeSourceFinished = false;
	bool bEnded = false;
	EGGYGOMotionExecutionEndReason EndReason = EGGYGOMotionExecutionEndReason::None;
	FString Diagnostic;
};
