/** @file GGYGOCombatHitQuery.h @brief 由 GA 资源持有的范围重叠、弹体段扫掠与 Actor 去重。 */
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"

class AActor;
class UWorld;
struct FCollisionQueryParams;
struct FCollisionShape;

/** 范围查询只提供原生重叠，不推导 ImpactPoint、法线或物理材质。 */
struct GGYGO_API FGGYGOCombatOverlapFact
{
	TWeakObjectPtr<AActor> SourceActor;
	FVector Origin = FVector::ZeroVector;
	FOverlapResult Overlap;
};

/** Origin 是本次扫掠段起点，Hit 保留物理查询的完整原始结果。 */
struct GGYGO_API FGGYGOCombatSweepFact
{
	TWeakObjectPtr<AActor> SourceActor;
	FVector Origin = FVector::ZeroVector;
	FHitResult Hit;
};

/**
 * 一个实例对应一个 GA 命中窗口或弹体寿命；关闭后不重新打开。
 * GA 决定查询时机、弹体推进和资源归还；本类没有 Tick、委托或伤害提交。
 * 查询成功但没有新命中返回 true；非法输入返回 false、空结果及可定位原因，且不消费去重。
 */
class GGYGO_API FGGYGOCombatHitQuery final
{
public:
	/** 只接受有效权威来源；忽略自身及显式附加 Actor，弹体可把自身加入附加忽略表。 */
	static TSharedPtr<FGGYGOCombatHitQuery> Open(AActor* SourceActor, FString& OutError,
		TConstArrayView<AActor*> IgnoredActors = {});

	FGGYGOCombatHitQuery(const FGGYGOCombatHitQuery&) = delete;
	FGGYGOCombatHitQuery& operator=(const FGGYGOCombatHitQuery&) = delete;
	~FGGYGOCombatHitQuery() = default;

	/** 世界中心／半径 → 原生球体重叠 → 本实例尚未命中的 Actor 事实。 */
	bool OverlapSphere(const FVector& Center, float Radius, ECollisionChannel Channel,
		TArray<FGGYGOCombatOverlapFact>& OutFacts, FString& OutError);

	/** 世界中心／单位旋转／正半尺寸 → 原生盒体重叠 → 本实例尚未命中的 Actor 事实。 */
	bool OverlapBox(const FVector& Center, const FQuat& Rotation, const FVector& HalfExtents,
		ECollisionChannel Channel, TArray<FGGYGOCombatOverlapFact>& OutFacts, FString& OutError);

	/** 原生移动段两端／世界半径 → 连续球扫，回填物材 → 尚未命中的 Actor 事实及独立物理阻挡。
	 *  OutBlockingHit 不参与 Actor 去重；同 Actor 的 Touch→Block 仍必须阻止弹体。 */
	bool SweepSphere(const FVector& From, const FVector& To, float Radius, ECollisionChannel Channel,
		TArray<FGGYGOCombatSweepFact>& OutFacts, FHitResult& OutBlockingHit, FString& OutError);

	/** 幂等归还本实例的来源、忽略表和去重；不销毁来源 Actor 或后继查询实例。 */
	void Close();

private:
	explicit FGGYGOCombatHitQuery(AActor* SourceActor, TConstArrayView<AActor*> IgnoredActors);
	bool PrepareQuery(ECollisionChannel Channel, FCollisionQueryParams& OutParams, FString& OutError) const;
	bool OverlapShape(const FVector& Center, const FQuat& Rotation, const FCollisionShape& Shape,
		ECollisionChannel Channel, TArray<FGGYGOCombatOverlapFact>& OutFacts, FString& OutError);
	bool RecordActor(AActor* Actor);

	TWeakObjectPtr<AActor> Source;
	TWeakObjectPtr<UWorld> World;
	TArray<TWeakObjectPtr<AActor>> Ignored;
	TArray<TWeakObjectPtr<AActor>> HitActors;
};
