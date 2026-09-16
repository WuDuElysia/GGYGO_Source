/**
 * @file GGYGOCameraComponent.cpp
 * @brief 相机组件实现
 */
#include "Camera/GGYGOCameraComponent.h"

#include "Camera/GGYGOCameraMode.h"
#include "Engine/Scene.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCameraComponent)

UGGYGOCameraComponent::UGGYGOCameraComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	CameraModeStack = nullptr;

	// 关掉引擎的"用 Pawn 控制旋转"：视角朝向由相机模式给出。
	// 两者同时生效会让模式算出的朝向被 Pawn 的控制旋转覆盖。
	bUsePawnControlRotation = false;
}

void UGGYGOCameraComponent::OnRegister()
{
	Super::OnRegister();

	if (!CameraModeStack)
	{
		// 栈的 Outer 是本组件。相机模式靠 GetOuter()->GetOuter() 反查组件，
		// 这层关系不能变。
		CameraModeStack = NewObject<UGGYGOCameraModeStack>(this);
	}
}

void UGGYGOCameraComponent::ClearCameraModeStack()
{
	if (CameraModeStack)
	{
		CameraModeStack->ClearStack();
	}
}

void UGGYGOCameraComponent::UpdateCameraModes()
{
	if (!CameraModeStack)
	{
		return;
	}

	// 引擎在需要视图时驱动本组件；外部委托只返回已经完成玩法仲裁的有效模式。
	// 每帧推入同一个类是幂等的，模式变化时则由栈负责平滑混合。
	if (DetermineCameraModeDelegate.IsBound())
	{
		if (const TSubclassOf<UGGYGOCameraMode> CameraMode = DetermineCameraModeDelegate.Execute())
		{
			CameraModeStack->PushCameraMode(CameraMode);
		}
	}
}

void UGGYGOCameraComponent::SetCameraOffset(const FGGYGOCameraOffset& InOffset)
{
	CameraOffset = InOffset;
	bCameraOffsetActive = true;

	// 不重置 alpha：上一份微调可能还没回落完就来了新的一份，
	// 从当前强度继续过渡比归零重来更平滑。
}

void UGGYGOCameraComponent::ClearCameraOffset()
{
	// 保留 CameraOffset 本身，回落期间还要用它的数值和 BlendOutTime。
	bCameraOffsetActive = false;
}

void UGGYGOCameraComponent::UpdateCameraOffsetAlpha(float DeltaTime)
{
	const float BlendTime = bCameraOffsetActive ? CameraOffset.BlendInTime : CameraOffset.BlendOutTime;
	const float Target = bCameraOffsetActive ? 1.0f : 0.0f;

	if (BlendTime <= 0.0f)
	{
		CameraOffsetAlpha = Target;
		return;
	}

	// 按时间线性推进而不是用 FInterpTo：后者是指数逼近，永远到不了端点，
	// 会让微调残留一个极小的偏移量。
	const float Step = DeltaTime / BlendTime;
	CameraOffsetAlpha = FMath::Clamp(
		CameraOffsetAlpha + (bCameraOffsetActive ? Step : -Step), 0.0f, 1.0f);
}

void UGGYGOCameraComponent::ApplyCameraOffset(FGGYGOCameraModeView& View) const
{
	if (CameraOffsetAlpha <= 0.0f || CameraOffset.IsNearlyZero())
	{
		return;
	}

	// 位置偏移在相机局部空间：同一份配置在角色朝任何方向时观感一致。
	const FVector LocalOffset = CameraOffset.LocationOffset * CameraOffsetAlpha;
	View.Location += View.Rotation.RotateVector(LocalOffset);

	// FOV 要钳制：多份配置叠加或误填大值会算出 0 或负数，那会让投影矩阵失效。
	View.FieldOfView = FMath::Clamp(
		View.FieldOfView + CameraOffset.FieldOfViewDelta * CameraOffsetAlpha, 5.0f, 170.0f);
}

void UGGYGOCameraComponent::GetCameraView(float DeltaTime, FMinimalViewInfo& DesiredView)
{
	check(CameraModeStack);

	UpdateCameraModes();
	if (!CameraModeStack->IsStackActivated())
	{
		// PawnData 尚未到达时没有模式可求值。组件保留着上一帧的世界变换，
		// 交给 UCameraComponent 生成视图即可；首次初始化也至少落在 Pawn 上，
		// 不会把未初始化的零向量写成世界原点镜头。
		Super::GetCameraView(DeltaTime, DesiredView);
		return;
	}

	FGGYGOCameraModeView CameraModeView;
	CameraModeStack->EvaluateStack(DeltaTime, CameraModeView);

	// 微调叠加在栈的求值结果之后：它调整的是"最终镜头"，
	// 而不是参与模式之间的加权（那会让它被其它模式的权重稀释）。
	UpdateCameraOffsetAlpha(DeltaTime);
	ApplyCameraOffset(CameraModeView);

	// 把算出的控制朝向写回 Controller。
	//
	// 这一步让"看向哪"与"前是哪个方向"保持一致：移动输入按控制朝向解析，
	// 不同步的话演出镜头期间玩家的前后左右会与画面不符。
	if (const APawn* TargetPawn = Cast<APawn>(GetTargetActor()))
	{
		if (AController* PawnController = TargetPawn->GetController())
		{
			PawnController->SetControlRotation(CameraModeView.ControlRotation);
		}
	}

	// 组件自身的变换也要跟上：附着在相机上的东西（后处理体积、UI 锚点）
	// 读的是组件变换而不是 FMinimalViewInfo。
	SetWorldLocationAndRotation(CameraModeView.Location, CameraModeView.Rotation);
	FieldOfView = CameraModeView.FieldOfView;

	DesiredView.Location = CameraModeView.Location;
	DesiredView.Rotation = CameraModeView.Rotation;
	DesiredView.FOV = CameraModeView.FieldOfView;
	DesiredView.OrthoWidth = OrthoWidth;
	DesiredView.OrthoNearClipPlane = OrthoNearClipPlane;
	DesiredView.OrthoFarClipPlane = OrthoFarClipPlane;
	DesiredView.AspectRatio = AspectRatio;
	DesiredView.bConstrainAspectRatio = bConstrainAspectRatio;
	DesiredView.bUseFieldOfViewForLOD = bUseFieldOfViewForLOD;
	DesiredView.ProjectionMode = ProjectionMode;

	// 后处理设置由组件自己的配置提供，模式栈不参与 —— 它只管几何视角。
	DesiredView.PostProcessBlendWeight = PostProcessBlendWeight;
	if (PostProcessBlendWeight > 0.0f)
	{
		DesiredView.PostProcessSettings = PostProcessSettings;
	}
}
