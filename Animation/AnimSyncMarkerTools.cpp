#include "Animation/AnimSyncMarkerTools.h"
#include "Animation/AnimSequence.h"

#if WITH_EDITOR
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Curves/RichCurve.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectGlobals.h"
#endif

namespace
{
#if WITH_EDITOR
	// 加 BakeTool 前缀避免与 Pipeline/Parameters/RootMotionParameterProcessor.cpp 里的同名常量冲突。
	// 两者都在匿名命名空间里定义同样的曲线名，一旦被 UE 的 unity build 合进同一个编译单元就会重定义。
	// 这两处引用的是同一套 ZZZ 曲线，本应共享一份定义；等 RootMotionParameterProcessor
	// 随移动层重建退役后，再把曲线名收敛到一处。
	const FName BakeToolCurveNamePosX(TEXT("RM_PosX"));
	const FName BakeToolCurveNamePosY(TEXT("RM_PosY"));
	const FName BakeToolCurveNameVelocityDirectionX(TEXT("RM_VelocityDirX"));
	const FName BakeToolCurveNameVelocityDirectionY(TEXT("RM_VelocityDirY"));

	float SanitizeCurveSample(const float Value)
	{
		return FMath::IsFinite(Value) ? Value : 0.f;
	}

	void HandleBakeVelocityDirectionCurvesCommand(const TArray<FString>& Args)
	{
		if (Args.Num() == 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Usage: ZZZBakeVelocityDirectionCurves <AnimSequenceObjectPath> [...]. Paths must include the asset object name."));
			return;
		}

		for (const FString& ObjectPath : Args)
		{
			UAnimSequence* Anim = LoadObject<UAnimSequence>(nullptr, *ObjectPath);
			if (!Anim)
			{
				UE_LOG(LogTemp, Error,
					TEXT("[AnimTools] Failed to load AnimSequence: %s"),
					*ObjectPath);
				continue;
			}

			const int32 NumKeys = UAnimSyncMarkerTools::BakeVelocityDirectionCurves(Anim);
			UE_LOG(LogTemp, Display,
				TEXT("[AnimTools] %s velocity direction curves: %s (%d keys). Package remains dirty until explicitly saved."),
				*Anim->GetPathName(),
				NumKeys > 0 ? TEXT("baked") : TEXT("failed"),
				NumKeys);
		}
	}

	static FAutoConsoleCommand GZZZBakeVelocityDirectionCurvesCommand(
		TEXT("ZZZBakeVelocityDirectionCurves"),
		TEXT("Bake RM_VelocityDirX/Y FloatCurves from RM_PosX/RM_PosY for one or more AnimSequence object paths."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleBakeVelocityDirectionCurvesCommand));
#endif
}

int32 UAnimSyncMarkerTools::SetAuthoredSyncMarkers(
	UAnimSequence* Anim,
	const TArray<FName>& MarkerNames,
	const TArray<float>& Times)
{
#if WITH_EDITOR
	if (!Anim)
	{
		return 0;
	}

	const int32 Num = FMath::Min(MarkerNames.Num(), Times.Num());

	// 用传入数据覆盖同步标记。
	Anim->AuthoredSyncMarkers.Empty(Num);
	for (int32 i = 0; i < Num; ++i)
	{
		FAnimSyncMarker Marker;
		Marker.MarkerName = MarkerNames[i];
		Marker.Time = Times[i];
		Anim->AuthoredSyncMarkers.Add(Marker);
	}

	// 运行时同步要求标记按时间升序。
	Anim->AuthoredSyncMarkers.Sort([](const FAnimSyncMarker& A, const FAnimSyncMarker& B)
	{
		return A.Time < B.Time;
	});

	// 重建唯一标记名等缓存，并标脏以便保存。
	Anim->RefreshSyncMarkerDataFromAuthored();
	Anim->MarkPackageDirty();

	return Anim->AuthoredSyncMarkers.Num();
#else
	return 0;
#endif
}

