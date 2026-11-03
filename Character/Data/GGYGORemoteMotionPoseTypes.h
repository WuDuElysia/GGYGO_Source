/**
 * @file GGYGORemoteMotionPoseTypes.h
 * @brief 权威 CMC 完成区间到远端观察端的只读姿态协议；不授予执行权、不推进任何时钟。
 *
 * 记录键取原生 ServerLastTransformUpdateTimeStamp（server 时域）。观察端只做相等配对：
 * 记录只有与同键、且由原生 SmoothCorrection 实参证明的呈现样本配对后才成为事实，不按键大小排序。
 * 新旧只用 PublicationSerial 判断；PublisherEpoch 划分发布端身份（控制者/远端角色/组件），
 * 同一 Epoch 内时间戳来源不变，Linear 插值只在同 Epoch 同段内使用键差。
 */
#pragma once

#include "CoreMinimal.h"
#include "Character/Data/GGYGOMovementTypes.h"
#include "Character/Data/GGYGOLocomotionSteeringTypes.h"
#include "Engine/NetSerialization.h"

#include "GGYGORemoteMotionPoseTypes.generated.h"

class UAnimSequence;

/** 记录类别；bKeyed 的记录只有与同键原生呈现样本配对后才成为观察端事实。 */
UENUM()
enum class EGGYGORemoteMotionPoseKind : uint8
{
	None,
	/** 单 Sequence 段的完成区间姿态。 */
	Sample,
	/** 该原生样本处没有单 Sequence 段（WalkRun、空闲、动作等）；每个新样本键都重新发布。 */
	NoSegment,
	/** 段被取消或失败；不携带姿态时间。 */
	Terminated,
	/** 本原生样本无法证明对应的完成区间；不替换已有事实。 */
	Unkeyed,
	/** 发布端协议不可用（键不复制、序号耗尽、源身份失败）。 */
	Invalid
};

/** 段结束方式；Completed 只来自原片非循环自然末端，网络等待不会产生任何结束。 */
UENUM()
enum class EGGYGORemoteMotionPoseClosure : uint8
{
	None, Completed, Cancelled, Failed
};

/** 发布端诊断原因；观察端据此输出可定位的失败或等待信息。 */
UENUM()
enum class EGGYGORemoteMotionPoseReason : uint8
{
	None,
	LaterMoveWithoutCompletion,
	TransformChangedAfterCompletion,
	NoNativeSampleKey,
	SourceIdentityFailed,
	SerialExhausted,
	ResourceRetired,
	RequestRetired,
	SourceInvalid,
	PublisherIdentityChanged
};

/**
 * 复制记录，整体 NetSerialize；字段按 Kind 条件序列化，未序列化字段在接收端为默认值。
 * SequenceStart/End 来自同一原完成区间（同段），已按原 Sequence RateScale 乘过一次并截到 PlayLength。
 * SampleLocation/Rotation 只用于与原生呈现做量化容差校验，不构成身份。
 */
USTRUCT()
struct GGYGO_API FGGYGORemoteMotionPoseRecord
{
	GENERATED_BODY()

	UPROPERTY() uint32 PublicationSerial = 0;
	UPROPERTY() uint32 PublisherEpoch = 0;
	UPROPERTY() float NativeSampleKey = 0.0f;
	UPROPERTY() bool bKeyed = false;
	UPROPERTY() EGGYGORemoteMotionPoseKind Kind = EGGYGORemoteMotionPoseKind::None;
	UPROPERTY() EGGYGORemoteMotionPoseClosure Closure = EGGYGORemoteMotionPoseClosure::None;
	UPROPERTY() EGGYGORemoteMotionPoseReason Reason = EGGYGORemoteMotionPoseReason::None;
	UPROPERTY() uint32 SegmentSerial = 0;
	UPROPERTY() uint64 ConfigurationDigest = 0;
	UPROPERTY() uint64 CurveContentDigest = 0;
	/** 发布端评估器实际读取的曲线表达：1=平台压缩解码流，2=编辑器 DataModel；只有同表达的摘要可比。 */
	UPROPERTY() uint8 CurveContentKind = 0;
	UPROPERTY() EGGYGOLocomotionMotionType MotionType = EGGYGOLocomotionMotionType::None;
	UPROPERTY() EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;
	UPROPERTY() double SequenceStartSeconds = 0.0;
	UPROPERTY() double SequenceEndSeconds = 0.0;
	/** true 时 SampleLocation/Rotation 为原生 BasedMovement 的相对值。 */
	UPROPERTY() bool bRelativeTransform = false;
	UPROPERTY() FVector_NetQuantize100 SampleLocation = FVector::ZeroVector;
	/** 网络上按 16 位分量压缩。 */
	UPROPERTY() FRotator SampleRotation = FRotator::ZeroRotator;
	/** 独立 camera facet，随同一 native 样本认证，不授单 Sequence 时间或本地执行权。 */
	UPROPERTY() uint8 CameraTurnStatusCode = static_cast<uint8>(EGGYGOLocomotionSteeringStatus::NotApplicable);
	UPROPERTY() float CameraControlYawDeltaDegrees = 0.0f;
	UPROPERTY() float CameraTurnDemandDegrees = 0.0f;
	UPROPERTY() EGGYGOGait Gait = EGGYGOGait::None;
	UPROPERTY() float WalkRunBlendAlpha = 0.0f;

	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
	/** 除发布序号外全部相等；发布端据此不为同一事实伪发新记录。 */
	bool HasSameContent(const FGGYGORemoteMotionPoseRecord& Other) const;
};

template<>
struct TStructOpsTypeTraits<FGGYGORemoteMotionPoseRecord> : public TStructOpsTypeTraitsBase2<FGGYGORemoteMotionPoseRecord>
{
	enum { WithNetSerializer = true };
};

