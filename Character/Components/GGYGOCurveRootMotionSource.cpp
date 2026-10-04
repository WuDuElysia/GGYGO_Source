#include "Character/Components/GGYGOCurveRootMotionSource.h"

#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "GameFramework/Character.h"

FRootMotionSource_GGYGOCurve::FRootMotionSource_GGYGOCurve()
{
	AccumulateMode = ERootMotionAccumulateMode::Override;

	// 无限时长，由 `PrepareRootMotion` 自己判定结束。
	// 曲线什么时候归零取决于当前播的是哪个动画，提交时无法预知，
	// 给一个固定 Duration 只会在动画被打断时切错。
	Duration = -1.0f;

	// 世界空间。基类的局部空间用的是**当前**组件旋转，而曲线要的是段起点朝向，
	// 两者在转身过程中会差出角色已转过的角度。方向转换由 `BaseYaw` 自己做。
	bInLocalSpace = false;

	// Z 不参与 Override。地面移动的 Z 速度本就接近 0，但 `PrepareRootMotion`
	// 运行在 phys 函数之前，存在"本帧刚离地、source 还没退场"的一帧窗口；
	// 不保护 Z 的话那一帧的重力速度会被曲线的 Z=0 抹掉。
	Settings.SetFlag(ERootMotionSourceSettingsFlags::IgnoreZAccumulate);
}

FRootMotionSource* FRootMotionSource_GGYGOCurve::Clone() const
{
	return new FRootMotionSource_GGYGOCurve(*this);
}

bool FRootMotionSource_GGYGOCurve::Matches(const FRootMotionSource* Other) const
{
	if (!FRootMotionSource::Matches(Other))
	{
		return false;
	}

	// 基类已经保证 ScriptStruct 相同，可以安全 static_cast。
	const FRootMotionSource_GGYGOCurve* OtherCast = static_cast<const FRootMotionSource_GGYGOCurve*>(Other);
	if ((Origin.IsValid() || OtherCast->Origin.IsValid()) && Origin != OtherCast->Origin)
	{
		return false;
	}

	return FMath::IsNearlyEqual(BaseYaw, OtherCast->BaseYaw, 0.1f)
		&& FMath::IsNearlyEqual(SpeedScale, OtherCast->SpeedScale, UE_SMALL_NUMBER)
		&& bEndOnZeroSpeed == OtherCast->bEndOnZeroSpeed;
}

bool FRootMotionSource_GGYGOCurve::MatchesAndHasSameState(const FRootMotionSource* Other) const
{
	return Matches(Other) && FRootMotionSource::MatchesAndHasSameState(Other);
}

bool FRootMotionSource_GGYGOCurve::UpdateStateFrom(const FRootMotionSource* SourceToTakeStateFrom, bool bMarkForSimulatedCatchup)
{
	if (!SourceToTakeStateFrom || SourceToTakeStateFrom->GetScriptStruct() != GetScriptStruct()) return false;
	const FRootMotionSource_GGYGOCurve* Other = static_cast<const FRootMotionSource_GGYGOCurve*>(SourceToTakeStateFrom);
	if ((Origin.IsValid() || Other->Origin.IsValid()) && Origin != Other->Origin) return false;
	return FRootMotionSource::UpdateStateFrom(SourceToTakeStateFrom, bMarkForSimulatedCatchup);
}

void FRootMotionSource_GGYGOCurve::PrepareRootMotion(
	float SimulationTime,
	float MovementTickTime,
	const ACharacter& Character,
	const UCharacterMovementComponent& MoveComponent)
{
	RootMotionParams.Clear();
	UGGYGOCharacterMovementComponent* OriginalOwner = Origin.IsValid() ? Origin->Owner.Get() : nullptr;
	if (!OriginalOwner || OriginalOwner != Character.GetCharacterMovement()
		|| static_cast<const UCharacterMovementComponent*>(OriginalOwner) != &MoveComponent)
	{
		// A retired/unproven contribution must not override a successor or independent action.
		RootMotionParams.Set(FTransform::Identity);
		AccumulateMode = ERootMotionAccumulateMode::Additive;
		Status.SetFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
		Prepared.Reset();
		if (!bOriginDiagnosticReported)
		{
			bOriginDiagnosticReported = true;
			UE_LOG(LogTemp, Error,
				TEXT("Movement CurveRMS rejected: Character='%s', Source='%s', OriginalCMC='%s', Reason='missing or mismatched local Origin; imported NetSerialize origins are not supported'."),
				*Character.GetPathName(), *InstanceName.ToString(), *GetPathNameSafe(OriginalOwner));
		}
		return;
	}

	TSharedPtr<const FGGYGOCurveRootMotionPrepared> Result;
	FString Error;
	const EGGYGOCurveRootMotionPrepareResult Disposition =
		OriginalOwner->PrepareLocomotionCurveRootMotion(*this, SimulationTime, MovementTickTime, Result, Error);
	if (Disposition == EGGYGOCurveRootMotionPrepareResult::Prepared
		|| Disposition == EGGYGOCurveRootMotionPrepareResult::Finished)
	{
		check(Result.IsValid());
		Prepared = Result;
		RootMotionParams.Set(FTransform(Result->OverrideVelocity));
		SetTime(Result->NativeEndTime);
		if (Disposition == EGGYGOCurveRootMotionPrepareResult::Finished)
		{
			Status.SetFlag(ERootMotionSourceStatusFlags::Finished);
		}
		return;
	}

	Prepared.Reset();
	RootMotionParams.Set(FTransform::Identity);
	Status.SetFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
	if (Disposition != EGGYGOCurveRootMotionPrepareResult::Failed)
	{
		// Native traversal still accumulates removed sources. Additive identity is retirement,
		// not a replacement movement mode, and cannot zero a later request's Override.
		AccumulateMode = ERootMotionAccumulateMode::Additive;
	}
	if (!Error.IsEmpty() && !bOriginDiagnosticReported)
	{
		bOriginDiagnosticReported = true;
		UE_LOG(LogTemp, Error,
			TEXT("Movement CurveRMS rejected: Character='%s', Source='%s', CMC='%s', ExecutionRequest=%llu, MovementSet='%s', Reason='%s'."),
			*Character.GetPathName(), *InstanceName.ToString(), *OriginalOwner->GetPathName(),
			static_cast<unsigned long long>(Origin->ExecutionRequestSerial),
			*GetPathNameSafe(Origin->MovementSet.Get()), *Error);
	}
}
bool FRootMotionSource_GGYGOCurve::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	if (Ar.IsLoading())
	{
		Origin.Reset();
		Prepared.Reset();
		bOriginDiagnosticReported = false;
	}
	if (!FRootMotionSource::NetSerialize(Ar, Map, bOutSuccess))
	{
		return false;
	}

	Ar << BaseYaw;
	Ar << SpeedScale;
	Ar << bEndOnZeroSpeed;

	bOutSuccess = true;
	return true;
}

UScriptStruct* FRootMotionSource_GGYGOCurve::GetScriptStruct() const
{
	return FRootMotionSource_GGYGOCurve::StaticStruct();
}

FString FRootMotionSource_GGYGOCurve::ToSimpleString() const
{
	return FString::Printf(TEXT("[ID:%u]FRootMotionSource_GGYGOCurve %s BaseYaw(%.1f)"),
		LocalID, *InstanceName.GetPlainNameString(), BaseYaw);
}
