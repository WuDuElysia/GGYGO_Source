/**
 * @file GGYGOCameraMode.h
 * @brief 相机模式与模式栈
 *
 * 一个"相机模式"回答一个问题：此刻镜头应该在哪、朝哪、视场角多大。
 * 常规跟随、锁定敌人、大招演出各是一个模式。
 *
 * ## 为什么用栈而不是一个变量
 * 相机状态是叠加的：锁定期间放大招，大招结束后应当回到锁定而不是回到常规跟随。
 * 用单个"当前模式"变量表达不了这种嵌套，调用方只能自己记住"我之前是什么模式"，
 * 而那份记录会在能力被打断时失效。
 *
 * 栈还顺带解决了混合：多个模式同时在栈里且各自有混合权重，
 * 最终视角是它们的加权结果，切换因此天然平滑，不需要额外的过渡状态。
 *
 * ## 谁来推入模式
 * 能力（`UGGYGOGameplayAbility::SetCameraMode`）推入，能力结束时清除。
 * 角色的默认模式由 `UGGYGOCameraComponent` 的委托提供，它永远在栈底，
 * 保证栈不会空 —— 空栈意味着没有视角可算。
 */
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "GGYGOCameraMode.generated.h"

class AActor;
class UCanvas;
class UGGYGOCameraComponent;
class UObject;

/** 相机模式的混合曲线。 */
UENUM(BlueprintType)
enum class EGGYGOCameraModeBlendFunction : uint8
{
	/** 匀速。混合过程可感知到线性，适合调试。 */
	Linear,

	/** 缓入缓出。默认选择，视觉上最不突兀。 */
	EaseInOut,

	/** 先快后慢。适合"迅速切到目标视角再稳下来"的锁定切换。 */
	EaseOut,

	/** 先慢后快。适合演出开场。 */
	EaseIn,

	Count UMETA(Hidden)
};

/** 一个模式算出的视角。 */
USTRUCT(BlueprintType)
struct FGGYGOCameraModeView
{
	GENERATED_BODY()

	FGGYGOCameraModeView();

	/**
	 * 按权重把另一份视角混进本份。
	 *
	 * 旋转用 `FMath::RInterpTo` 之外的方式处理：直接对欧拉角做线性插值会在
	 * 跨越 ±180 度时绕远路，所以走 `FRotator::NormalizeAxis` 后再插值。
	 */
	void Blend(const FGGYGOCameraModeView& Other, float OtherWeight);

	/** 镜头位置。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera")
	FVector Location = FVector::ZeroVector;

	/** 镜头朝向。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera")
	FRotator Rotation = FRotator::ZeroRotator;

	/**
	 * 控制朝向。
	 *
	 * 与 `Rotation` 分开是因为两者用途不同：`Rotation` 决定画面看向哪，
	 * `ControlRotation` 决定"前"是哪个方向（移动输入与瞄准都依赖它）。
	 * 演出镜头可以任意摆 `Rotation` 而不影响玩家的移动方向感。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera")
	FRotator ControlRotation = FRotator::ZeroRotator;

	/** 视场角（度）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera")
	float FieldOfView = 80.0f;
};

/**
 * 相机模式基类。
 *
 * 派生类只需实现 `UpdateView`。混合权重、生命周期由栈统一管理。
 */
UCLASS(Abstract, NotBlueprintable)
class GGYGO_API UGGYGOCameraMode : public UObject
{
	GENERATED_BODY()

public:
	UGGYGOCameraMode();

	/** 拥有本模式的相机组件。 */
	UGGYGOCameraComponent* GetGGYGOCameraComponent() const;

	/** 视角的目标 Actor。取自相机组件的拥有者。 */
	AActor* GetTargetActor() const;

	/** 本模式当前算出的视角。 */
	const FGGYGOCameraModeView& GetCameraModeView() const { return View; }

	/** 推进一帧：先更新视角，再推进混合权重。 */
	void UpdateCameraMode(float DeltaTime);

	/** 当前混合权重，[0, 1]。 */
	float GetBlendWeight() const { return BlendWeight; }

	/**
	 * 直接设定混合权重。
	 *
	 * 用于模式被移出栈时反向退出：把权重强制设为当前值再让它衰减，
	 * 避免退出动画从 1 开始而产生跳变。
	 */
	void SetBlendWeight(float Weight);

