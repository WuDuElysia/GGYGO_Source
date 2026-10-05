/** @file GGYGOMeleeTraceShape.h @brief 主 Mesh 上的显式近战查询形状配置。 */
#pragma once

#include "CoreMinimal.h"
#include "GGYGOMeleeTraceShape.generated.h"

/** 两种正常配置均由同一 Trace 执行器处理；非法配置不切换模式。 */
UENUM(BlueprintType)
enum class EGGYGOMeleeTraceShapeMode : uint8
{
	RootTip,
	SocketChains
};

/** 有序控制点组成折线；相邻点各自按固定世界半径分段扫球。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOMeleeTraceChain
{
	GENERATED_BODY()

	/** 同一角色主 Mesh 上的 Socket/Bone 名称，顺序就是查询拓扑。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MeleeTrace")
	TArray<FName> Points;

	/** 世界厘米，不随骨骼或组件 Scale 自动改变。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MeleeTrace", meta = (ClampMin = "1.0"))
	float WorldRadiusCm = 20.0f;
};

/** RootTip 必须恰好一链两点；SocketChains 至少一链，每链至少两点。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOMeleeTraceShape
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MeleeTrace")
	EGGYGOMeleeTraceShapeMode Mode = EGGYGOMeleeTraceShapeMode::RootTip;

	/** 默认空配置明确无效；不以旧字段、默认动作或另一模式补齐。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MeleeTrace")
	TArray<FGGYGOMeleeTraceChain> Chains;
};