int32 UAnimSyncMarkerTools::BakeVelocityDirectionCurves(UAnimSequence* Anim)
{
#if WITH_EDITOR
	if (!Anim)
	{
		return 0;
	}

	const IAnimationDataModel* DataModel = Anim->GetDataModel();
	if (!DataModel)
	{
		return 0;
	}

	const FAnimationCurveIdentifier PosXIdentifier(
		BakeToolCurveNamePosX,
		ERawCurveTrackTypes::RCT_Float);
	const FAnimationCurveIdentifier PosYIdentifier(
		BakeToolCurveNamePosY,
		ERawCurveTrackTypes::RCT_Float);
	const FAnimationCurveIdentifier DirectionXIdentifier(
		BakeToolCurveNameVelocityDirectionX,
		ERawCurveTrackTypes::RCT_Float);
	const FAnimationCurveIdentifier DirectionYIdentifier(
		BakeToolCurveNameVelocityDirectionY,
		ERawCurveTrackTypes::RCT_Float);

	const FFloatCurve* PosXCurve = DataModel->FindFloatCurve(PosXIdentifier);
	const FFloatCurve* PosYCurve = DataModel->FindFloatCurve(PosYIdentifier);

	// RM_PosX/RM_PosY 保留原始曲线分量（X=左右、Y=前后）的累计位置。
	// 运行时由 MotionDriver 转换为 Bone_Root/UE 局部 X=前、Y=右，
	// 因此方向直接由根轨迹相邻采样点差分得到。
	// 没有 RM_PosX/RM_PosY 的静止动画仍需拥有真实方向曲线；按采样帧生成全零方向，
	// 这样运行时可以统一采样，且不会把缺失位置轨迹误判为有效位移。
	const int32 NumSampledKeys = Anim->GetNumberOfSampledKeys();
	if (NumSampledKeys <= 0)
	{
		return 0;
	}

	TArray<FRichCurveKey> DirectionXKeys;
	TArray<FRichCurveKey> DirectionYKeys;
	DirectionXKeys.Reserve(NumSampledKeys);
	DirectionYKeys.Reserve(NumSampledKeys);

	float PreviousPosX = 0.f;
	float PreviousPosY = 0.f;
	for (int32 Frame = 0; Frame < NumSampledKeys; ++Frame)
	{
		const float Time = Anim->GetTimeAtFrame(Frame);
		const float CurrentPosX = PosXCurve
			? SanitizeCurveSample(PosXCurve->Evaluate(Time))
			: 0.f;
		const float CurrentPosY = PosYCurve
			? SanitizeCurveSample(PosYCurve->Evaluate(Time))
			: 0.f;

		float DirectionX = 0.f;
		float DirectionY = 0.f;
		if (Frame > 0)
		{
			const float DeltaX = CurrentPosX - PreviousPosX;
			const float DeltaY = CurrentPosY - PreviousPosY;
			const float DeltaLength = FMath::Sqrt(DeltaX * DeltaX + DeltaY * DeltaY);
			if (FMath::IsFinite(DeltaLength) && DeltaLength > KINDA_SMALL_NUMBER)
			{
				DirectionX = DeltaX / DeltaLength;
				DirectionY = DeltaY / DeltaLength;
			}
		}

		DirectionXKeys.Emplace(Time, DirectionX);
		DirectionYKeys.Emplace(Time, DirectionY);
		PreviousPosX = CurrentPosX;
		PreviousPosY = CurrentPosY;
	}

	IAnimationDataController& Controller = Anim->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("Bake velocity direction curves")), false);

	bool bSuccess = true;
	if (!DataModel->FindFloatCurve(DirectionXIdentifier))
	{
		bSuccess = Controller.AddCurve(DirectionXIdentifier, AACF_Editable, false);
	}
	if (bSuccess && !DataModel->FindFloatCurve(DirectionYIdentifier))
	{
		bSuccess = Controller.AddCurve(DirectionYIdentifier, AACF_Editable, false);
	}
	if (bSuccess)
	{
		bSuccess = Controller.SetCurveKeys(DirectionXIdentifier, DirectionXKeys, false);
	}
	if (bSuccess)
	{
		bSuccess = Controller.SetCurveKeys(DirectionYIdentifier, DirectionYKeys, false);
	}

	Controller.CloseBracket(false);
	if (!bSuccess)
	{
		return 0;
	}

	Anim->MarkPackageDirty();
	return NumSampledKeys;
#else
	return 0;
#endif
}
