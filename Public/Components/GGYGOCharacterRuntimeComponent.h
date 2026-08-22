/**
 * @file GGYGOCharacterRuntimeComponent.h
 * @brief GGYGO 角色控制 Pipeline 的 Unreal 生命周期薄壳
 *
 * 组件只负责创建、初始化和转发给 FCharacterControlPipeline；自身不参与独立 Tick。
 * 角色逻辑的所有权和每帧阶段顺序由纯 C++ Pipeline 统一维护。
 */
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Pipeline/CharacterControlPipeline.h"
#include "GGYGOCharacterRuntimeComponent.generated.h"

class ABaseCharacter;
class UAbilitySystemComponent;
class USkeletalMeshComponent;

class FCharacterControlPipeline;
struct FRuntimeData;

/**
 * 角色控制 Pipeline 的 Unreal 生命周期薄壳。
 *
 * 该组件只持有一个 FCharacterControlPipeline，并提供初始化、帧转发和输入门面；
 * 它不启用 Component Tick，调用顺序由 ABaseCharacter::Tick 保持绝对可见。
 */
UCLASS(ClassGroup = (GGYGO), meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOCharacterRuntimeComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGGYGOCharacterRuntimeComponent();
	virtual ~UGGYGOCharacterRuntimeComponent() override;

	/**
	 * 注入角色、ASC 和 Mesh 依赖，并按既有 BeginPlay 顺序初始化单一控制 Pipeline。
	 * 重复调用不会重复注册处理器、仲裁器或状态实例。
	 */
	void InitializeRuntime(
		ABaseCharacter* InOwner,
		UAbilitySystemComponent* InASC,
		USkeletalMeshComponent* InMesh);

	/**
	 * 唯一运行时帧入口，转发给 FCharacterControlPipeline。
	 * Pipeline 内部顺序：Capture → Intent → Decision → Motion Commit → Animation Publish → Reset。
	 */
	void ProcessFrame(float DeltaTime);

	/** 获取由控制 Pipeline 拥有的运行时黑板，返回非拥有指针。 */
	FRuntimeData* GetRuntimeData() const;

	/** BaseCharacter 输入门面。 */
	void SetMoveInput(const FVector2D& Value);
	void ClearMoveInput();
	void SetLookInput(const FVector2D& Value);
	void SetSprintHeld(bool bHeld);
	void SetForceWalkHeld(bool bHeld);

	/** 返回控制 Pipeline 的初始化状态。 */
	bool IsInitialized() const;

private:
	/** 唯一纯 C++ 运行时对象；其内部拥有数据、阶段管线、状态和运动驱动器。 */
	TUniquePtr<FCharacterControlPipeline> ControlPipeline;
};
