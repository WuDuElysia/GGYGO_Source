/**
 * @file GGYGOLocalPlayer.h
 * @brief 本地玩家 —— 跨关卡存活的玩家级数据宿主
 *
 * 保有主本地玩家的唯一编队预设缓存，以及Input层的原生输入出生资格资源。
 * 输入资格规则归OriginResource；本类只保有原对象并转发生命周期，不观察物理输入。
 *
 * ## 为什么需要缓存而不是每次读盘
 * 单次加载工厂不会持有缓存。编成面板与装配流程各自加载会拿到两份互不相干的副本：
 * 面板改完存盘，装配那边读到的却是自己那份旧数据，
 * 表现为"编好的队进关卡不生效"，而且不报任何错。
 *
 * 缓存在 LocalPlayer 上而不是 PlayerState 或 GameInstance：
 * PlayerState 每次关卡切换都重建；GameInstance 只有一份，
 * 本项目保留固定存档槽位，仅主本地玩家可持久化，次级玩家明确拒绝。
 * LocalPlayer 提供跨关卡存活的宿主；原缓存失效时拒绝使用，不自动换新。
 *
 * 配置在 `DefaultEngine.ini` 的 `LocalPlayerClassName`。没配时引擎用
 * 基类 `ULocalPlayer`，现有取得入口会明确拒绝错误宿主配置。
 */
#pragma once

#include "Engine/LocalPlayer.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "GGYGOLocalPlayer.generated.h"

class UGameInstance;
class UGGYGOSquadPresets;
class UGGYGOMovementInputOriginResource;
class UObject;

UCLASS(Transient)
class GGYGO_API UGGYGOLocalPlayer : public ULocalPlayer
{
	GENERATED_BODY()

public:
	/** Native projection is published only for the original association's confirmed manager view. */
	virtual bool GetProjectionData(FViewport* Viewport, FSceneViewProjectionData& OutProjectionData,
		int32 StereoViewIndex = INDEX_NONE) const override;

	/**
	 * 唯一缓存的原生取得入口；首次加载及缓存返回均校验原主玩家、GI、用户和槽位关联。
	 * 失败清空输出并提供原因；失效原缓存保留，不重读、换新或排队同步重入。
	 */
	bool TryGetSquadPresets(UGGYGOSquadPresets*& OutPresets, FString& OutError) const;

	/** 蓝图兼容入口，只委托 TryGetSquadPresets；失败记录诊断并返回 nullptr。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Player")
	UGGYGOSquadPresets* GetSquadPresets() const;

	/** Pure read: real PlayerAdded alone creates the resource; removal retains it retired. */
	UGGYGOMovementInputOriginResource* GetMovementInputOriginResource() const;

	virtual void PlayerAdded(UGameViewportClient* InViewportClient, int32 InControllerID) override;
	virtual void PlayerAdded(UGameViewportClient* InViewportClient, FPlatformUserId InUserId) override;
	virtual void PlayerRemoved() override;
	virtual void ReceivedPlayerController(APlayerController* NewController) override;
	virtual bool SpawnPlayActor(const FString& URL, FString& OutError, UWorld* InWorld) override;
	virtual void BeginDestroy() override;

private:
	UGGYGOMovementInputOriginResource* PrepareMovementInputOriginForPlayerAdded();
	void FinishMovementInputOriginPlayerAdded(
		const TWeakObjectPtr<UGGYGOMovementInputOriginResource>& OriginalResource,
		TWeakObjectPtr<APlayerController> BeforeSuper);
	void ReportMovementInputOriginFailure(FName Reason,
		const UGGYGOMovementInputOriginResource* Resource) const;

	/** One original resource, including after Removed; never reset to recover qualification. */
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOMovementInputOriginResource> MovementInputOriginResource;

	/**
	 * 编队预设缓存。
	 *
	 * `mutable` 是为了让 `GetSquadPresets()` 保持 const —— 调用方拿编队是读操作，
	 * 首次读盘属于实现细节，不应该迫使所有调用点持有非常量指针。
	 */
	UPROPERTY(Transient)
	mutable TObjectPtr<UGGYGOSquadPresets> SquadPresets;

	/** Read-only provenance of the original publication, not another cache or readiness state. */
	mutable TWeakObjectPtr<UGGYGOSquadPresets> SquadPresetsSourceObject;
	mutable TWeakObjectPtr<const ULocalPlayer> SquadPresetsSourceLocalPlayer;
	mutable TWeakObjectPtr<UGameInstance> SquadPresetsSourceGameInstance;
	mutable FPlatformUserId SquadPresetsSourcePlatformUserId = PLATFORMUSERID_NONE;
	mutable int32 SquadPresetsSourcePlatformUserIndex = INDEX_NONE;
	mutable FString SquadPresetsSourceSlotName;
	mutable FString SquadPresetsSourceGameInstancePath;
	mutable FString SquadPresetsSourceObjectPath;

	/** True only inside a synchronous access scope; never queues or retries a request. */
	mutable bool bSquadPresetsAccessBusy = false;

	/** Diagnostic only; never grants or caches camera publication eligibility. */
	mutable bool bReportedCameraProjectionFailure = false;
};
