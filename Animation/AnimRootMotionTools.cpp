#include "Animation/AnimRootMotionTools.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"

#if WITH_EDITOR
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectGlobals.h"
#endif

namespace
{
#if WITH_EDITOR
	/**
	 * 一根骨骼的逐帧 local transform。
	 *
	 * 预取整轨而不是每帧去问 DataModel：父链上每根骨骼每帧都查一次的话，
	 * 一个 144 帧的动画要发上千次查询，而且每次都要自己钳帧号 —— 帧号的合法上界
	 * （GetNumberOfKeys 与 GetNumberOfFrames 差一）很容易搞错并越界。
	 */
	struct FBoneTrackCache
	{
		TArray<FTransform> Frames;
		FTransform Constant = FTransform::Identity;
		bool bAnimated = false;

		FTransform Get(const int32 Frame) const
		{
			if (!bAnimated || Frames.Num() == 0)
			{
				return Constant;
			}
			return Frames[FMath::Clamp(Frame, 0, Frames.Num() - 1)];
		}
	};

	/** 建好从 BoneName 到骨架根的父链缓存，顺序是 [bone, parent, ..., root]。 */
	bool BuildChainCache(const IAnimationDataModel* DataModel,
		const FReferenceSkeleton& RefSkeleton, const FName BoneName,
		TArray<FBoneTrackCache>& OutChain)
	{
		OutChain.Reset();

		int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			return false;
		}

		const TArray<FTransform>& RefPose = RefSkeleton.GetRefBonePose();
		int32 Guard = 0;
		while (BoneIndex != INDEX_NONE && Guard++ < 256)
		{
			const FName CurrentName = RefSkeleton.GetBoneName(BoneIndex);

			FBoneTrackCache Cache;
			if (DataModel->IsValidBoneTrackName(CurrentName))
			{
				DataModel->GetBoneTrackTransforms(CurrentName, Cache.Frames);
				Cache.bAnimated = Cache.Frames.Num() > 0;
			}
			if (!Cache.bAnimated)
			{
				// 没有动画轨道的骨骼整段保持 reference pose。
				Cache.Constant = RefPose.IsValidIndex(BoneIndex)
					? RefPose[BoneIndex]
					: FTransform::Identity;
			}

			OutChain.Add(MoveTemp(Cache));
			BoneIndex = RefSkeleton.GetParentIndex(BoneIndex);
		}

		return OutChain.Num() > 0;
	}

	/** 沿缓存好的父链累乘，得到骨骼在 component 空间的 transform。 */
	FTransform ComponentFromChain(const TArray<FBoneTrackCache>& Chain, const int32 Frame)
	{
		FTransform Result = FTransform::Identity;
		// 从骨骼自身往根走：FTransform 的 `Child * Parent` 表示先应用子的局部变换再应用父的。
		for (const FBoneTrackCache& Cache : Chain)
		{
			Result = Result * Cache.Get(Frame);
		}
		return Result;
	}

	void HandleStripRootMotionCommand(const TArray<FString>& Args)
	{
		if (Args.Num() < 5)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Usage: ZZZStripRootMotion <MotionBone> <RootBone> <AlignToOrigin 0|1> <DryRun 0|1> <AnimObjectPath> [...]"));
			return;
		}

		const FName MotionBone(*Args[0]);
		const FName RootBone(*Args[1]);
		const bool bAlignToOrigin = Args[2] != TEXT("0");
		const bool bDryRun = Args[3] != TEXT("0");

		for (int32 Index = 4; Index < Args.Num(); ++Index)
		{
			UAnimSequence* Anim = LoadObject<UAnimSequence>(nullptr, *Args[Index]);
			if (!Anim)
			{
				UE_LOG(LogTemp, Error, TEXT("[AnimTools] 加载失败: %s"), *Args[Index]);
				continue;
			}

			const int32 NumKeys = UAnimRootMotionTools::StripRootMotionFromBone(
				Anim, MotionBone, RootBone, bAlignToOrigin, bDryRun);
			UE_LOG(LogTemp, Display, TEXT("[AnimTools] %s writtenKeys=%d"),
				*Anim->GetName(), NumKeys);
		}
	}

	static FAutoConsoleCommand GZZZStripRootMotionCommand(
		TEXT("ZZZStripRootMotion"),
		TEXT("Strip the root motion carried by RootBone out of MotionBone's track so the animation plays in place."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleStripRootMotionCommand));