/**
 * 观察端状态。只有 Current 允许进入段与使用新时间；Held 只允许继续已进入的同段并保持认证终点。
 * 两者都要求本端评估器实际读取的曲线内容与发布端同表达且摘要相等。
 */
enum class EGGYGORemoteMotionPoseStatus : uint8
{
	/** 本地执行角色，使用 GetMotionExecutionSnapshot。 */
	NotRemote,
	Initial,
	/** 当前没有可用的已认证事实，原因见 WaitReason。 */
	Awaiting,
	/** 已认证样本，且就是最新原生呈现样本；本端源身份为曲线内容级。 */
	Current,
	/** 已认证样本，但之后已有新的原生呈现而对应记录尚未配对；不是结束，也不授予新时间/入场。 */
	Held,
	/** 该样本处没有单 Sequence 段，或段被取消；不携带时间，不是 Completed。 */
	NotApplicable,
	/** 已证明失败：段失败、源身份不一致、发布端协议不可用。 */
	Invalid
};

enum class EGGYGORemoteMotionPoseWaitReason : uint8
{
	None,
	/** 记录尚未与其原生呈现样本配对。 */
	NativeSample,
	/** 最新原生呈现不可认证（根运动接收路径、非原生复制呈现调用或变换校验失败）。 */
	NativeSampleUnverified,
	/** 本端动画源尚未发布。 */
	ProviderNotPublished,
	/** 本端动画源正随原生生命周期退役/换绑，或换绑后尚未重验。 */
	ProviderRetiring,
	/** 两端评估器当前读取的曲线表达不同（编辑器异步压缩未完成一侧仍读 DataModel），等待原生压缩生命周期。 */
	ProviderContentPending
};

/** 已认证样本所依据的曲线内容表达。 */
enum class EGGYGORemoteMotionPoseIdentity : uint8
{
	None,
	/** 两端同为平台压缩解码流且解码器类、名称索引表、字节流一致。 */
	CompressedCurveContent,
	/** 两端同为编辑器 DataModel，四条必需曲线完整 RichCurve 编码一致。 */
	EditorModelCurveContent
};

/** SequenceSeconds 所在时域。 */
enum class EGGYGORemoteMotionPoseTimeDomain : uint8
{
	None,
	/** 权威完成区间终点，对应 NativeSampleKey 的原生样本。 */
	AuthorityCompletedEndpoint,
	/** 原生 Linear 显示坐标在同 Epoch 同段两个已认证样本之间的插值，不越过最新认证终点。 */
	NativeLinearDisplay
};

/**
 * GT 查询结果。Sequence 来自观察端当前动画源绑定，配置与评估器实际曲线内容均已与发布端核对（表达见 Identity）。
 * TurnBackPhase/bTurnBackCurveDriven/bTurnBackRunOut 与姿态出自同一条已接受记录。
 * NativeSampleKey 属 server 时域；AcceptedLocalFrame 属本端 GFrameCounter，两者不可互换。
 */
struct GGYGO_API FGGYGORemoteMotionPoseSnapshot
{
	EGGYGORemoteMotionPoseStatus Status = EGGYGORemoteMotionPoseStatus::Initial;
	EGGYGORemoteMotionPoseWaitReason WaitReason = EGGYGORemoteMotionPoseWaitReason::None;
	EGGYGORemoteMotionPoseIdentity Identity = EGGYGORemoteMotionPoseIdentity::None;
	EGGYGORemoteMotionPoseTimeDomain TimeDomain = EGGYGORemoteMotionPoseTimeDomain::None;
	EGGYGORemoteMotionPoseClosure Closure = EGGYGORemoteMotionPoseClosure::None;
	bool bEnded = false;
	/**
	 * 仅当 Status==Awaiting 且原因是本端动画源资格（ProviderNotPublished/ProviderRetiring/ProviderContentPending），
	 * 而已接受记录是同 Epoch 的认证 Sample 时为 true。此时 MotionType/TurnBackPhase/CurveDriven/RunOut/Closure/bEnded
	 * 与 PublisherEpoch/SegmentSerial 是该段的历史已认证事实；不提供 Sequence、任何时间或 TimeDomain，
	 * 不授予入场、新时间、本地执行或 Finished。只允许已实际原生消费同 PublisherEpoch+SegmentSerial 的节点保持其原生输出姿态等待；
	 * bEnded 只表示该段已认证自然末端，可用于关闭判定，不得据此新造终点姿态。其它任何状态恒为 false。
	 */
	bool bSegmentFactsRetained = false;
	EGGYGOLocomotionMotionType MotionType = EGGYGOLocomotionMotionType::None;
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;
	bool bTurnBackCurveDriven = false;
	bool bTurnBackRunOut = false;
	TWeakObjectPtr<UAnimSequence> Sequence;
	double SequenceSeconds = 0.0;
	double CertifiedSequenceSeconds = 0.0;
	uint32 SegmentSerial = 0;
	uint32 PublisherEpoch = 0;
	uint32 PublicationSerial = 0;
	float NativeSampleKey = 0.0f;
	uint64 AcceptedLocalFrame = 0;
	EGGYGOLocomotionSteeringStatus CameraTurnStatus = EGGYGOLocomotionSteeringStatus::Initial;
	float CameraControlYawDeltaDegrees = 0.0f;
	float CameraTurnDemandDegrees = 0.0f;
	EGGYGOGait CameraTurnGait = EGGYGOGait::None;
	float CameraTurnWalkRunBlendAlpha = 0.0f;
	FString CameraTurnDiagnostic;
	FString Diagnostic;
};
