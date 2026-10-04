/** @file GGYGOMeleeTraceTestReceiver.h @brief 自动化测试使用的反射命中接收器 */
#pragma once

#include "UObject/Object.h"
#include "GGYGOMeleeTraceTestReceiver.generated.h"

class UGGYGOMeleeTraceComponent;
class UPhysicalMaterial;
class AActor;
struct FHitResult;

/** 动态委托需要 UFUNCTION；仅由自动化测试创建，不参与游戏资产配置。 */
UCLASS(Transient)
class UGGYGOMeleeTraceTestReceiver : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOMeleeTraceComponent> Trace;
	int32 HitCount = 0;
	const UPhysicalMaterial* LastPhysicalMaterial = nullptr;
	bool bCloseOnHit = false;
	bool bReopenOnHit = false;
	UFUNCTION()
	void HandleHit(AActor* Actor, const FHitResult& Hit);
};