#endif
}

int32 UAnimRootMotionTools::StripRootMotionFromBone(UAnimSequence* Anim,
	FName MotionBoneName, FName RootBoneName, bool bAlignToOrigin, bool bDryRun)
{
#if WITH_EDITOR
	if (!Anim)
	{
		return 0;
	}

	const USkeleton* Skeleton = Anim->GetSkeleton();
	if (!Skeleton)
	{
		UE_LOG(LogTemp, Error, TEXT("[AnimTools] %s 没有骨架"), *Anim->GetName());
		return 0;
	}

	const IAnimationDataModel* DataModel = Anim->GetDataModel();
	if (!DataModel)
	{
		UE_LOG(LogTemp, Error, TEXT("[AnimTools] %s 没有 DataModel"), *Anim->GetName());
		return 0;
	}

	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();

	if (!DataModel->IsValidBoneTrackName(MotionBoneName))
	{
		UE_LOG(LogTemp, Warning, TEXT("[AnimTools] %s 里 %s 没有动画轨道，跳过"),
			*Anim->GetName(), *MotionBoneName.ToString());
		return 0;
	}

	// 预取三条父链：MotionBone 自己、它的父（用于 local <-> component 换算）、RootBone。
	TArray<FBoneTrackCache> MotionChain;
	TArray<FBoneTrackCache> RootChain;
	if (!BuildChainCache(DataModel, RefSkeleton, MotionBoneName, MotionChain)
		|| !BuildChainCache(DataModel, RefSkeleton, RootBoneName, RootChain))
	{
		UE_LOG(LogTemp, Error, TEXT("[AnimTools] %s 骨架里缺少 %s 或 %s"),
			*Anim->GetName(), *MotionBoneName.ToString(), *RootBoneName.ToString());
		return 0;
	}

	// MotionBone 的父链就是 MotionChain 去掉第一个元素。
	TArray<FBoneTrackCache> ParentChain;
	for (int32 i = 1; i < MotionChain.Num(); ++i)
	{
		ParentChain.Add(MotionChain[i]);
	}

	// 帧数以 MotionBone 自己的轨道长度为准，避免与 GetNumberOfKeys 差一而越界。
	const int32 NumFrames = MotionChain[0].bAnimated
		? MotionChain[0].Frames.Num()
		: DataModel->GetNumberOfKeys();
	if (NumFrames <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AnimTools] %s 的 %s 轨道没有关键帧"),
			*Anim->GetName(), *MotionBoneName.ToString());
		return 0;
	}

	// RootBone 首帧的 component 变换里含着 FBX→UE 的坐标系转换（Pyrios 是根骨骼的 Roll 90 度）。
	// 那是坐标系约定而不是 root motion，所以扣除量取「相对首帧的增量」，让首帧的 RM 恒为单位变换。
	const FTransform RootAtFirstFrameInverse = ComponentFromChain(RootChain, 0).Inverse();

	// 扣除后首帧会停在 MotionBone 自己的首帧水平位置上，而那个位置不一定是原点：
	// 收招类动画（Walk_End / Run_End）是「从远处走过来停在原点」，首帧在 -330cm。
	// 这个补偿把它平移回原点。
	FTransform AlignCompensation = FTransform::Identity;
	if (bAlignToOrigin)
	{
		FVector FirstFrameOffset =
			(MotionChain[0].Get(0) * ComponentFromChain(ParentChain, 0)).GetTranslation();
		FirstFrameOffset.Z = 0.f;
		AlignCompensation = FTransform(FQuat::Identity, -FirstFrameOffset, FVector::OneVector);
	}

	TArray<FVector> NewPosKeys;
	TArray<FQuat> NewRotKeys;
	TArray<FVector> NewScaleKeys;
	NewPosKeys.Reserve(NumFrames);
	NewRotKeys.Reserve(NumFrames);
	NewScaleKeys.Reserve(NumFrames);

	FString Diagnostics;
	for (int32 Frame = 0; Frame < NumFrames; ++Frame)
	{
		// 相对首帧的整体位移与转身，满足 RootComponent(f) == RootComponent(0) * RootMotion。
		//
		// 顺序不能反：`RootComponent * 首帧逆` 得到的是「在首帧的局部空间里表达当前姿态」，
		// 那会把水平位移旋转到别的轴上（Pyrios 首帧带 Roll 90 度，水平位移会被转到 Z 轴，
		// 随后被下面的去高度步骤清掉，扣除量就整体变成零）。
		FTransform RootMotion = RootAtFirstFrameInverse * ComponentFromChain(RootChain, Frame);

		// 只保留水平位移与绕竖直轴的转角。高度与 pitch/roll 是动作本身的起伏，
		// 扣掉会让角色沉进地面或前后倾倒。
		FVector RootMotionTranslation = RootMotion.GetTranslation();
		RootMotionTranslation.Z = 0.f;
		const float RootMotionYaw = RootMotion.GetRotation().Rotator().Yaw;
		RootMotion = FTransform(
			FRotator(0.f, RootMotionYaw, 0.f).Quaternion(),
			RootMotionTranslation,
			FVector::OneVector);

		const FTransform ParentComponent = ComponentFromChain(ParentChain, Frame);
		const FTransform MotionLocal = MotionChain[0].Get(Frame);
		const FTransform MotionComponent = MotionLocal * ParentComponent;

		// NewComponent 满足 NewComponent * RootMotion == MotionComponent，
		// 即「原地姿势叠加上整体位移与转身」等于原始姿势。
		// AlignCompensation 是常量平移，放在最外层，不影响上面那条等式的成立。
		const FTransform NewComponent =
			MotionComponent * RootMotion.Inverse() * AlignCompensation;
		const FTransform NewLocal = NewComponent * ParentComponent.Inverse();

		NewPosKeys.Add(NewLocal.GetTranslation());
		NewRotKeys.Add(NewLocal.GetRotation());
		NewScaleKeys.Add(MotionLocal.GetScale3D());

		if (Frame == 0 || Frame == NumFrames / 2 || Frame == NumFrames - 1)
		{
			const FVector OldCompT = MotionComponent.GetTranslation();
			const FVector NewCompT = NewComponent.GetTranslation();
			Diagnostics += FString::Printf(
				TEXT(" | f%d RM=(%.1f %.1f) Yaw=%.1f old=(%.1f %.1f %.1f) new=(%.1f %.1f %.1f)"),
				Frame,
				RootMotionTranslation.X, RootMotionTranslation.Y, RootMotionYaw,
				OldCompT.X, OldCompT.Y, OldCompT.Z,
				NewCompT.X, NewCompT.Y, NewCompT.Z);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("[AnimTools] %s frames=%d%s"),
		*Anim->GetName(), NumFrames, *Diagnostics);

	if (bDryRun)
	{
		return 0;
	}

	IAnimationDataController& Controller = Anim->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("Strip root motion from bone")), false);
	const bool bSuccess = Controller.SetBoneTrackKeys(
		MotionBoneName, NewPosKeys, NewRotKeys, NewScaleKeys, false);
	Controller.CloseBracket(false);

	if (!bSuccess)
	{
		UE_LOG(LogTemp, Error, TEXT("[AnimTools] %s 写入骨骼轨道失败"), *Anim->GetName());
		return 0;
	}

	Anim->MarkPackageDirty();
	return NumFrames;
#else
	return 0;
#endif
}
