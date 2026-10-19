/**
 * @file GGYGOAnimationRemotePoseNodeUpdateTest.cpp
 * @brief 真实动画更新中 SequenceEvaluator UpdateFunction 对远端姿态的写入、进入证据续接、等待保持与关闭。
 *
 * 需要 Saved 验证宿主先在 /Temp 建好夹具 AnimBlueprint（父类为夹具 AnimInstance，图为 SequenceEvaluator
 * 加 UpdateFunction 调用 ApplyMotionPoseToEvaluator）；缺失时明确失败，不跳过。快照为显式夹具输入，
 * 只证明 Animation 消费合同在原生节点更新中的行为，不证明 CMC 已执行区间、网络运输或自然 MeshFinalize。
 *
 * 每帧走 TickAnimation + RefreshBoneTransforms(nullptr)：无 TickFunction 时引擎在 GT 上执行同一
 * ParallelUpdateAnimation/求值/PostAnimEvaluation，不是工作线程派发。
 */
#include "Animation/Tests/GGYGOAnimationRemotePoseTestTypes.h"

#include "Character/Components/GGYGOCharacterMovementComponent.h"

AGGYGOAnimationRemotePoseFixtureCharacter::AGGYGOAnimationRemotePoseFixtureCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UGGYGOCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimNode_AssetPlayerBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "PreviewScene.h"
#include "UObject/UnrealType.h"

namespace GGYGOAnimationRemotePoseNodeUpdate
{
	/** 由 Saved/Codex/AnimationRemoteIntegration 验证宿主建立；本测试只读取，不创建或保存。 */
	constexpr const TCHAR* FixtureClassPath = TEXT("/Temp/CodexAnimRemoteFixture/ABP_RemoteFixture.ABP_RemoteFixture_C");

	/** 夹具图只允许一个资产播放器；节点地址只在本实例生命周期内读取。 */
	const FAnimNode_AssetPlayerBase* FindSinglePlayer(const UAnimInstance& Anim, int32& OutCount)
	{
		const FAnimNode_AssetPlayerBase* Found = nullptr;
		OutCount = 0;
		for (TFieldIterator<FStructProperty> It(Anim.GetClass()); It; ++It)
		{
			if (It->Struct->IsChildOf(FAnimNode_AssetPlayerBase::StaticStruct()))
			{
				Found = It->ContainerPtrToValuePtr<FAnimNode_AssetPlayerBase>(&Anim);
				++OutCount;
			}
		}
		return Found;
	}

	FGGYGORemoteMotionPoseSnapshot Sample(UAnimSequence* Sequence, EGGYGORemoteMotionPoseStatus Status, double Seconds, bool bEnded)
	{
		FGGYGORemoteMotionPoseSnapshot Snapshot;
		Snapshot.Status = Status;
		Snapshot.Identity = EGGYGORemoteMotionPoseIdentity::CompressedCurveContent;
		Snapshot.TimeDomain = EGGYGORemoteMotionPoseTimeDomain::AuthorityCompletedEndpoint;
		Snapshot.MotionType = EGGYGOLocomotionMotionType::TurnBack;
		Snapshot.TurnBackPhase = EGGYGOTurnBackPhase::Turning;
		Snapshot.bTurnBackCurveDriven = true;
		Snapshot.Sequence = Sequence;
		Snapshot.SequenceSeconds = Seconds;
		Snapshot.CertifiedSequenceSeconds = Seconds;
		Snapshot.bEnded = bEnded;
		Snapshot.Closure = bEnded ? EGGYGORemoteMotionPoseClosure::Completed : EGGYGORemoteMotionPoseClosure::None;
		Snapshot.PublisherEpoch = 9;
		Snapshot.SegmentSerial = 1;
		return Snapshot;
	}

