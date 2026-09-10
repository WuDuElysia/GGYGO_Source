/**
 * @file GGYGOCameraMode.cpp
 * @brief 相机模式与模式栈实现
 */
#include "Camera/GGYGOCameraMode.h"

#include "Camera/GGYGOCameraComponent.h"
#include "GameFramework/Actor.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCameraMode)

// ============================================================================
// FGGYGOCameraModeView
// ============================================================================

FGGYGOCameraModeView::FGGYGOCameraModeView()
	: Location(ForceInit)
	, Rotation(ForceInit)
	, ControlRotation(ForceInit)
	, FieldOfView(80.0f)
{
}

void FGGYGOCameraModeView::Blend(const FGGYGOCameraModeView& Other, float OtherWeight)
{
	if (OtherWeight <= 0.0f)
	{
		return;
	}

	if (OtherWeight >= 1.0f)
	{
		*this = Other;
		return;
	}

	Location = FMath::Lerp(Location, Other.Location, OtherWeight);

	// 旋转分量先归一化到 [-180, 180] 再插值。
	// 直接对原始欧拉角插值会在跨越 ±180 度时绕远路，表现为镜头猛甩一圈。
	const FRotator DeltaRotation = (Other.Rotation - Rotation).GetNormalized();
	Rotation = Rotation + (OtherWeight * DeltaRotation);

	const FRotator DeltaControlRotation = (Other.ControlRotation - ControlRotation).GetNormalized();
	ControlRotation = ControlRotation + (OtherWeight * DeltaControlRotation);

	FieldOfView = FMath::Lerp(FieldOfView, Other.FieldOfView, OtherWeight);
}

// ============================================================================
// UGGYGOCameraMode
// ============================================================================

UGGYGOCameraMode::UGGYGOCameraMode()
{
}

UGGYGOCameraComponent* UGGYGOCameraMode::GetGGYGOCameraComponent() const
{
	// Outer 必然是相机组件：模式实例只由 UGGYGOCameraModeStack 创建，
	// 而栈的 Outer 是相机组件。
	return CastChecked<UGGYGOCameraComponent>(GetOuter()->GetOuter());
}

AActor* UGGYGOCameraMode::GetTargetActor() const
{
	const UGGYGOCameraComponent* CameraComponent = GetGGYGOCameraComponent();

	return CameraComponent ? CameraComponent->GetTargetActor() : nullptr;
}

FVector UGGYGOCameraMode::GetPivotLocation() const
{
	const AActor* TargetActor = GetTargetActor();
	if (!TargetActor)
	{
		return FVector::ZeroVector;
	}

	// 用 GetPawnViewLocation 而不是 Actor 原点：前者已经算进了眼高，
	// 用原点会让镜头从脚底出发。
	if (const APawn* TargetPawn = Cast<APawn>(TargetActor))
	{
		return TargetPawn->GetPawnViewLocation();
	}

	return TargetActor->GetActorLocation();
}

FRotator UGGYGOCameraMode::GetPivotRotation() const
{
	const AActor* TargetActor = GetTargetActor();
	if (!TargetActor)
	{
		return FRotator::ZeroRotator;
	}

	if (const APawn* TargetPawn = Cast<APawn>(TargetActor))
	{
		return TargetPawn->GetViewRotation();
	}

	return TargetActor->GetActorRotation();
}

void UGGYGOCameraMode::UpdateView(float DeltaTime)
{
	const FVector PivotLocation = GetPivotLocation();
	FRotator PivotRotation = GetPivotRotation();

	// 钳制俯仰。不钳会让镜头翻过头顶，此后左右方向感反转。
	PivotRotation.Pitch = FMath::ClampAngle(PivotRotation.Pitch, ViewPitchMin, ViewPitchMax);

	View.Location = PivotLocation;
	View.Rotation = PivotRotation;
	View.ControlRotation = View.Rotation;
	View.FieldOfView = FieldOfView;
}

