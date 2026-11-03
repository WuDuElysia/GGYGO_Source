#pragma once

#include "Character/Data/GGYGOMovementTypes.h"
#include "Animation/Data/GGYGOLocomotionSourceBinding.h"

class UGGYGOLocomotionMotionProfile;

/** Pure interval values. Sample remains unscaled; Scale does not affect Yaw. */
struct FGGYGOLocomotionEvaluationResult
{
	FGGYGOLocomotionCurveSample Sample;
	float ScaledSpeed = 0.0f;
	FVector ScaledVelocity = FVector::ZeroVector;
};

/**
 * 评估器此刻实际读取的曲线数据表达；与 UAnimSequence::EvaluateCurveData_Lockless 的原生选路一致。
 * 两种表达的摘要不可直接比较：同一资产在编辑器异步压缩完成前后会从 EditorDataModel 切到 CompressedCodecStream。
 */
enum class EGGYGOLocomotionCurveContentKind : uint8
{
	None,
	/** 平台压缩数据有效：解码器类 + 与名称排序置换无关的"曲线名→缓冲槽位"规范映射 + 原样压缩曲线字节流。 */
	CompressedCodecStream,
	/** 仅编辑器、压缩数据尚未就绪：DataModel 中四条必需曲线的完整 RichCurve 编码。 */
	EditorDataModel
};

/** 源身份两部分分列；内容摘要只在同一表达下可比，不合并成一个"已认证"布尔。 */
struct FGGYGOLocomotionSourceIdentityDigest
{
	uint64 ConfigurationDigest = 0;
	uint64 CurveContentDigest = 0;
	EGGYGOLocomotionCurveContentKind ContentKind = EGGYGOLocomotionCurveContentKind::None;
};

/** EndCyclePosition is an unwrapped mathematical endpoint, not a committed clock. */
struct FGGYGOWalkRunEvaluationResult
{
	FGGYGOLocomotionEvaluationResult Motion;
	float EndCyclePosition = 0.0f;
};

namespace GGYGOLocomotionEvaluation
{
	/** Validate original metadata and required curve presence without retaining state. */
	GGYGO_API bool ValidateSource(const FGGYGOLocomotionSequenceSource& Source, FString& OutError);
	GGYGO_API bool ValidateBinding(const FGGYGOLocomotionSourceBinding& Binding, FString& OutError);

	/**
	 * 原 Sequence 源身份，均为平台无关的规范编码（小端定长整数、IEEE 位模式、长度前缀 UTF-8）。
	 * ConfigurationDigest：资产类与路径、MotionType/RouteKey/Loop/RateScale/PlayLength。
	 * CurveContentDigest：与评估器同一原生选路（FAnimExtractContext 默认平台数据）取实际被解码的数据——
	 * 压缩有效时为解码器类、按该解码器实际读取规则求出并按名称 UTF-8 字节序排序的"名称→槽位"映射
	 * （不受进程内 FName 排序置换影响；支持 CompressedRichCurve／UniformIndexable／UniformlySampled／ACL 的精确原生类，
	 * 其余明确失败）与原样 CompressedCurveByteStream；
	 * 同时要求四条必需曲线在所选压缩表中各出现一次、置换合法、头部与长度可被解码器读取；
	 * 编辑器压缩未就绪时为 DataModel 四条必需曲线的完整 RichCurve 编码。两种都不可得时失败。
	 * 不缓存；压缩完成、资产编辑都会改变结果，调用方须在每个需要认证的点重算。
	 */
	GGYGO_API bool ComputeSourceIdentity(const FGGYGOLocomotionSequenceSource& Source,
		FGGYGOLocomotionSourceIdentityDigest& OutIdentity, FString& OutError);

	/** Move seconds map through the source's authored rate. Uses runtime curve data, never the current pose. */
	GGYGO_API bool EvaluateSingleInterval(const FGGYGOLocomotionSequenceSource& Source,
		float StartTime, float EndTime, float RootMotionScale,
		FGGYGOLocomotionEvaluationResult& OutResult, FString* OutError = nullptr);

	/** Native BlendSpace weights/period on the CMC move context; this is not the AnimGraph node's filtered cache. */
	GGYGO_API bool EvaluateWalkRunInterval(const FGGYGOLocomotionSourceBinding& Binding,
		float StartCyclePosition, float AcceptedIntervalSeconds, float BlendAlpha, float RootMotionScale,
		FGGYGOWalkRunEvaluationResult& OutResult, FString* OutError = nullptr);

	// Historical Profile APIs below remain for migration tools and their original failure evidence.
	// Production movement exclusively consumes the original source overloads above.
	/**
	 * Delegate asset evaluation to Profile::EvaluateInterval, then check scaling.
	 * Authored zero, tiny positive speed and zero Scale are successful values.
	 * Reset the complete output and optional old Error on entry; publish only on
	 * success. Errors retain the Profile path, interval and native field/reason.
	 * Profiles are borrowed for this call; no state, logging or cache is retained.
	 */
	GGYGO_API bool EvaluateSingleInterval(
		const UGGYGOLocomotionMotionProfile* Profile,
		float StartTime,
		float EndTime,
		float RootMotionScale,
		FGGYGOLocomotionEvaluationResult& OutResult,
		FString* OutError = nullptr);

	/**
	 * Evaluate both required Loop Profiles on one cycle interval, then compose
	 * their values. Alpha must be in [0,1]; Scale and interval inputs must be
	 * finite and non-negative. Alpha endpoints still require BOTH evaluations
	 * to succeed, and retain the selected endpoint's exact unscaled sample.
	 *
	 * Effective period = the two durations blended by Alpha. EndCyclePosition =
	 * StartCyclePosition + AcceptedIntervalSeconds / effective period. A cycle
	 * coordinate can cross multiple periods; this function does not wrap it,
	 * clamp Delta, select a motion, or update a caller's time or phase.
	 *
	 * Positive unscaled speed requires a valid composed direction, even when
	 * Scale is zero. Native asset validation belongs to EvaluateInterval;
	 * this helper checks its own mapping, composition and scaling operations.
	 * Output/Error follow the same atomic reset contract as the single function.
	 */
	GGYGO_API bool EvaluateWalkRunInterval(
		const UGGYGOLocomotionMotionProfile* WalkProfile,
		const UGGYGOLocomotionMotionProfile* RunProfile,
		float StartCyclePosition,
		float AcceptedIntervalSeconds,
		float BlendAlpha,
		float RootMotionScale,
		FGGYGOWalkRunEvaluationResult& OutResult,
		FString* OutError = nullptr);
}
