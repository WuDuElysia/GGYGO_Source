/**
 * @file GGYGOBodyLeanNodeLifecycleTest.cpp
 * @brief BodyLean 原生骨缓存、状态重入和骨集合更换的回归；不依赖角色资产或测试专用生产入口
 */
#include "Animation/Nodes/GGYGOAnimNode_BodyLean.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "Misc/MemStack.h"
#include "ReferenceSkeleton.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "PreviewScene.h"
#include "Rendering/SkeletalMeshRenderData.h"

namespace GGYGOBodyLeanNodeLifecycle
{
	/** 仅开放原生 protected static 取用器；不构造派生 UObject，也不替换实例自有 Proxy。 */
	struct FAnimInstanceAccess : UAnimInstance
	{
		static FAnimInstanceProxy* GetProxy(UAnimInstance* Instance)
		{
			return GetProxyOnGameThreadStatic<FAnimInstanceProxy>(Instance);
		}
	};

	/** 仅开放原生有效性查询；初始化、缓存、更新和求值均调用真实节点入口。 */
	struct FNode : FGGYGOAnimNode_BodyLean
	{
		using FGGYGOAnimNode_BodyLean::IsValidToEvaluate;
	};

	struct FFixture
	{
		FPreviewScene PreviewScene { FPreviewScene::ConstructionValues().SetTransactional(false) };
		TStrongObjectPtr<USkeleton> Skeleton { NewObject<USkeleton>(GetTransientPackage()) };
		TStrongObjectPtr<USkeletalMesh> MeshAsset { NewObject<USkeletalMesh>(GetTransientPackage()) };
		TStrongObjectPtr<USkeletalMeshComponent> Mesh { NewObject<USkeletalMeshComponent>(GetTransientPackage()) };
		FAnimInstanceProxy* Proxy = nullptr;
		FNode Node;
		TArray<FBoneIndexType> FullBones { 0, 1, 2, 3, 4, 5, 6, 7 };

		FFixture()
		{
			{
				FReferenceSkeletonModifier Modifier(MeshAsset->GetRefSkeleton(), Skeleton.Get());
				const auto Add = [&Modifier](const TCHAR* Name, int32 Parent, const FVector& Translation)
				{
					Modifier.Add(FMeshBoneInfo(FName(Name), FString(Name), Parent), FTransform(Translation));
				};
				Add(TEXT("Root"), INDEX_NONE, FVector::ZeroVector);
				Add(TEXT("Body"), 0, FVector(0.0, 0.0, 100.0));
				Add(TEXT("UpperL"), 1, FVector(0.0, -10.0, -10.0));
				Add(TEXT("LowerL"), 2, FVector(8.0, 0.0, -45.0));
				Add(TEXT("FootL"), 3, FVector(-8.0, 0.0, -45.0));
				Add(TEXT("UpperR"), 1, FVector(0.0, 10.0, -10.0));
				Add(TEXT("LowerR"), 5, FVector(8.0, 0.0, -45.0));
				Add(TEXT("FootR"), 6, FVector(-8.0, 0.0, -45.0));
			}
			MeshAsset->SetSkeleton(Skeleton.Get());
			Skeleton->MergeAllBonesToBoneTree(MeshAsset.Get());
			MeshAsset->CalculateInvRefMatrices();
			// 真实宿主沿用项目渲染测试的合法无三角形 LOD；不加载或修改角色资产。
			MeshAsset->AllocateResourceForRendering();
			FSkeletalMeshLODRenderData* LOD = new FSkeletalMeshLODRenderData();
			LOD->ActiveBoneIndices = FullBones;
			LOD->RequiredBones = FullBones;
			MeshAsset->GetResourceForRendering()->LODRenderData.Add(LOD);
			MeshAsset->AddLODInfo();
			Mesh->SetVisibility(false);
			Mesh->bUseRefPoseOnInitAnim = true;
			Mesh->SetSkeletalMeshAsset(MeshAsset.Get());
			PreviewScene.AddComponent(Mesh.Get(), FTransform::Identity);
			Mesh->SetAnimInstanceClass(UAnimInstance::StaticClass());
			Proxy = FAnimInstanceAccess::GetProxy(Mesh->GetAnimInstance());
			Node.Body.BoneName = FName(TEXT("Body"));
			Node.LeftLeg.Foot.BoneName = FName(TEXT("FootL"));
			Node.RightLeg.Foot.BoneName = FName(TEXT("FootR"));
			Node.LeftLeg.KneeBendAxisLocal = FVector::ForwardVector;
			Node.RightLeg.KneeBendAxisLocal = FVector::ForwardVector;
			Node.LeanDegrees = 20.0f;
			Node.LeanAxisCS = -FVector::ForwardVector;
			Node.UpDirectionCS = FVector::UpVector;
			Node.Alpha = 1.0f;
		}