void UGGYGOCameraMode::SetBlendWeight(float Weight)
{
	BlendWeight = FMath::Clamp(Weight, 0.0f, 1.0f);

	// 从权重反解出线性进度，这样后续推进能从当前视觉状态接着走。
	// 指数取倒数是上面 BlendWeight 计算的逆运算。
	const float InvExponent = (BlendExponent > SMALL_NUMBER) ? (1.0f / BlendExponent) : 1.0f;

	switch (BlendFunction)
	{
	case EGGYGOCameraModeBlendFunction::Linear:
		BlendAlpha = BlendWeight;
		break;

	case EGGYGOCameraModeBlendFunction::EaseIn:
		BlendAlpha = FMath::InterpEaseIn(0.0f, 1.0f, BlendWeight, InvExponent);
		break;

	case EGGYGOCameraModeBlendFunction::EaseOut:
		BlendAlpha = FMath::InterpEaseOut(0.0f, 1.0f, BlendWeight, InvExponent);
		break;

	case EGGYGOCameraModeBlendFunction::EaseInOut:
		BlendAlpha = FMath::InterpEaseInOut(0.0f, 1.0f, BlendWeight, InvExponent);
		break;

	default:
		BlendAlpha = BlendWeight;
		break;
	}
}

void UGGYGOCameraMode::UpdateCameraMode(float DeltaTime)
{
	UpdateView(DeltaTime);

	// 推进线性进度。BlendTime 为 0 时直接满权重，避免除零。
	if (BlendTime > 0.0f)
	{
		BlendAlpha += (DeltaTime / BlendTime);
		BlendAlpha = FMath::Min(BlendAlpha, 1.0f);
	}
	else
	{
		BlendAlpha = 1.0f;
	}

	const float Exponent = (BlendExponent > 0.0f) ? BlendExponent : 1.0f;

	switch (BlendFunction)
	{
	case EGGYGOCameraModeBlendFunction::Linear:
		BlendWeight = BlendAlpha;
		break;

	case EGGYGOCameraModeBlendFunction::EaseIn:
		BlendWeight = FMath::InterpEaseIn(0.0f, 1.0f, BlendAlpha, Exponent);
		break;

	case EGGYGOCameraModeBlendFunction::EaseOut:
		BlendWeight = FMath::InterpEaseOut(0.0f, 1.0f, BlendAlpha, Exponent);
		break;

	case EGGYGOCameraModeBlendFunction::EaseInOut:
		BlendWeight = FMath::InterpEaseInOut(0.0f, 1.0f, BlendAlpha, Exponent);
		break;

	default:
		BlendWeight = BlendAlpha;
		break;
	}
}

// ============================================================================
// UGGYGOCameraModeStack
// ============================================================================

UGGYGOCameraModeStack::UGGYGOCameraModeStack()
{
}

void UGGYGOCameraModeStack::ClearStack()
{
	for (const TObjectPtr<UGGYGOCameraMode>& Mode : CameraModeStack)
	{
		if (Mode)
		{
			Mode->OnDeactivation();
		}
	}

	CameraModeStack.Reset();
}

UGGYGOCameraMode* UGGYGOCameraModeStack::GetCameraModeInstance(TSubclassOf<UGGYGOCameraMode> CameraModeClass)
{
	check(CameraModeClass);

	for (const TObjectPtr<UGGYGOCameraMode>& Mode : CameraModeInstances)
	{
		if (Mode && Mode->GetClass() == CameraModeClass)
		{
			return Mode;
		}
	}

	// 池里没有才创建。复用实例是有意的：模式可能持有平滑状态
	// （如上一帧的臂长），每次推入都新建会丢掉那些状态并产生跳变。
	UGGYGOCameraMode* NewMode = NewObject<UGGYGOCameraMode>(this, CameraModeClass, NAME_None, RF_NoFlags);
	check(NewMode);

	CameraModeInstances.Add(NewMode);

	return NewMode;
}

