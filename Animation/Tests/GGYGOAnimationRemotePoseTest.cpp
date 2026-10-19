/**
 * @file GGYGOAnimationRemotePoseTest.cpp
 * @brief 远端权威复制姿态在 Animation 消费端的入场、真实节点消费、保持、结束与清理契约
 *
 * 输入是手工构造的 Movement 快照值与消费记录，只验证 Animation 的转换与进入证据规则；
 * 不冒称 CMC 配对、源身份认证、原生图更新或网络呈现已经运行。
 */
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Animation/Runtime/GGYGOAnimationRemotePose.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace GGYGOAnimationRemotePoseTest
{
	constexpr uint64 NodeA = 0x1000;
	constexpr uint64 NodeB = 0x2000;

	FGGYGORemoteMotionPoseSnapshot Sample(UAnimSequence* Sequence, EGGYGORemoteMotionPoseStatus Status,
		uint32 Epoch, uint32 Segment, double Seconds, double Certified, bool bEnded)
	{
		FGGYGORemoteMotionPoseSnapshot Snapshot;
		Snapshot.Status = Status;
		Snapshot.Identity = EGGYGORemoteMotionPoseIdentity::CompressedCurveContent;
		Snapshot.TimeDomain = EGGYGORemoteMotionPoseTimeDomain::NativeLinearDisplay;
		Snapshot.MotionType = EGGYGOLocomotionMotionType::TurnBack;
		Snapshot.TurnBackPhase = EGGYGOTurnBackPhase::Turning;
		Snapshot.bTurnBackCurveDriven = true;
		Snapshot.Sequence = Sequence;
		Snapshot.SequenceSeconds = Seconds;
		Snapshot.CertifiedSequenceSeconds = Certified;
		Snapshot.bEnded = bEnded;
		Snapshot.Closure = bEnded ? EGGYGORemoteMotionPoseClosure::Completed : EGGYGORemoteMotionPoseClosure::None;
		Snapshot.PublisherEpoch = Epoch;
		Snapshot.SegmentSerial = Segment;
		return Snapshot;
	}

	/** 模拟一次 GT 捕获与之后的一轮图更新：bUpdated=false 表示原生图未更新，Consumer=0 表示无节点消费。 */
	struct FHarness
	{
		FGGYGOAnimationRemotePoseContinuity Continuity;
		FGGYGOAnimationRemotePoseWitness Witness;
		FGGYGOAnimationRemotePoseFrame Frame;
		const UObject* PreviousProvider = nullptr;
		uint64 Token = 0;

		void Capture(const FGGYGORemoteMotionPoseSnapshot& Snapshot, const UObject* Provider, bool bGraphUpdated)
		{
			GGYGOAnimationRemotePose::InheritConsumption(bGraphUpdated, Frame.PublicationToken, PreviousProvider, Witness, Continuity);
			Witness = {};
			PreviousProvider = Provider;
			GGYGOAnimationRemotePose::Resolve(Snapshot, Provider, ++Token, Continuity, Frame);
		}

		/** 工作线程节点应用：只有消费入口能写记录；查询不能。 */
		bool Consume(uint64 Node, float& Seconds, bool& bEnded)
		{
			UAnimSequence* Out = nullptr;
			if (!Frame.TryGetPoseForConsumer(EGGYGOLocomotionMotionType::TurnBack, Node, Out, Seconds, bEnded))
			{
				return false;
			}
			GGYGOAnimationRemotePose::RecordConsumption(Frame, Node, Witness);
			return true;
		}

		bool Query(float& Seconds, bool& bEnded) const
		{
			UAnimSequence* Out = nullptr;
			return Frame.TryGetPose(EGGYGOLocomotionMotionType::TurnBack, Out, Seconds, bEnded) && Out;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGGYGOAnimationRemotePoseConsumerTest,
	"GGYGO.Animation.RemotePose.ConsumerContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAnimationRemotePoseConsumerTest::RunTest(const FString& Parameters)
{
	using namespace GGYGOAnimationRemotePoseTest;
	using EStatus = EGGYGORemoteMotionPoseStatus;
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	if (!TestNotNull(TEXT("Fixture requires the engine skeletal cube"), Mesh) || !TestNotNull(TEXT("Fixture skeleton"), Mesh->GetSkeleton()))
	{
		return false;
	}
	// 私有骨架副本，测试不修改引擎资产。
	TStrongObjectPtr<USkeleton> Skeleton(DuplicateObject<USkeleton>(Mesh->GetSkeleton(), GetTransientPackage()));
	if (!TestNotNull(TEXT("Fixture owns its skeleton copy"), Skeleton.Get()))
	{
		return false;
	}
	Skeleton->SetFlags(RF_Transient);
	TStrongObjectPtr<UAnimSequence> Sequence(NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient));
	Sequence->SetSkeleton(Skeleton.Get());
	IAnimationDataController& Controller = Sequence->GetController();
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(30), false);
	Controller.NotifyPopulated();
	// 非 1 的 RateScale 证明消费端不会再次缩放 Movement 已换算的原片时间。
	Sequence->RateScale = 2.0f;
	if (!TestEqual(TEXT("Fixture play length"), static_cast<double>(Sequence->GetPlayLength()), 1.0, 1.e-6))
	{
		return false;
	}
	// Provider 只作身份比较；UObject 为抽象类，须用可构造的具体类型（原生运行实测会触发 ensure）。
	TStrongObjectPtr<UObject> Provider(NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient));
	TStrongObjectPtr<UObject> OtherProvider(NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient));
	if (!TestTrue(TEXT("Fixture providers are distinct live objects"), Provider.IsValid() && OtherProvider.IsValid()
		&& Provider.Get() != OtherProvider.Get()))
	{
		return false;
	}
	UAnimSequence* Seq = Sequence.Get();
	float Seconds = -1.0f;
	bool bEnded = true;

	{
		// Current 原值、可入场；GT 见过 Current 但图更新中无节点消费，则之后的 Held 被拒。
		FHarness H;
		H.Capture(Sample(Seq, EStatus::Current, 3, 7, 0.4, 0.5, false), Provider.Get(), false);
		TestTrue(TEXT("Current samples its own time"), H.Query(Seconds, bEnded) && Seconds == 0.4f);
		TestTrue(TEXT("Current admits entry"), H.Frame.IsAdmissible(EGGYGOLocomotionMotionType::TurnBack));
		TestFalse(TEXT("Other motion type is not admitted"), H.Frame.IsAdmissible(EGGYGOLocomotionMotionType::WalkStop));
		TestTrue(TEXT("Query alone records no consumption"), !H.Witness.bSet);
		H.Capture(Sample(Seq, EStatus::Held, 3, 7, 0.5, 0.5, false), Provider.Get(), true);
		TestTrue(TEXT("Held without real node consumption is rejected"), !H.Query(Seconds, bEnded) && H.Frame.bConsumerRejected);
		TestEqual(TEXT("Rejected Held carries no phase"), H.Frame.TurnBackPhase, EGGYGOTurnBackPhase::None);
	}
	{
		// 真实节点消费 Current 后，同段 Held 只对同一节点开放，使用认证终点且不入场。
		FHarness H;
		H.Capture(Sample(Seq, EStatus::Current, 3, 7, 0.4, 0.5, false), Provider.Get(), false);
		TestTrue(TEXT("Node A consumes Current"), H.Consume(NodeA, Seconds, bEnded) && Seconds == 0.4f);
		H.Capture(Sample(Seq, EStatus::Held, 3, 7, 0.5, 0.5, false), Provider.Get(), true);
		TestTrue(TEXT("Held of the consumed segment samples the certified endpoint"), H.Query(Seconds, bEnded) && Seconds == 0.5f);
		TestFalse(TEXT("Held never admits entry"), H.Frame.IsAdmissible(EGGYGOLocomotionMotionType::TurnBack));
		TestFalse(TEXT("Another node cannot apply the Held pose"), H.Consume(NodeB, Seconds, bEnded));
		TestTrue(TEXT("The entered node keeps the Held pose"), H.Consume(NodeA, Seconds, bEnded) && Seconds == 0.5f);
		// 原生图未更新（跳过更新）不是缺席证据。
		H.Witness = {};
		H.Capture(Sample(Seq, EStatus::Held, 3, 7, 0.5, 0.5, false), Provider.Get(), false);
		TestTrue(TEXT("No graph update keeps the entered segment"), H.Query(Seconds, bEnded));
		// 图已更新但节点未消费：状态已离开或节点未更新，不再保持。
		H.Capture(Sample(Seq, EStatus::Held, 3, 7, 0.5, 0.5, false), Provider.Get(), true);
		TestFalse(TEXT("Updated graph without consumption retires the entered segment"), H.Query(Seconds, bEnded));
	}
	{
		// 换段、换 Epoch、换 Provider 都不继承进入证据。
		FHarness H;
		H.Capture(Sample(Seq, EStatus::Current, 3, 7, 0.4, 0.4, false), Provider.Get(), false);
		H.Consume(NodeA, Seconds, bEnded);
		H.Capture(Sample(Seq, EStatus::Held, 3, 8, 0.1, 0.1, false), Provider.Get(), true);
		TestFalse(TEXT("Held of another segment is rejected"), H.Query(Seconds, bEnded));
		FHarness E;
		E.Capture(Sample(Seq, EStatus::Current, 3, 7, 0.4, 0.4, false), Provider.Get(), false);
		E.Consume(NodeA, Seconds, bEnded);
		E.Capture(Sample(Seq, EStatus::Held, 4, 7, 0.4, 0.4, false), Provider.Get(), true);
		TestFalse(TEXT("Held of another publisher epoch is rejected"), E.Query(Seconds, bEnded));
		FHarness P;
		P.Capture(Sample(Seq, EStatus::Current, 3, 7, 0.4, 0.4, false), Provider.Get(), false);
		P.Consume(NodeA, Seconds, bEnded);
		P.Capture(Sample(Seq, EStatus::Held, 3, 7, 0.4, 0.4, false), OtherProvider.Get(), true);
		TestFalse(TEXT("Held from another provider is rejected"), P.Query(Seconds, bEnded));
	}
	{
		// 过期或冲突的消费记录不能建立进入。
		FHarness H;
		H.Capture(Sample(Seq, EStatus::Current, 3, 7, 0.4, 0.4, false), Provider.Get(), false);
		H.Consume(NodeA, Seconds, bEnded);
		H.Witness.PublicationToken += 100;
		H.Capture(Sample(Seq, EStatus::Held, 3, 7, 0.4, 0.4, false), Provider.Get(), true);
		TestFalse(TEXT("A record for another publication is not consumption of this frame"), H.Query(Seconds, bEnded));
		FHarness C;
		C.Capture(Sample(Seq, EStatus::Current, 3, 7, 0.4, 0.4, false), Provider.Get(), false);
		C.Consume(NodeA, Seconds, bEnded);
		C.Consume(NodeB, Seconds, bEnded);
		TestTrue(TEXT("Two consumers are a conflict"), C.Witness.bConflict);
		C.Capture(Sample(Seq, EStatus::Held, 3, 7, 0.4, 0.4, false), Provider.Get(), true);
		TestFalse(TEXT("Conflicting consumption cannot select a Held owner"), C.Query(Seconds, bEnded));
	}
	{
		// Awaiting 无时间、无相位，不以 0 采样，并清除进入证据。
		FHarness H;
		H.Capture(Sample(Seq, EStatus::Current, 3, 10, 0.3, 0.3, false), Provider.Get(), false);
		H.Consume(NodeA, Seconds, bEnded);
		FGGYGORemoteMotionPoseSnapshot Awaiting;
		Awaiting.Status = EStatus::Awaiting;
		Awaiting.WaitReason = EGGYGORemoteMotionPoseWaitReason::ProviderContentPending;
		H.Capture(Awaiting, Provider.Get(), true);
		Seconds = -1.0f;
		TestTrue(TEXT("Awaiting is remote without pose"), H.Frame.bRemote && !H.Query(Seconds, bEnded) && Seconds == 0.0f);
		TestFalse(TEXT("Awaiting is not admissible"), H.Frame.IsAdmissible(EGGYGOLocomotionMotionType::TurnBack));
		TestFalse(TEXT("Awaiting cannot be consumed"), H.Consume(NodeA, Seconds, bEnded) || H.Witness.bSet);
		H.Capture(Sample(Seq, EStatus::Held, 3, 10, 0.3, 0.3, false), Provider.Get(), true);
		TestFalse(TEXT("Held after Awaiting is not silently continued"), H.Query(Seconds, bEnded));
	}
	{
		// 自然终点可采样不授入场；取消、失败、越界与 NotRemote 均清除。
		FHarness H;
		H.Capture(Sample(Seq, EStatus::Current, 3, 11, 1.0, 1.0, true), Provider.Get(), false);
		TestTrue(TEXT("Ended terminal pose samples"), H.Query(Seconds, bEnded) && bEnded && Seconds == 1.0f);
		TestFalse(TEXT("Ended terminal pose is not admissible"), H.Frame.IsAdmissible(EGGYGOLocomotionMotionType::TurnBack));
		H.Consume(NodeA, Seconds, bEnded);
		FGGYGORemoteMotionPoseSnapshot Invalid;
		Invalid.Status = EStatus::Invalid;
		Invalid.Diagnostic = TEXT("provider failure");
		H.Capture(Invalid, Provider.Get(), true);
		TestTrue(TEXT("Invalid keeps provider diagnostic without pose"), !H.Query(Seconds, bEnded) && H.Frame.Diagnostic.Contains(TEXT("provider failure")));
		TestFalse(TEXT("Invalid retires the entered segment"), H.Continuity.bSet);
		FGGYGORemoteMotionPoseSnapshot Cancelled;
		Cancelled.Status = EStatus::NotApplicable;
		Cancelled.Closure = EGGYGORemoteMotionPoseClosure::Cancelled;
		H.Capture(Cancelled, Provider.Get(), true);
		TestTrue(TEXT("Cancelled has no pose and is not an end"), !H.Query(Seconds, bEnded) && !H.Frame.bEnded);
		H.Capture(Sample(Seq, EStatus::Current, 3, 12, 1.5, 1.5, false), Provider.Get(), true);
		TestTrue(TEXT("Out-of-range time is rejected"), !H.Query(Seconds, bEnded) && H.Frame.bConsumerRejected);
		H.Capture(Sample(Seq, EStatus::Current, 3, 13, 0.2, 0.2, false), Provider.Get(), true);
		H.Consume(NodeA, Seconds, bEnded);
		FGGYGORemoteMotionPoseSnapshot Local;
		Local.Status = EStatus::NotRemote;
		H.Capture(Local, Provider.Get(), true);
		TestTrue(TEXT("NotRemote yields the local path without remote evidence"), !H.Frame.bRemote && !H.Query(Seconds, bEnded) && !H.Continuity.bSet);
		H.Continuity.Reset();
		TestFalse(TEXT("Reset clears the entered segment"), H.Continuity.bSet);
	}
	const auto Retained = [](uint32 Epoch, uint32 Segment, bool bEnded, EGGYGOTurnBackPhase Phase)
	{
		// Provider 资格等待：只有段事实，没有 Sequence/时间/TimeDomain/Identity。
		FGGYGORemoteMotionPoseSnapshot Snapshot;
		Snapshot.Status = EGGYGORemoteMotionPoseStatus::Awaiting;
		Snapshot.WaitReason = EGGYGORemoteMotionPoseWaitReason::ProviderContentPending;
		Snapshot.bSegmentFactsRetained = true;
		Snapshot.MotionType = EGGYGOLocomotionMotionType::TurnBack;
		Snapshot.TurnBackPhase = Phase;
		Snapshot.bTurnBackRunOut = Phase == EGGYGOTurnBackPhase::RunOut;
		Snapshot.bEnded = bEnded;
		Snapshot.Closure = bEnded ? EGGYGORemoteMotionPoseClosure::Completed : EGGYGORemoteMotionPoseClosure::None;
		Snapshot.PublisherEpoch = Epoch;
		Snapshot.SegmentSerial = Segment;
		return Snapshot;
	};
	{
		// 节点 A 消费 Current → 连续 Held → 保留等待（只保持、不采样）→ 同段 Current 恢复。
		FHarness H;
		H.Capture(Sample(Seq, EStatus::Current, 5, 20, 0.2, 0.2, false), Provider.Get(), false);
		TestTrue(TEXT("A consumes Current"), H.Consume(NodeA, Seconds, bEnded));
		for (int32 Index = 0; Index < 2; ++Index)
		{
			H.Capture(Sample(Seq, EStatus::Held, 5, 20, 0.3, 0.3, false), Provider.Get(), true);
			TestTrue(TEXT("Consecutive Held keeps the same node's witness"), H.Consume(NodeA, Seconds, bEnded) && Seconds == 0.3f);
		}
		H.Capture(Retained(5, 20, false, EGGYGOTurnBackPhase::Turning), Provider.Get(), true);
		TestTrue(TEXT("Retained wait keeps phase for the entered node"), H.Frame.bRetainedWait
			&& H.Frame.TurnBackPhase == EGGYGOTurnBackPhase::Turning);
		TestFalse(TEXT("Retained wait has no samplable pose"), H.Query(Seconds, bEnded) || H.Frame.IsAdmissible(EGGYGOLocomotionMotionType::TurnBack));
		TestFalse(TEXT("Retained wait gives no pose even to the entered node"), H.Consume(NodeA, Seconds, bEnded));
		TestTrue(TEXT("Only the entered node is a waiting participant"),
			H.Frame.IsRetainedWaitConsumer(EGGYGOLocomotionMotionType::TurnBack, NodeA)
			&& !H.Frame.IsRetainedWaitConsumer(EGGYGOLocomotionMotionType::TurnBack, NodeB));
		GGYGOAnimationRemotePose::RecordConsumption(H.Frame, NodeA, H.Witness);
		H.Capture(Retained(5, 20, false, EGGYGOTurnBackPhase::Turning), Provider.Get(), true);
		TestTrue(TEXT("Participation during wait keeps the entered segment"), H.Frame.bRetainedWait);
		GGYGOAnimationRemotePose::RecordConsumption(H.Frame, NodeA, H.Witness);
		H.Capture(Sample(Seq, EStatus::Held, 5, 20, 0.4, 0.4, false), Provider.Get(), true);
		TestTrue(TEXT("Held after a witnessed wait resumes for A"), H.Consume(NodeA, Seconds, bEnded) && Seconds == 0.4f);
		H.Capture(Sample(Seq, EStatus::Current, 5, 20, 0.5, 0.5, false), Provider.Get(), true);
		TestTrue(TEXT("Same segment Current resumes"), H.Consume(NodeA, Seconds, bEnded) && Seconds == 0.5f);
	}
	{
		// 未真实消费、他节点参与、图更新无参与、retained=false、新段/新 Epoch/换 Provider 均不保持。
		FHarness H;
		H.Capture(Sample(Seq, EStatus::Current, 5, 21, 0.2, 0.2, false), Provider.Get(), false);
		H.Capture(Retained(5, 21, false, EGGYGOTurnBackPhase::Turning), Provider.Get(), true);
		TestTrue(TEXT("Retained wait without real consumption has no facts"), !H.Frame.bRetainedWait
			&& H.Frame.TurnBackPhase == EGGYGOTurnBackPhase::None);
		FHarness B;
		B.Capture(Sample(Seq, EStatus::Current, 5, 22, 0.2, 0.2, false), Provider.Get(), false);
		B.Consume(NodeA, Seconds, bEnded);
		B.Capture(Retained(5, 22, false, EGGYGOTurnBackPhase::Turning), Provider.Get(), true);
		GGYGOAnimationRemotePose::RecordConsumption(B.Frame, NodeB, B.Witness);
		TestFalse(TEXT("Another node's wait participation is not recorded"), B.Witness.bSet);
		B.Capture(Retained(5, 22, false, EGGYGOTurnBackPhase::Turning), Provider.Get(), true);
		TestFalse(TEXT("Updated graph without the entered node retires the wait"), B.Frame.bRetainedWait);
		FHarness N;
		N.Capture(Sample(Seq, EStatus::Current, 5, 23, 0.2, 0.2, false), Provider.Get(), false);
		N.Consume(NodeA, Seconds, bEnded);
		FGGYGORemoteMotionPoseSnapshot NotRetained = Retained(5, 23, false, EGGYGOTurnBackPhase::Turning);
		NotRetained.bSegmentFactsRetained = false;
		N.Capture(NotRetained, Provider.Get(), true);
		TestTrue(TEXT("Awaiting without retained facts closes the segment"), !N.Frame.bRetainedWait && !N.Continuity.bSet);
		const auto Mismatch = [&](uint32 Epoch, uint32 Segment, const UObject* WaitProvider)
		{
			FHarness M;
			M.Capture(Sample(Seq, EStatus::Current, 5, 24, 0.2, 0.2, false), Provider.Get(), false);
			M.Consume(NodeA, Seconds, bEnded);
			M.Capture(Retained(Epoch, Segment, false, EGGYGOTurnBackPhase::Turning), WaitProvider, true);
			return !M.Frame.bRetainedWait && !M.Continuity.bSet;
		};
		TestTrue(TEXT("Retained wait of another segment closes"), Mismatch(5, 25, Provider.Get()));
		TestTrue(TEXT("Retained wait of another epoch closes"), Mismatch(6, 24, Provider.Get()));
		TestTrue(TEXT("Retained wait from another provider closes"), Mismatch(5, 24, OtherProvider.Get()));
	}
	{
		// 保留等待中的真实自然末端只用于关闭判定，不造终点姿态；随后的取消/失败/无段即撤销。
		FHarness H;
		H.Capture(Sample(Seq, EStatus::Current, 5, 26, 0.9, 0.9, false), Provider.Get(), false);
		H.Consume(NodeA, Seconds, bEnded);
		H.Capture(Retained(5, 26, true, EGGYGOTurnBackPhase::RunOut), Provider.Get(), true);
		TestTrue(TEXT("Retained natural end is a closure fact"), H.Frame.HasEndedFact(EGGYGOLocomotionMotionType::TurnBack));
		TestFalse(TEXT("Retained natural end creates no terminal pose"), H.Query(Seconds, bEnded) || H.Consume(NodeA, Seconds, bEnded));
		for (const EGGYGORemoteMotionPoseStatus Closing : {EStatus::NotApplicable, EStatus::Invalid, EStatus::Initial})
		{
			FHarness C;
			C.Capture(Sample(Seq, EStatus::Current, 5, 27, 0.2, 0.2, false), Provider.Get(), false);
			C.Consume(NodeA, Seconds, bEnded);
			FGGYGORemoteMotionPoseSnapshot Close;
			Close.Status = Closing;
			C.Capture(Close, Provider.Get(), true);
			TestTrue(TEXT("Cancel/no segment/failure/initial closes immediately"),
				!C.Frame.bRetainedWait && !C.Frame.bHasPose && !C.Continuity.bSet && !C.Frame.HasEndedFact(EGGYGOLocomotionMotionType::TurnBack));
		}
	}
	return true;
}

#endif