	/** 本模式进入栈时调用。 */
	virtual void OnActivation() {}

	/** 本模式离开栈时调用。 */
	virtual void OnDeactivation() {}

	/** 混合到满权重所需时间（秒）。0 表示瞬切。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Blending", meta = (ClampMin = "0.0"))
	float BlendTime = 0.5f;

	/** 混合曲线。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Blending")
	EGGYGOCameraModeBlendFunction BlendFunction = EGGYGOCameraModeBlendFunction::EaseOut;

	/** 缓动指数。仅 EaseIn / EaseOut / EaseInOut 有效。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Blending", meta = (ClampMin = "0.0"))
	float BlendExponent = 4.0f;

protected:
	/** 派生类在这里算出 `View`。 */
	virtual void UpdateView(float DeltaTime);

	/** 视角的枢轴位置。默认取目标 Actor 的视点。 */
	virtual FVector GetPivotLocation() const;

	/** 视角的枢轴朝向。默认取目标的控制朝向。 */
	virtual FRotator GetPivotRotation() const;

	/** 本模式算出的视角。 */
	FGGYGOCameraModeView View;

	/** 视场角（度）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "View", meta = (UIMin = "5.0", UIMax = "170.0", ClampMin = "5.0", ClampMax = "170.0"))
	float FieldOfView = 80.0f;

	/** 俯仰角下限（度）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "View", meta = (UIMin = "-89.9", UIMax = "89.9", ClampMin = "-89.9", ClampMax = "89.9"))
	float ViewPitchMin = -89.0f;

	/** 俯仰角上限（度）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "View", meta = (UIMin = "-89.9", UIMax = "89.9", ClampMin = "-89.9", ClampMax = "89.9"))
	float ViewPitchMax = 89.0f;

private:
	/** 线性混合进度，[0, 1]。混合曲线作用在它上面得到 `BlendWeight`。 */
	float BlendAlpha = 1.0f;

	/** 经过混合曲线后的权重，[0, 1]。栈按它加权。 */
	float BlendWeight = 1.0f;
};

/**
 * 相机模式栈。
 *
 * 栈顶是最新推入的模式，权重最高。求值时从栈底往栈顶依次混合，
 * 于是栈顶模式在其混合完成后完全遮盖下层。
 *
 * 权重已满的模式会把它下面的所有模式从栈里移除 —— 那些模式已经完全不可见，
 * 留着只是白算。这也保证了栈不会随着能力反复激活而无限增长。
 */
UCLASS()
class GGYGO_API UGGYGOCameraModeStack : public UObject
{
	GENERATED_BODY()

public:
	UGGYGOCameraModeStack();

	/** 清空栈。切换 Avatar 时调用。 */
	void ClearStack();

	/**
	 * 推入一个模式。
	 *
	 * 已在栈中的模式会被移到栈顶并保留当前混合权重 ——
	 * 重新从 0 开始混合会让"锁定→大招→回到锁定"的最后一步出现明显跳变。
	 */
	void PushCameraMode(TSubclassOf<UGGYGOCameraMode> CameraModeClass);

	/** 推进所有模式并算出最终视角。 */
	void EvaluateStack(float DeltaTime, FGGYGOCameraModeView& OutCameraModeView);

	/** 栈是否为空。 */
	bool IsStackActivated() const { return CameraModeStack.Num() > 0; }

protected:
	/** 取某个类的模式实例，没有则创建。模式实例复用，不每次 new。 */
	UGGYGOCameraMode* GetCameraModeInstance(TSubclassOf<UGGYGOCameraMode> CameraModeClass);

	/** 推进各模式的混合权重，并丢弃被完全遮盖的模式。 */
	void UpdateStack(float DeltaTime);

	/** 从栈底往栈顶加权混合。 */
	void BlendStack(FGGYGOCameraModeView& OutCameraModeView) const;

	/** 模式实例池。按类复用，避免每次推入都构造新对象。 */
	UPROPERTY()
	TArray<TObjectPtr<UGGYGOCameraMode>> CameraModeInstances;

	/** 当前栈。索引 0 是栈顶。 */
	UPROPERTY()
	TArray<TObjectPtr<UGGYGOCameraMode>> CameraModeStack;
};
