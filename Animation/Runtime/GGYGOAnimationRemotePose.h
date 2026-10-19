/**
 * @file GGYGOAnimationRemotePose.h
 * @brief 远端观察端的权威复制姿态在 Animation 的一帧表现视图、真实节点消费证据与已进入段。
 *
 * 姿态来源只有 UGGYGOCharacterMovementComponent::GetRemoteMotionPoseSnapshot（GT）。这里不推进时间、
 * 不外推、不再乘 RateScale，也不持有 CMC/Provider 资源；图工作线程只读本帧复制出的值。
 *
 * 段进入证据的数据流（无额外 Tick/计时器/时钟）：
 *  1. GT 捕获：Resolve 生成本帧视图并附带 PublicationToken；Held 与 Provider 资格保留等待只对已进入段的同一消费节点开放。
 *  2. 工作线程图更新：实际被更新的 SequenceEvaluator 在其 UpdateFunction 中把姿态写入自身，并以
 *     RecordConsumption 记录（Token、Epoch、Segment、节点）。纯查询从不记录。
 *  3. 下一次 GT 捕获：仅当原生图在两次捕获之间确实更新过时，InheritConsumption 才据记录确认或清除已进入段。
 */
#pragma once

#include "CoreMinimal.h"
#include "Character/Data/GGYGOMovementTypes.h"
#include "Character/Data/GGYGORemoteMotionPoseTypes.h"

#include "GGYGOAnimationRemotePose.generated.h"

class UAnimSequence;

/**
 * AuthorityReplicatedPose：一帧 GT 捕获结果。bRemote=false 时姿态查询使用本机完成区间（LocalExecution）。
 * 只记录姿态源、时间与同记录相位；不含胶囊变换、Quaternion 或执行增量，缺失值不以 0 代替。
 */
USTRUCT()
struct GGYGO_API FGGYGOAnimationRemotePoseFrame
{
	GENERATED_BODY()

	/** 本角色是远端观察端（Movement 判定）；为 false 时其余字段保持默认。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	bool bRemote = false;

	/** EGGYGORemoteMotionPoseStatus 等 Movement 协议枚举的原始值，仅供原生诊断与观察读回。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	uint8 Status = 0;
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	uint8 WaitReason = 0;
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	uint8 Identity = 0;
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	uint8 TimeDomain = 0;
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	uint8 Closure = 0;

	/** 可提供同段姿态：Current，或已由真实节点消费进入的同段 Held。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	bool bHasPose = false;

	/** 可进入新段：仅 Current 且未自然结束。Held 与终点姿态都不授予入场。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	bool bAdmissible = false;

	/**
	 * Provider 资格合法等待（Awaiting 且 bSegmentFactsRetained），且已有真实节点消费进入同 Epoch/段。
	 * 只允许该节点保持原生输出姿态；无 Sequence/时间，不可采样、不入场。相位与 bEnded 为该段保留的认证事实。
	 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	bool bRetainedWait = false;

	/** Movement 给出 Current/Held，但本端拒绝使用（Held 无同段进入证据，或跨边界值无效）；诊断见 Diagnostic。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	bool bConsumerRejected = false;

	/** 仅原片非循环自然末端；网络等待、取消不产生结束。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	bool bEnded = false;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	EGGYGOLocomotionMotionType MotionType = EGGYGOLocomotionMotionType::None;

	/** 与姿态出自同一条已接受记录；无姿态时为 None。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	bool bTurnBackRunOut = false;

	/** GT 解析后的强引用；本帧覆盖及生命周期复位时释放。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	TObjectPtr<UAnimSequence> Sequence = nullptr;

	/** 原 Sequence 坐标（Movement 已乘过一次 RateScale）；Current 取 SequenceSeconds，Held 取 CertifiedSequenceSeconds。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	double SequenceSeconds = 0.0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	uint32 PublisherEpoch = 0;
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	uint32 SegmentSerial = 0;
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	uint32 PublicationSerial = 0;

	/** server 时域原生样本键；不可与本机帧号互换。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	float NativeSampleKey = 0.0f;

	/** 本机 GFrameCounter 域的接受帧。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	uint64 AcceptedLocalFrame = 0;

	/** 本 AnimInstance 每次 GT 捕获单调递增的发布号；只把消费记录关联到它读取的那一帧，不是时间。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	uint64 PublicationToken = 0;

	/** Held 或保留等待时唯一允许继续的消费节点（本实例内节点地址身份，不解引用）；其它情况为 0。 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	uint64 HeldConsumerNode = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Animation|RemotePose")
	FString Diagnostic;

	/** 线程安全只读查询；不记录消费。false 时不得写播放器时间。 */
	bool TryGetPose(EGGYGOLocomotionMotionType InMotionType, UAnimSequence*& OutSequence,
		float& OutSequenceSeconds, bool& bOutEnded) const;