	FGGYGORemoteMotionPoseSnapshot Retained()
	{
		FGGYGORemoteMotionPoseSnapshot Snapshot;
		Snapshot.Status = EGGYGORemoteMotionPoseStatus::Awaiting;
		Snapshot.WaitReason = EGGYGORemoteMotionPoseWaitReason::ProviderContentPending;
		Snapshot.bSegmentFactsRetained = true;
		Snapshot.MotionType = EGGYGOLocomotionMotionType::TurnBack;
		Snapshot.TurnBackPhase = EGGYGOTurnBackPhase::Turning;
		Snapshot.bTurnBackCurveDriven = true;
		Snapshot.PublisherEpoch = 9;
		Snapshot.SegmentSerial = 1;
		return Snapshot;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGGYGOAnimationRemotePoseNodeUpdateTest,
	"GGYGO.Harness.RemotePose.NodeUpdate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAnimationRemotePoseNodeUpdateTest::RunTest(const FString& Parameters)
{
	using namespace GGYGOAnimationRemotePoseNodeUpdate;
	using EStatus = EGGYGORemoteMotionPoseStatus;
	UAnimBlueprintGeneratedClass* AnimClass = FindObject<UAnimBlueprintGeneratedClass>(nullptr, FixtureClassPath);
	if (!TestNotNull(TEXT("Harness /Temp fixture AnimBlueprint class is present"), AnimClass)
		|| !TestTrue(TEXT("Fixture class derives from the fixture AnimInstance"),
			AnimClass->IsChildOf(UGGYGOAnimationRemotePoseFixtureAnimInstance::StaticClass())))
	{
		return false;
	}
	USkeleton* Skeleton = AnimClass->GetTargetSkeleton();
	USkeletalMesh* MeshAsset = Skeleton ? Skeleton->GetPreviewMesh(true) : nullptr;
	if (!MeshAsset && Skeleton)
	{
		MeshAsset = Skeleton->FindCompatibleMesh();
	}
	if (!TestNotNull(TEXT("Fixture skeleton has a compatible mesh"), MeshAsset))
	{
		return false;
	}

	// 自有 EditorPreview 世界：不是 PIE/游戏世界，不属于任何地图包，不进事务；作用域结束即销毁。
	FPreviewScene PreviewScene(FPreviewScene::ConstructionValues().SetTransactional(false));
	UWorld* World = PreviewScene.GetWorld();
	if (!TestNotNull(TEXT("Preview world exists"), World))
	{
		return false;
	}
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	AGGYGOAnimationRemotePoseFixtureCharacter* Character = World->SpawnActor<AGGYGOAnimationRemotePoseFixtureCharacter>(SpawnParameters);
	if (!TestNotNull(TEXT("Fixture character spawned in the preview world"), Character)
		|| !TestNotNull(TEXT("Fixture character uses the project CMC"), Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement())))
	{
		return false;
	}
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	Mesh->SetSkeletalMeshAsset(MeshAsset);
	Mesh->SetAnimInstanceClass(AnimClass);
	UGGYGOAnimationRemotePoseFixtureAnimInstance* Anim = Cast<UGGYGOAnimationRemotePoseFixtureAnimInstance>(Mesh->GetAnimInstance());
	if (!TestNotNull(TEXT("Mesh created the fixture AnimInstance"), Anim))
	{
		return false;
	}
	int32 PlayerCount = 0;
	const FAnimNode_AssetPlayerBase* Player = FindSinglePlayer(*Anim, PlayerCount);
	UAnimSequence* Sequence = Player ? Cast<UAnimSequence>(Player->GetAnimAsset()) : nullptr;
	if (!TestEqual(TEXT("Fixture graph has exactly one asset player (the SequenceEvaluator)"), PlayerCount, 1)
		|| !TestNotNull(TEXT("Evaluator has an authored sequence"), Sequence))
	{
		return false;
	}
	const double Length = Sequence->GetPlayLength();
	const double Tolerance = 1.e-4;
	// 输入时间是节点实例的 ExplicitTime（setter 写入目标）；输出时间是 teleport 后的累计时间。
	const auto InputTime = [&]() { return static_cast<double>(Player->GetCurrentAssetTime()); };
	const auto NodeTime = [&]() { return static_cast<double>(Player->GetAccumulatedTime()); };
	const auto Tick = [&](const FGGYGORemoteMotionPoseSnapshot& Snapshot, const TCHAR* Step)
	{
		int16 UpdateBefore = 0, Evaluation = 0;
		bool bRootMotion = false;
		Anim->GetNativePoseEvaluationFacts(UpdateBefore, Evaluation, bRootMotion);
		Anim->SetFixtureSnapshot(Snapshot);
		Mesh->TickAnimation(1.0f / 30.0f, false);
		Mesh->RefreshBoneTransforms(nullptr);
		int16 UpdateAfter = 0;
		Anim->GetNativePoseEvaluationFacts(UpdateAfter, Evaluation, bRootMotion);
		TestTrue(FString::Printf(TEXT("Native graph update ran (%s)"), Step), UpdateAfter != UpdateBefore);
		AddInfo(FString::Printf(TEXT("[%s] Input=%.6f Node=%.6f Length=%.6f HasPose=%d Retained=%d Rejected=%d"),
			Step, InputTime(), NodeTime(), Length, Anim->GetRemoteMotionPoseFrame().bHasPose,
			Anim->GetRemoteMotionPoseFrame().bRetainedWait, Anim->GetRemoteMotionPoseFrame().bConsumerRejected));
	};

	// 0. 无事实的等待：节点被更新，但不写 null/0，保持作者时间与作者序列。
	const double AuthoredInput = InputTime();
	// 夹具必须能区分保持作者时间、错误写零与提前写入任一消费样本；无效夹具明确失败。
	if (!TestTrue(TEXT("夹具源长度和作者时间必须有限，作者时间需严格位于有效源区间内"),
		FMath::IsFinite(Length) && FMath::IsFinite(AuthoredInput)
		&& AuthoredInput > Tolerance && AuthoredInput < Length - Tolerance))
	{
		return false;
	}
	constexpr double SampleFractions[] = { 0.2, 0.25, 0.3, 0.4, 0.5, 1.0 };
	for (const double SampleFraction : SampleFractions)
	{
		if (!TestTrue(FString::Printf(TEXT("夹具作者时间必须区别于消费样本 %.2fL"), SampleFraction),
			!FMath::IsNearlyEqual(AuthoredInput, SampleFraction * Length, Tolerance)))
		{
			return false;
		}
	}
	FGGYGORemoteMotionPoseSnapshot Awaiting;
	Awaiting.Status = EStatus::Awaiting;
	Awaiting.WaitReason = EGGYGORemoteMotionPoseWaitReason::NativeSample;
	Tick(Awaiting, TEXT("Awaiting"));
	TestTrue(TEXT("No pose leaves the authored node input"), FMath::IsNearlyEqual(InputTime(), AuthoredInput, Tolerance)
		&& FMath::IsNearlyEqual(NodeTime(), AuthoredInput, Tolerance) && Player->GetAnimAsset() == Sequence);
	TestFalse(TEXT("Awaiting has no pose"), Anim->GetRemoteMotionPoseFrame().bHasPose);

	// 1. Current：节点写入远端绝对时间（原 Sequence 坐标，不乘 RateScale）。
	Tick(Sample(Sequence, EStatus::Current, 0.2 * Length, false), TEXT("Current"));
	TestTrue(TEXT("Current is written into the updated node"), FMath::IsNearlyEqual(InputTime(), 0.2 * Length, Tolerance)
		&& FMath::IsNearlyEqual(NodeTime(), 0.2 * Length, Tolerance));
	TestTrue(TEXT("Current admits entry"), Anim->IsMotionPoseActive(EGGYGOLocomotionMotionType::TurnBack));

	// 2-3. 连续 Held：只有真实消费过的同一节点继续写入认证终点。
	Tick(Sample(Sequence, EStatus::Held, 0.25 * Length, false), TEXT("Held1"));
	TestTrue(TEXT("Held after real node consumption has pose"), Anim->GetRemoteMotionPoseFrame().bHasPose);
	TestTrue(TEXT("Held certified time is written"), FMath::IsNearlyEqual(NodeTime(), 0.25 * Length, Tolerance));
	TestFalse(TEXT("Held does not admit entry"), Anim->IsMotionPoseActive(EGGYGOLocomotionMotionType::TurnBack));
	Tick(Sample(Sequence, EStatus::Held, 0.3 * Length, false), TEXT("Held2"));
	TestTrue(TEXT("Consecutive Held continues through the node witness"), Anim->GetRemoteMotionPoseFrame().bHasPose
		&& FMath::IsNearlyEqual(NodeTime(), 0.3 * Length, Tolerance));

	// 4-5. 合法保留等待：原节点参与但不调用 setter，保持原输出时间；连续两帧保持。
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Tick(Retained(), TEXT("RetainedWait"));
		TestTrue(TEXT("Retained wait keeps the entered node"), Anim->GetRemoteMotionPoseFrame().bRetainedWait);
		TestTrue(TEXT("Retained wait writes nothing"), FMath::IsNearlyEqual(InputTime(), 0.3 * Length, Tolerance)
			&& FMath::IsNearlyEqual(NodeTime(), 0.3 * Length, Tolerance));
		TestTrue(TEXT("Retained wait keeps the certified phase"), Anim->IsTurnBackCurveDriven());
	}