		~FFixture()
		{
			Mesh->DestroyComponent();
		}

		bool IsValid()
		{
			return Proxy && Node.IsValidToEvaluate(Skeleton.Get(), Proxy->GetRequiredBones());
		}

		void Initialize()
		{
			Node.Initialize_AnyThread(FAnimationInitializeContext(Proxy));
		}

		void CacheBones()
		{
			Node.CacheBones_AnyThread(FAnimationCacheBonesContext(Proxy));
		}

		TArray<FTransform> Evaluate()
		{
			Node.Update_AnyThread(FAnimationUpdateContext(Proxy, 1.0f / 60.0f));
			FComponentSpacePoseContext Output(Proxy);
			Node.EvaluateComponentSpace_AnyThread(Output);
			TArray<FTransform> Result;
			for (const FCompactPoseBoneIndex Index : Output.Pose.GetPose().ForEachBoneIndex())
			{
				Result.Add(Output.Pose.GetComponentSpaceTransform(Index));
			}
			return Result;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBodyLeanNodeReentryTest,
	"GGYGO.Animation.BodyLean.NodeReentry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOBodyLeanNodeReentryTest::RunTest(const FString& Parameters)
{
	using namespace GGYGOBodyLeanNodeLifecycle;
	FMemMark Memory(FMemStack::Get());
	FFixture Fixture;
	if (!TestNotNull(TEXT("真实Mesh初始化其AnimInstance自有Proxy"), Fixture.Proxy)) return false;
	if (!TestTrue(TEXT("原生Proxy包含宿主Skeleton"), Fixture.Proxy->IsSkeletonValid())
		|| !TestEqual(TEXT("Proxy绑定真实Mesh的原Skeleton"), Fixture.Proxy->GetSkeleton(), Fixture.Skeleton.Get())) return false;
	Fixture.Initialize();
	TestFalse(TEXT("尚未CacheBones不具备骨控制资格"), Fixture.IsValid());
	Fixture.CacheBones();
	if (!TestTrue(TEXT("完整双腿链经真实CacheBones获得资格"), Fixture.IsValid())) return false;
	const TArray<FTransform> Before = Fixture.Evaluate();
	if (!TestEqual(TEXT("真实输入姿态含八骨"), Before.Num(), 8)) return false;
	TestFalse(TEXT("非零倾身实际改变Body旋转"), Before[1].GetRotation().Equals(FQuat::Identity, 1.e-5));

	// 对应原生StateMachine重入：同一RequiredBones可只Initialize，不再调用CacheBones。
	Fixture.Initialize();
	if (!TestTrue(TEXT("同骨集合重入保留完整缓存资格"), Fixture.IsValid())) return false;
	const TArray<FTransform> After = Fixture.Evaluate();
	if (!TestEqual(TEXT("重入求值保留完整骨数"), After.Num(), Before.Num())) return false;
	for (int32 Index = 0; Index < Before.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("重入后骨[%d]保持原求解姿态"), Index), Before[Index].Equals(After[Index], 1.e-6));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBodyLeanNodeBoneCacheTest,
	"GGYGO.Animation.BodyLean.NodeBoneCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOBodyLeanNodeBoneCacheTest::RunTest(const FString& Parameters)
{
	using namespace GGYGOBodyLeanNodeLifecycle;
	FMemMark Memory(FMemStack::Get());
	FFixture Fixture;
	if (!TestNotNull(TEXT("真实Mesh初始化其AnimInstance自有Proxy"), Fixture.Proxy)) return false;
	if (!TestTrue(TEXT("原生Proxy包含宿主Skeleton"), Fixture.Proxy->IsSkeletonValid())
		|| !TestEqual(TEXT("Proxy绑定真实Mesh的原Skeleton"), Fixture.Proxy->GetSkeleton(), Fixture.Skeleton.Get())) return false;
	Fixture.Initialize();
	Fixture.CacheBones();
	if (!TestTrue(TEXT("原完整骨集合有效"), Fixture.IsValid())) return false;

	const TArray<FBoneIndexType> LeftOnly { 0, 1, 2, 3, 4 };
	Fixture.Proxy->GetRequiredBones().InitializeTo(LeftOnly, UE::Anim::FCurveFilterSettings(), *Fixture.MeshAsset.Get());
	TestFalse(TEXT("原缓存不能跨RequiredBones重建继续使用"), Fixture.IsValid());
	Fixture.Initialize();
	TestFalse(TEXT("Initialize不能代替新骨集合的CacheBones"), Fixture.IsValid());
	Fixture.CacheBones();
	TestFalse(TEXT("缺右腿的新骨集合明确拒绝"), Fixture.IsValid());
	AddExpectedErrorPlain(TEXT("Leg[1]脚骨[FootR]或明确膝弯单位轴无效"), EAutomationExpectedErrorFlags::Contains, 2);
	const TArray<FTransform> Incomplete = Fixture.Evaluate();
	if (TestEqual(TEXT("拒绝时仍由原生输入建立当前五骨姿态"), Incomplete.Num(), 5))
	{
		TestTrue(TEXT("无效完整链不提交半条腿的倾身"), Incomplete[1].GetRotation().Equals(FQuat::Identity, 1.e-6));
	}

	Fixture.Proxy->GetRequiredBones().InitializeTo(Fixture.FullBones, UE::Anim::FCurveFilterSettings(), *Fixture.MeshAsset.Get());
	Fixture.Node.RightLeg.KneeBendAxisLocal = FVector::ZeroVector;
	Fixture.CacheBones();
	TestFalse(TEXT("第二条腿配置失败不能发布第一条腿缓存"), Fixture.IsValid());
	Fixture.Initialize();
	TestFalse(TEXT("所有BoneReference有效时重入仍保留失败资格"), Fixture.IsValid());
	const TArray<FTransform> Rejected = Fixture.Evaluate();
	if (TestEqual(TEXT("失败重入无越界且保留当前八骨"), Rejected.Num(), 8))
	{
		TestTrue(TEXT("失败诊断不被Initialize清除而错误提交"), Rejected[1].GetRotation().Equals(FQuat::Identity, 1.e-6));
	}

	Fixture.Node.RightLeg.KneeBendAxisLocal = FVector::ForwardVector;
	TestFalse(TEXT("改配置仍需原生CacheBones重新准入"), Fixture.IsValid());
	Fixture.CacheBones();
	if (!TestTrue(TEXT("完整新缓存可重新准入"), Fixture.IsValid())) return false;
	const TArray<FTransform> Recovered = Fixture.Evaluate();
	if (TestEqual(TEXT("重新准入后实际求解完整八骨"), Recovered.Num(), 8))
	{
		TestFalse(TEXT("重新准入不偷换中立姿态"), Recovered[1].GetRotation().Equals(FQuat::Identity, 1.e-5));
	}
	return true;
}
#endif