	/** 指定消费节点可应用的姿态：Current 任意节点；Held 仅为已进入该段的同一节点。 */
	bool TryGetPoseForConsumer(EGGYGOLocomotionMotionType InMotionType, uint64 ConsumerNode,
		UAnimSequence*& OutSequence, float& OutSequenceSeconds, bool& bOutEnded) const;

	/** 线程安全只读：是否允许以该运动类型进入新段。 */
	bool IsAdmissible(EGGYGOLocomotionMotionType InMotionType) const;

	/** 该节点是保留等待中的原进入节点：只记录参与、保持原输出，不写姿态。 */
	bool IsRetainedWaitConsumer(EGGYGOLocomotionMotionType InMotionType, uint64 ConsumerNode) const;

	/** 已进入段的认证自然末端事实（可采样终点或保留等待）；只用于关闭判定，不产生姿态。 */
	bool HasEndedFact(EGGYGOLocomotionMotionType InMotionType) const;
};

/**
 * 工作线程图更新期间由真实应用了姿态的节点写入；GT 只在下一次捕获读取并立即清空。
 * 同一 AnimInstance 的图更新单线程执行，且 UE 在下一次 GT NativeUpdate 前完成上一轮并行更新。
 */
struct FGGYGOAnimationRemotePoseWitness
{
	uint64 PublicationToken = 0;
	uint32 PublisherEpoch = 0;
	uint32 SegmentSerial = 0;
	uint64 ConsumerNode = 0;
	bool bSet = false;
	/** 同一轮有不同节点或不同段消费；不能确定唯一进入者，拒绝后续 Held。 */
	bool bConflict = false;
};

/**
 * 已被真实节点以 Current/Held 消费进入的段（GT 拥有）。来源只有上一轮图更新的消费记录；
 * 不是时钟，不延长 Movement 事实有效期。生命周期复位、换 Provider/段/Epoch、图更新后无消费记录、
 * 或 Current/Held/保留事实的 Awaiting 之外的任何状态都会清除。
 */
struct FGGYGOAnimationRemotePoseContinuity
{
	TWeakObjectPtr<const UObject> Provider;
	uint32 PublisherEpoch = 0;
	uint32 SegmentSerial = 0;
	uint64 ConsumerNode = 0;
	bool bSet = false;

	void Reset() { *this = FGGYGOAnimationRemotePoseContinuity(); }
};

namespace GGYGOAnimationRemotePose
{
	/**
	 * GT：按上一轮图更新的真实消费记录确认或清除已进入段。
	 * @param bGraphUpdated 两次捕获之间原生图是否完成过更新；未更新时没有新证据，已进入段保持不变。
	 * @param PreviousToken / PreviousProvider 上一帧发布的视图及其原 CMC。
	 */
	GGYGO_API void InheritConsumption(bool bGraphUpdated, uint64 PreviousToken, const UObject* PreviousProvider,
		const FGGYGOAnimationRemotePoseWitness& Witness, FGGYGOAnimationRemotePoseContinuity& Continuity);

	/**
	 * GT：把 Movement 快照转为本帧视图；只清除与本快照矛盾的已进入段，从不据快照本身建立进入。
	 * @param Provider 产生该快照的原 CMC，仅用作身份比较，不被保存为强引用。
	 */
	GGYGO_API void Resolve(const FGGYGORemoteMotionPoseSnapshot& Snapshot, const UObject* Provider, uint64 PublicationToken,
		FGGYGOAnimationRemotePoseContinuity& Continuity, FGGYGOAnimationRemotePoseFrame& OutFrame);

	/** 工作线程：节点已把本帧姿态写入自身后调用，记录真实消费。 */
	GGYGO_API void RecordConsumption(const FGGYGOAnimationRemotePoseFrame& Frame, uint64 ConsumerNode,
		FGGYGOAnimationRemotePoseWitness& Witness);
}
