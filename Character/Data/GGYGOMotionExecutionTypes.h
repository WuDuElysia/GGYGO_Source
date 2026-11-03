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
class UWorld;
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
	/** 原完成区间生产点的 World 事件采样；不是 cue 内部连续时间映射。不可由接收端补 Now。 */
	bool bHasCompletionWorldTime = false;
	TWeakObjectPtr<UWorld> CompletionWorld;
	double CompletionWorldTimeSeconds = 0.0;
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

/**
 * 原 CMC 签出的只读来源观察值；不延长 Origin、资产或输入请求寿命。
 * 历史相等只供关闭原资源；活源资格须回原 issuer 调用 IsLocomotionSourceProofCurrent。
 */
class GGYGO_API FGGYGOLocomotionSourceProof
{
public:
	/** 观察身份存在；原 Origin 过期后仍可为 true，不代表当前许可。 */
	bool IsSet() const { return Observer.IsValid() && !Issuer.IsExplicitlyNull(); }
	/** 原 issuer 仍可取得时返回它；不得在失效后改取新 Character 的 CMC。 */
	UGGYGOCharacterMovementComponent* GetIssuer() const;
	/** 默认空不相等；原 weak 控制块随共享观察值保留，过期来源仍能精确清理。 */
	bool IsSameSource(const FGGYGOLocomotionSourceProof& Other) const;
private:
	friend class UGGYGOCharacterMovementComponent;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> Issuer;
	TSharedPtr<const TWeakPtr<const FGGYGOCurveRootMotionOrigin>> Observer;
};

/**
 * 原来源真实退休的同步事实。不能用它给新来源入场，也不将 NA、RMS 归还或等待转为结束。
 * FinalInterval 仅在原源有真实自然末区间时提供，消费者先对原已拥有来源去重消费，再正常关闭。
 * 事件值仅供同步消费；资源持有者保存 SourceProof，按原来源幂等清理。
 */
struct GGYGO_API FGGYGOLocomotionSourceRetirementFact
{
	FGGYGOLocomotionSourceProof SourceProof;
	EGGYGOMotionExecutionEndReason EndReason = EGGYGOMotionExecutionEndReason::None;
	FName Cause;
	FString Diagnostic;
	bool bHasFinalInterval = false;
	FGGYGOMotionExecutionSnapshot FinalInterval;
private:
	friend class UGGYGOCharacterMovementComponent;
	/** 保证同步派发期间原 weak 来源可观察；不作为消费者的长期资源。 */
	TSharedPtr<const FGGYGOCurveRootMotionOrigin> OriginalOrigin;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FGGYGOLocomotionSourceRetiredDelegate,
	const FGGYGOLocomotionSourceRetirementFact&);
