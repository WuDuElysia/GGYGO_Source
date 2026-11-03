/** @file GGYGOFXFieldSettings.h @brief 原 Niagara 表现携带的显式场配置，不拥有播放状态。 */
#pragma once

#include "CoreMinimal.h"
#include "GGYGOFXFieldSettings.generated.h"

class UGGYGODistortionProfile;
class UMaterialInterface;

/**
 * Field 开启态按原材质实际编译结果分类，未知或双场不能当作普通颜色。
 * 关闭态的显式 NoCustomField 仅走标准 Niagara，不提供 Field 分类证据；DualSignedField 仍拒绝图形激活。
 */
UENUM(BlueprintType)
enum class EGGYGOFXFieldMode : uint8 { NoCustomField, DualSignedField };

/** 配置随原 Step／Publication／组件传递；运行登记仅属于原 Niagara 实例。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOFXFieldSettings
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX|Field")
	EGGYGOFXFieldMode Mode = EGGYGOFXFieldMode::NoCustomField;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX|Field")
	TObjectPtr<UGGYGODistortionProfile> Profile = nullptr;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX|Field")
	TArray<TObjectPtr<UMaterialInterface>> FieldMaterials;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "FX|Field")
	FString MaterialContractEvidence;

	/** 仅结构及 Profile 纯配置校验；不要求 World、ShaderMap 或 DedicatedServer 的 GPU。 */
	bool ValidateConfiguration(FString& OutError) const;
	/** 仅比较同一原请求的配置值，不缓存运行时材质资格。 */
	bool HasSameValue(const FGGYGOFXFieldSettings& Other) const;
};