	// 6. 同段 Current 恢复。
	Tick(Sample(Sequence, EStatus::Current, 0.4 * Length, false), TEXT("CurrentResume"));
	TestTrue(TEXT("Same segment Current resumes"), FMath::IsNearlyEqual(NodeTime(), 0.4 * Length, Tolerance));

	// 7. 自然结束：终点可写入，关闭判定为真，不授予入场。
	Tick(Sample(Sequence, EStatus::Current, Length, true), TEXT("NaturalEnd"));
	TestTrue(TEXT("Natural end pose is written"), FMath::IsNearlyEqual(NodeTime(), Length, Tolerance));
	TestTrue(TEXT("Natural end is a closure fact"), Anim->IsMotionPoseEnded(EGGYGOLocomotionMotionType::TurnBack));
	TestFalse(TEXT("Natural end is not admissible"), Anim->IsMotionPoseActive(EGGYGOLocomotionMotionType::TurnBack));

	// 8-9. 取消：无姿态、不写；之后同段 Held 被拒绝。
	FGGYGORemoteMotionPoseSnapshot Cancelled;
	Cancelled.Status = EStatus::NotApplicable;
	Cancelled.Closure = EGGYGORemoteMotionPoseClosure::Cancelled;
	Tick(Cancelled, TEXT("Cancelled"));
	TestTrue(TEXT("Cancel writes nothing"), !Anim->GetRemoteMotionPoseFrame().bHasPose
		&& FMath::IsNearlyEqual(InputTime(), Length, Tolerance) && FMath::IsNearlyEqual(NodeTime(), Length, Tolerance));
	Tick(Sample(Sequence, EStatus::Held, 0.5 * Length, false), TEXT("HeldAfterCancel"));
	TestTrue(TEXT("Held after cancel is rejected and writes nothing"), Anim->GetRemoteMotionPoseFrame().bConsumerRejected
		&& FMath::IsNearlyEqual(InputTime(), Length, Tolerance) && FMath::IsNearlyEqual(NodeTime(), Length, Tolerance));

	// 10. 生命周期重建：同一实例重新初始化后不继承旧段；节点输入在该帧前后不变。
	Mesh->InitializeAnimScriptInstance(true);
	Anim = Cast<UGGYGOAnimationRemotePoseFixtureAnimInstance>(Mesh->GetAnimInstance());
	Player = Anim ? FindSinglePlayer(*Anim, PlayerCount) : nullptr;
	if (TestNotNull(TEXT("Reinitialized fixture instance"), Anim) && TestNotNull(TEXT("Reinitialized player"), Player))
	{
		const double InputBefore = InputTime();
		Tick(Sample(Sequence, EStatus::Held, 0.5 * Length, false), TEXT("HeldAfterReinit"));
		TestTrue(TEXT("Held after lifecycle reinit is rejected"), Anim->GetRemoteMotionPoseFrame().bConsumerRejected);
		TestTrue(TEXT("Rejected Held after reinit writes nothing"), FMath::IsNearlyEqual(InputTime(), InputBefore, Tolerance)
			&& !FMath::IsNearlyEqual(InputTime(), 0.5 * Length, Tolerance));
	}
	return true;
}

#endif
