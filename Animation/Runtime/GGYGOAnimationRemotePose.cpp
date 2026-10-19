/**
 * @file GGYGOAnimationRemotePose.cpp
 * @brief 远端权威复制姿态快照到 Animation 表现视图的纯转换、节点消费记录与已进入段
 */
#include "Animation/Runtime/GGYGOAnimationRemotePose.h"

#include "Animation/AnimSequence.h"

bool FGGYGOAnimationRemotePoseFrame::TryGetPose(EGGYGOLocomotionMotionType InMotionType,
	UAnimSequence*& OutSequence, float& OutSequenceSeconds, bool& bOutEnded) const
{
	OutSequence = nullptr;
	OutSequenceSeconds = 0.0f;
	bOutEnded = false;
	if (!bHasPose || MotionType != InMotionType || !Sequence)
	{
		return false;
	}
	OutSequence = Sequence;
	OutSequenceSeconds = static_cast<float>(SequenceSeconds);
	bOutEnded = bEnded;
	return true;
}

bool FGGYGOAnimationRemotePoseFrame::IsRetainedWaitConsumer(EGGYGOLocomotionMotionType InMotionType, uint64 ConsumerNode) const
{
	return bRetainedWait && ConsumerNode != 0 && ConsumerNode == HeldConsumerNode && MotionType == InMotionType;
}

bool FGGYGOAnimationRemotePoseFrame::HasEndedFact(EGGYGOLocomotionMotionType InMotionType) const
{
	return (bHasPose || bRetainedWait) && bEnded && MotionType == InMotionType;
}

bool FGGYGOAnimationRemotePoseFrame::TryGetPoseForConsumer(EGGYGOLocomotionMotionType InMotionType, uint64 ConsumerNode,
	UAnimSequence*& OutSequence, float& OutSequenceSeconds, bool& bOutEnded) const
{
	const bool bHeld = Status == static_cast<uint8>(EGGYGORemoteMotionPoseStatus::Held);
	if (ConsumerNode == 0 || (bHeld && ConsumerNode != HeldConsumerNode))
	{
		OutSequence = nullptr;
		OutSequenceSeconds = 0.0f;
		bOutEnded = false;
		return false;
	}
	return TryGetPose(InMotionType, OutSequence, OutSequenceSeconds, bOutEnded);
}

bool FGGYGOAnimationRemotePoseFrame::IsAdmissible(EGGYGOLocomotionMotionType InMotionType) const
{
	return bAdmissible && bHasPose && MotionType == InMotionType && Sequence;
}

void GGYGOAnimationRemotePose::InheritConsumption(bool bGraphUpdated, uint64 PreviousToken, const UObject* PreviousProvider,
	const FGGYGOAnimationRemotePoseWitness& Witness, FGGYGOAnimationRemotePoseContinuity& Continuity)
{
	check(IsInGameThread());
	if (!bGraphUpdated)
	{
		// 原生图未更新（首帧、跳过更新）时没有新的参与或缺席证据。
		return;
	}
	if (!Witness.bSet || Witness.bConflict || PreviousToken == 0 || Witness.PublicationToken != PreviousToken
		|| !PreviousProvider)
	{
		// 图已更新但没有节点消费上一帧姿态：已进入的状态已离开或未被更新，不再保持。
		Continuity.Reset();
		return;
	}
	Continuity.Provider = PreviousProvider;
	Continuity.PublisherEpoch = Witness.PublisherEpoch;
	Continuity.SegmentSerial = Witness.SegmentSerial;
	Continuity.ConsumerNode = Witness.ConsumerNode;
	Continuity.bSet = true;
}

void GGYGOAnimationRemotePose::RecordConsumption(const FGGYGOAnimationRemotePoseFrame& Frame, uint64 ConsumerNode,
	FGGYGOAnimationRemotePoseWitness& Witness)
{
	// 等待帧只接受原进入节点自身的参与记录；它保持原输出姿态，不写节点。
	const bool bWaitingOwner = Frame.bRetainedWait && ConsumerNode == Frame.HeldConsumerNode;
	if ((!Frame.bHasPose && !bWaitingOwner) || Frame.PublicationToken == 0 || ConsumerNode == 0)
	{
		return;
	}
	if (Witness.bSet && Witness.PublicationToken == Frame.PublicationToken)
	{
		if (Witness.ConsumerNode != ConsumerNode || Witness.PublisherEpoch != Frame.PublisherEpoch
			|| Witness.SegmentSerial != Frame.SegmentSerial)
		{
			Witness.bConflict = true;
		}
		return;
	}
	Witness.PublicationToken = Frame.PublicationToken;
	Witness.PublisherEpoch = Frame.PublisherEpoch;
	Witness.SegmentSerial = Frame.SegmentSerial;
	Witness.ConsumerNode = ConsumerNode;
	Witness.bSet = true;
	Witness.bConflict = false;
}

