#include "Character/Components/GGYGOCurveRootMotionSource.h"

#include "Character/Components/GGYGOCharacterMovementComponent.h"
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

	return FMath::IsNearlyEqual(BaseYaw, OtherCast->BaseYaw, 0.1f)
		&& FMath::IsNearlyEqual(SpeedScale, OtherCast->SpeedScale, UE_SMALL_NUMBER)
		&& bEndOnZeroSpeed == OtherCast->bEndOnZeroSpeed;
}

bool FRootMotionSource_GGYGOCurve::MatchesAndHasSameState(const FRootMotionSource* Other) const
{
	// 本类型没有独立的运行状态：每帧的速度与方向都是从组件现取的，
	// 时间由基类维护。所以基类的检查已经足够。
	return FRootMotionSource::MatchesAndHasSameState(Other);
}

bool FRootMotionSource_GGYGOCurve::UpdateStateFrom(const FRootMotionSource* SourceToTakeStateFrom, bool bMarkForSimulatedCatchup)
{
	return FRootMotionSource::UpdateStateFrom(SourceToTakeStateFrom, bMarkForSimulatedCatchup);
}

void FRootMotionSource_GGYGOCurve::PrepareRootMotion(
	float SimulationTime,
	float MovementTickTime,
	const ACharacter& Character,
	const UCharacterMovementComponent& MoveComponent)
{
	RootMotionParams.Clear();

	const UGGYGOCharacterMovementComponent* CurveMoveComp = Cast<UGGYGOCharacterMovementComponent>(&MoveComponent);

	// 退场帧必须把当前速度原样输出，不能置 Finished 就直接返回。
	//
	// 引擎在 `FRootMotionSourceGroup::PrepareRootMotion` 里遍历**所有**有效源
	// （不看 Finished 也不看 MarkedForRemoval），并在调用完 `PrepareRootMotion`
	// 之后无条件置 `bHasOverrideSources`。也就是说置了 Finished 的这一帧
	// Override 照样生效，而 `RootMotionParams` 空着时 translation 是零 ——
	// 水平速度会被硬清一帧。踏空时的表现就是原地竖直下落。
	//
	// 原样输出当前速度让这一帧的 Override 成为空操作，动量得以保留；
	// 引擎下一帧的 `CleanUpInvalidRootMotion` 会把源摘掉，`CalcVelocity` 随即恢复。
	const auto FinishPreservingMomentum = [&]()
	{
		RootMotionParams.Set(FTransform(MoveComponent.Velocity));
		Status.SetFlag(ERootMotionSourceStatusFlags::Finished);
		SetTime(GetTime() + SimulationTime);
	};

	// 离地必须退场。`PhysFalling` 与 `PhysWalking` 用同一个判据跳过 `CalcVelocity`，
	// 也就是说 Override source 在空中会连重力一起顶掉，角色会沿曲线方向平飘出去。
	if (!CurveMoveComp || !MoveComponent.IsMovingOnGround())
	{
		FinishPreservingMomentum();
		return;
	}

	const FGGYGOAnimCurveMotion& Motion = CurveMoveComp->GetCurveMotion();
	const float Speed = Motion.Speed * SpeedScale;
	const bool bNoSpeed = !Motion.HasUsableSpeed() || Speed <= UE_KINDA_SMALL_NUMBER;

	if (bNoSpeed)
	{
		if (bEndOnZeroSpeed)
		{
			FinishPreservingMomentum();
			return;
		}

		// 留场但输出零速度。动画这几帧确实原地不动，退场会让 `CalcVelocity`
		// 用玩家输入把角色推走。
		RootMotionParams.Set(FTransform(FVector::ZeroVector));
		SetTime(GetTime() + SimulationTime);
		return;
	}

	FVector LocalDirection(Motion.Direction.X, Motion.Direction.Y, 0.0f);
	if (LocalDirection.IsNearlyZero())
	{
		// 有速度但没方向时沿基准朝向直行。让速度作废会让角色在刹车段原地不动，
		// 那比方向略有偏差更糟。
		LocalDirection = FVector::ForwardVector;
	}

	const FVector WorldVelocity =
		FRotator(0.0f, BaseYaw, 0.0f).RotateVector(LocalDirection.GetSafeNormal2D()) * Speed;

	// RootMotionParams 的 translation 是**速度**（cm/s），不是本帧位移 ——
	// 引擎在 `AccumulateRootMotionVelocityFromSource` 里直接把它赋给 Velocity。
	FTransform NewTransform(WorldVelocity);

	// 服务器追帧时一个 tick 要补算多段模拟时间。位移是 `MovementTickTime * Velocity`，
	// 要凑出 `Speed * SimulationTime` 的位移就得按这个比例放大速度。
	// 正常帧两者相等，系数为 1。
	const float Multiplier = (MovementTickTime > UE_SMALL_NUMBER) ? (SimulationTime / MovementTickTime) : 1.0f;
	NewTransform.ScaleTranslation(Multiplier);

	RootMotionParams.Set(NewTransform);

	SetTime(GetTime() + SimulationTime);
}

bool FRootMotionSource_GGYGOCurve::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
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