void UGGYGOCameraModeStack::PushCameraMode(TSubclassOf<UGGYGOCameraMode> CameraModeClass)
{
	if (!CameraModeClass)
	{
		return;
	}

	UGGYGOCameraMode* CameraMode = GetCameraModeInstance(CameraModeClass);
	check(CameraMode);

	const int32 StackSize = CameraModeStack.Num();

	if (StackSize > 0 && CameraModeStack[0] == CameraMode)
	{
		// 已经在栈顶，无需处理。
		return;
	}

	// 若已在栈中，取出它并记住当前权重 —— 重新从 0 混合会产生可见跳变。
	int32 ExistingStackIndex = INDEX_NONE;
	float ExistingStackContribution = 1.0f;

	for (int32 StackIndex = 0; StackIndex < StackSize; ++StackIndex)
	{
		if (CameraModeStack[StackIndex] == CameraMode)
		{
			ExistingStackIndex = StackIndex;
			ExistingStackContribution *= CameraMode->GetBlendWeight();
			break;
		}

		ExistingStackContribution *= (1.0f - CameraModeStack[StackIndex]->GetBlendWeight());
	}

	if (ExistingStackIndex != INDEX_NONE)
	{
		CameraModeStack.RemoveAt(ExistingStackIndex);
	}
	else
	{
		ExistingStackContribution = 0.0f;
	}

	CameraModeStack.Insert(CameraMode, 0);

	// 栈底模式必须满权重：它是最终视角的基准，权重不满会让画面
	// 混进未初始化的值。
	const bool bIsBottomMode = (CameraModeStack.Num() == 1);
	CameraMode->SetBlendWeight(bIsBottomMode ? 1.0f : ExistingStackContribution);

	if (ExistingStackIndex == INDEX_NONE)
	{
		CameraMode->OnActivation();
	}
}

void UGGYGOCameraModeStack::UpdateStack(float DeltaTime)
{
	const int32 StackSize = CameraModeStack.Num();
	if (StackSize == 0)
	{
		return;
	}

	int32 RemoveIndex = INDEX_NONE;
	int32 RemoveCount = 0;

	for (int32 StackIndex = 0; StackIndex < StackSize; ++StackIndex)
	{
		UGGYGOCameraMode* CameraMode = CameraModeStack[StackIndex];
		CameraMode->UpdateCameraMode(DeltaTime);

		if (CameraMode->GetBlendWeight() >= 1.0f)
		{
			// 该模式已完全遮盖下层，下层再算也看不见。
			RemoveIndex = StackIndex + 1;
			RemoveCount = StackSize - RemoveIndex;
			break;
		}
	}

	if (RemoveCount > 0)
	{
		for (int32 StackIndex = RemoveIndex; StackIndex < StackSize; ++StackIndex)
		{
			CameraModeStack[StackIndex]->OnDeactivation();
		}

		CameraModeStack.RemoveAt(RemoveIndex, RemoveCount);
	}
}

void UGGYGOCameraModeStack::BlendStack(FGGYGOCameraModeView& OutCameraModeView) const
{
	const int32 StackSize = CameraModeStack.Num();
	if (StackSize == 0)
	{
		return;
	}

	// 从栈底开始：它是基准，权重视为 1。
	OutCameraModeView = CameraModeStack[StackSize - 1]->GetCameraModeView();

	// 往栈顶方向逐层混合。上层权重越高，遮盖下层越彻底。
	for (int32 StackIndex = StackSize - 2; StackIndex >= 0; --StackIndex)
	{
		const UGGYGOCameraMode* CameraMode = CameraModeStack[StackIndex];
		OutCameraModeView.Blend(CameraMode->GetCameraModeView(), CameraMode->GetBlendWeight());
	}
}

void UGGYGOCameraModeStack::EvaluateStack(float DeltaTime, FGGYGOCameraModeView& OutCameraModeView)
{
	UpdateStack(DeltaTime);
	BlendStack(OutCameraModeView);
}