void GGYGOAnimationRemotePose::Resolve(const FGGYGORemoteMotionPoseSnapshot& Snapshot, const UObject* Provider,
	uint64 PublicationToken, FGGYGOAnimationRemotePoseContinuity& Continuity, FGGYGOAnimationRemotePoseFrame& OutFrame)
{
	check(IsInGameThread());
	OutFrame = FGGYGOAnimationRemotePoseFrame();
	OutFrame.PublicationToken = PublicationToken;
	if (Snapshot.Status == EGGYGORemoteMotionPoseStatus::NotRemote)
	{
		// 本机执行角色：姿态查询回到 GetMotionExecutionSnapshot 的完成区间，不保留远端进入证据。
		Continuity.Reset();
		return;
	}
	OutFrame.bRemote = true;
	OutFrame.Status = static_cast<uint8>(Snapshot.Status);
	OutFrame.WaitReason = static_cast<uint8>(Snapshot.WaitReason);
	OutFrame.Identity = static_cast<uint8>(Snapshot.Identity);
	OutFrame.TimeDomain = static_cast<uint8>(Snapshot.TimeDomain);
	OutFrame.Closure = static_cast<uint8>(Snapshot.Closure);
	OutFrame.PublisherEpoch = Snapshot.PublisherEpoch;
	OutFrame.SegmentSerial = Snapshot.SegmentSerial;
	OutFrame.PublicationSerial = Snapshot.PublicationSerial;
	OutFrame.NativeSampleKey = Snapshot.NativeSampleKey;
	OutFrame.AcceptedLocalFrame = Snapshot.AcceptedLocalFrame;
	OutFrame.Diagnostic = Snapshot.Diagnostic;

	const bool bCurrent = Snapshot.Status == EGGYGORemoteMotionPoseStatus::Current;
	const bool bRetainedWait = Snapshot.Status == EGGYGORemoteMotionPoseStatus::Awaiting && Snapshot.bSegmentFactsRetained;
	if (!bCurrent && !bRetainedWait && Snapshot.Status != EGGYGORemoteMotionPoseStatus::Held)
	{
		// Initial/无保留事实的 Awaiting 无时间；NotApplicable/Invalid 结束已进入段的姿态来源。均不给 0 或默认时刻。
		Continuity.Reset();
		return;
	}
	if (Continuity.bSet && (Continuity.Provider.Get() != Provider || Continuity.PublisherEpoch != Snapshot.PublisherEpoch
		|| Continuity.SegmentSerial != Snapshot.SegmentSerial))
	{
		// 已进入段属于另一 Provider/Epoch/段，不能被本快照继承。
		Continuity.Reset();
	}
	if (bRetainedWait)
	{
		// Provider 资格合法等待：只有已真实消费进入同段的节点可保持其原生输出姿态。
		// 快照不提供 Sequence/时间/TimeDomain，这里不建立入场、新时间或终点姿态；bEnded 仅作真实关闭事实。
		if (!Continuity.bSet)
		{
			Continuity.Reset();
			return;
		}
		OutFrame.bRetainedWait = true;
		OutFrame.bEnded = Snapshot.bEnded;
		OutFrame.HeldConsumerNode = Continuity.ConsumerNode;
		OutFrame.MotionType = Snapshot.MotionType;
		OutFrame.TurnBackPhase = Snapshot.TurnBackPhase;
		OutFrame.bTurnBackRunOut = Snapshot.bTurnBackRunOut;
		return;
	}
	const auto Reject = [&OutFrame, &Continuity](const TCHAR* Reason)
	{
		Continuity.Reset();
		OutFrame.bConsumerRejected = true;
		OutFrame.Diagnostic = FString::Printf(TEXT("[Animation][RemotePose] %s; Provider: %s"), Reason, *OutFrame.Diagnostic);
	};
	if (!bCurrent && !Continuity.bSet)
	{
		Reject(TEXT("Held segment has no real node consumption in this instance; it cannot start, switch or resume a segment"));
		return;
	}
	// Movement 已核对源身份与时间；这里只防止跨模块边界上的空引用或越界时间被交给播放器。
	UAnimSequence* Sequence = Snapshot.Sequence.Get();
	const double Seconds = bCurrent ? Snapshot.SequenceSeconds : Snapshot.CertifiedSequenceSeconds;
	if (!IsValid(Sequence) || Snapshot.MotionType == EGGYGOLocomotionMotionType::None
		|| Snapshot.Identity == EGGYGORemoteMotionPoseIdentity::None
		|| Snapshot.TimeDomain == EGGYGORemoteMotionPoseTimeDomain::None
		|| !FMath::IsFinite(Seconds) || Seconds < 0.0 || Seconds > Sequence->GetPlayLength())
	{
		Reject(TEXT("certified sample has no live sequence, motion type, content identity, time domain or in-range time"));
		return;
	}
	OutFrame.bHasPose = true;
	OutFrame.bEnded = Snapshot.bEnded;
	OutFrame.bAdmissible = bCurrent && !Snapshot.bEnded;
	OutFrame.HeldConsumerNode = bCurrent ? 0 : Continuity.ConsumerNode;
	OutFrame.MotionType = Snapshot.MotionType;
	OutFrame.TurnBackPhase = Snapshot.TurnBackPhase;
	OutFrame.bTurnBackRunOut = Snapshot.bTurnBackRunOut;
	OutFrame.Sequence = Sequence;
	OutFrame.SequenceSeconds = Seconds;
}
