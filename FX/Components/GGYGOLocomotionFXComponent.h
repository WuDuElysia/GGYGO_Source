/** @file GGYGOLocomotionFXComponent.h @brief 原运动完成区间驱动的无 Tick 表现适配。 */
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Character/Data/GGYGOMotionExecutionTypes.h"
#include "FX/GGYGOMontageFXPublication.h"
#include "GGYGOLocomotionFXComponent.generated.h"

class UAnimSequence;
struct FGGYGOPawnASCNotice;

/** 作者声明的原 Sequence 窗口；所有时间均为原片坐标，不重复应用 RateScale。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOLocomotionFXCue
{
	GENERATED_BODY()
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Source")
	FName CueName;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Source")
	EGGYGOLocomotionMotionType MotionType = EGGYGOLocomotionMotionType::None;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Source")
	TObjectPtr<UAnimSequence> Sequence = nullptr;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Source", meta = (Units = "s"))
	double BeginSequenceSeconds = -1.;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Source", meta = (Units = "s"))
	double EndSequenceSeconds = -1.;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX")
	TObjectPtr<UNiagaraSystem> System = nullptr;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX|Field")
	FGGYGOFXFieldSettings FieldSettings;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX")
	EGGYGONiagaraEffectSpace Space = EGGYGONiagaraEffectSpace::Unspecified;
	/** None 明确使用原 Mesh 根，不把无效非空挂点回落到根。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX")
	FName SocketName;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX")
	FTransform LocalTransform = FTransform::Identity;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX")
	EGGYGONiagaraEffectStopMode NormalStopMode = EGGYGONiagaraEffectStopMode::Unspecified;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX")
	EGGYGONiagaraEffectStopMode AbortStopMode = EGGYGONiagaraEffectStopMode::Unspecified;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX", meta = (Units = "s"))
	float SeekDeltaSeconds = 0.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX", meta = (Units = "s"))
	float MaxSimTimeSeconds = 0.f;
};

/** 可选角色组件。只消费原完成/退休事实；不发运动请求，不读取动画显示状态。 */
UCLASS(ClassGroup = (GGYGO), meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOLocomotionFXComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UGGYGOLocomotionFXComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	/** 由原资产作者配置；缺少来源时不安装空组件或补默认窗口。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Locomotion FX")
	TArray<FGGYGOLocomotionFXCue> Cues;
protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
private:
	struct FConsumer;
	struct FSource;
	struct FCueResource;
	TSharedPtr<FConsumer> Consumer;
	bool OwnsConsumer(const TSharedPtr<FConsumer>& Original) const;
	bool IsOriginalConsumer(const TSharedPtr<FConsumer>& Original) const;
	bool ValidateCues(FString& OutError) const;
	void ConsumeNotice(const TSharedPtr<FConsumer>& Original, const FGGYGOPawnASCNotice& Notice);
	void ConsumeInterval(const TSharedPtr<FConsumer>& Original,
		const FGGYGOMotionExecutionSnapshot& Interval, bool bAllowNewSource,
		const FGGYGOLocomotionSourceProof* RetiredSource = nullptr);
	void ConsumeRetirement(const TSharedPtr<FConsumer>& Original,
		const FGGYGOLocomotionSourceRetirementFact& Fact);
	void BeginCue(const TSharedPtr<FConsumer>& Original, const TSharedPtr<FSource>& Source,
		const TSharedPtr<FCueResource>& Cue, const FGGYGOMotionExecutionSnapshot& Interval);
	void StartLocalCue(const TSharedPtr<FConsumer>& Original, const TSharedPtr<FSource>& Source,
		const TSharedPtr<FCueResource>& Cue);
	void CloseCue(const TSharedPtr<FCueResource>& Cue, bool bNormal);
	void CloseSources(const TSharedPtr<FConsumer>& Original);
	UFUNCTION() void HandleMovementUpdated(float DeltaSeconds, FVector OldLocation, FVector OldVelocity);
};
