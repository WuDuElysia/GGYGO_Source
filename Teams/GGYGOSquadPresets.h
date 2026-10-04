/**
 * @file GGYGOSquadPresets.h
 * @brief 玩家保存的编队预设集合 —— 多份编队与当前出战选择
 *
 * 回答"这个玩家有哪几套编队、现在用哪一套"。编成面板读写这里，
 * 进关卡时把出战那一套灌进 `UGGYGOSquadComponent::SetRoster`。
 *
 * ## 为什么不存在 SquadComponent 上
 * 编成面板运行在主菜单，那时没有 World、没有 PlayerState，
 * SquadComponent 根本不存在。而且 PlayerState 在非无缝切换时会重建，
 * 存在它上面的编队会随每次关卡加载丢失。
 *
 * 三层的分工：
 * - **本类**：玩家有哪几套编队（跨关卡，落盘）
 * - `SquadComponent::Roster`：这一局带谁（出战那套的快照，装配后锁定）
 * - `SquadComponent::Slots`：装配结果，ASC 与属性集的宿主
 *
 * ## 成员为什么存 FPrimaryAssetId 而不是资产指针
 * 存档里放 `UGGYGOPawnData*` 会让反序列化拉起**所有**编队里的**所有**角色资产，
 * 包括这局用不到的那几套；而角色资产带着 AbilitySet、动画、相机模式，
 * 是一整条引用链。存 Id 则只在装配时按需加载出战那一套。
 *
 * 用 `FPrimaryAssetId` 而不是 `TSoftObjectPtr` 的理由是它走 AssetManager 索引：
 * 资产改名或移动后仍能解析，且可以在解析前判断"这个角色还存在吗"。
 * 代价是 `GGYGOPawnData` 必须注册为 PrimaryAssetType（见 `DefaultGame.ini`），
 * 未注册、缺失或加载/类型错误明确返回 Invalid；只有合法未配置是正常默认来源模式。
 * GameMode 的结果消费另步适配，不能把 Invalid 当作空名单或默认来源。
 *
 * ## 编辑接口不自动落盘
 * 改完调用方显式调 `AsyncSaveGameToSlotForLocalPlayer()`。面板上连续调整
 * 若每次都写盘，一次拖拽换位就是十几次磁盘 IO。
 *
 * ## 联机边界
 * 这是**客户端本地**数据。服务器拿不到远程玩家的存档，
 * 所以远程玩家的编队必须由客户端上报（尚未实现），
 * 服务器侧目前会回落到玩法配置里的默认编队。
 */
#pragma once

#include "GameFramework/SaveGame.h"
#include "Misc/CoreMiscDefines.h"
#include "UObject/PrimaryAssetId.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "GGYGOSquadPresets.generated.h"

class UGameInstance;
class UGGYGOPawnData;
struct FGGYGOSquadPresetLoadResult;
class ULocalPlayer;
class UObject;

/** One synchronous preset parse outcome; no roster, saved state or cached readiness. */
enum class EGGYGOSquadPresetRosterResolveStatus : uint8
{
	Resolved,
	Unconfigured,
	Invalid
};

struct GGYGO_API FGGYGOSquadPresetRosterResolveResult
{
	EGGYGOSquadPresetRosterResolveStatus Status = EGGYGOSquadPresetRosterResolveStatus::Invalid;
	FString Error;
	FString PresetsPath;
	int32 PresetIndex = INDEX_NONE;
	int32 MemberIndex = INDEX_NONE;
	FPrimaryAssetId MemberId;
	FSoftObjectPath MemberPath;
};

/**
 * 一套编队：一个玩家起的名字加一串角色。
 *
 * 成员允许为空 Id —— 面板上留空位是合法编成状态，解析时跳过。
 */
USTRUCT(BlueprintType)
struct FGGYGOSquadPreset
{
	GENERATED_BODY()

	/** 玩家起的名字。用 FString 而不是 FText：玩家输入的名字不需要本地化。 */
	UPROPERTY(BlueprintReadWrite, Category = "GGYGO|Squad")
	FString DisplayName;

	/** 成员角色，按出场顺序。容量上限为 `GGYGO_MAX_SQUAD_SIZE`；超限旧数据保留但拒绝解析。 */
	UPROPERTY(BlueprintReadWrite, Category = "GGYGO|Squad")
	TArray<FPrimaryAssetId> Members;

	/** 一个有效成员都没有（全空或长度为 0）。这种编队不能出战。 */
	bool HasAnyMember() const;
};

/**
 * 编队预设存档：保留现有固定槽位，仅主本地玩家可加载或新建。
 *
 * ULocalPlayerSaveGame 提供原生关联、初始化和保存生命周期。
 * 本类的显式加载入口拒绝已有坏档；唯一持久缓存由 LocalPlayer 宿主持有。
 * 次级玩家在访问存储前明确拒绝，不自动迁移、重置或覆盖原文件。
 */
UCLASS(BlueprintType)
class GGYGO_API UGGYGOSquadPresets : public ULocalPlayerSaveGame
{
	GENERATED_BODY()

public:
	/** 存档槽位名。 */
	static const FString SaveSlotName;

	/**
	 * 主本地玩家的单次同步加载。仅原生后端明确报告缺档时创建空内存对象。
	 * 已有档的读取、类型、归档错误或原出战索引错误返回 Invalid，不创建替代对象。
	 * 结果携带原来源身份及本次作用域强引用；调用方在结果释放前发布到唯一宿主缓存。
	 * 不保存、迁移或修复存档，也不建立持久缓存。
	 */
	static FGGYGOSquadPresetLoadResult TryLoadForLocalPlayer(const ULocalPlayer* LocalPlayer);

	/** 兼容入口，只委托 TryLoadForLocalPlayer；Invalid 返回 nullptr，不创建替代对象。 */
	static UGGYGOSquadPresets* LoadOrCreateForLocalPlayer(const ULocalPlayer* LocalPlayer);

	/** 编队套数。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	int32 GetPresetCount() const { return Presets.Num(); }

	/** 全部编队，面板列表用。 */
	const TArray<FGGYGOSquadPreset>& GetPresets() const { return Presets; }

	/** 指定编队。越界返回 nullptr。 */
	const FGGYGOSquadPreset* GetPreset(int32 PresetIndex) const;

	/** 当前出战编队的序号。没有任何编队时为 `INDEX_NONE`。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Squad")
	int32 GetActivePresetIndex() const { return ActivePresetIndex; }

	/** 当前出战编队。没有则返回 nullptr。 */
	const FGGYGOSquadPreset* GetActivePreset() const { return GetPreset(ActivePresetIndex); }

	/**
	 * 新增一套空编队。
	 *
	 * 编队套数不设上限 —— 限制的是每套的人数（`GGYGO_MAX_SQUAD_SIZE`），
	 * 存几十套编队只是几十行 Id，不产生运行时开销。
	 *
	 * @return 新编队的序号。这是第一套时它自动成为出战编队。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	int32 AddPreset(const FString& DisplayName);

	/**
	 * 删除一套编队。
	 *
	 * 删除会影响出战序号：删掉出战编队本身或它前面的编队，序号都要跟着动，
	 * 否则出战指向会错位到另一套编队上。本函数负责修正。
	 *
	 * @return 是否删除了。越界返回 false。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool RemovePreset(int32 PresetIndex);

	/** 改名。越界返回 false。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool RenamePreset(int32 PresetIndex, const FString& NewDisplayName);

	/**
	 * 替换一套编队的成员。超过 `GGYGO_MAX_SQUAD_SIZE` 时拒绝，不截断或修改原名单。
	 *
	 * @return 是否写入了。编队序号越界或成员数量超限返回 false，原名单保持不变。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool SetPresetMembers(int32 PresetIndex, const TArray<FPrimaryAssetId>& NewMembers);

	/**
	 * 选择出战编队。仅在局外有意义 —— 装配之后改这里不影响当前这一局，
	 * 因为 `SquadComponent` 已经按旧选择装配完了。
	 *
	 * @return 是否选中。越界返回 false。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Squad")
	bool SetActivePresetIndex(int32 PresetIndex);

	/**
	 * 同步解析出战预设；无预设且选择为 INDEX_NONE，或合法空编队返回 Unconfigured。
	 * 只默认 FPrimaryAssetId() 是合法空位；非法索引/Id、超限或资产错误返回 Invalid。
	 * 入口清空 OutRoster，只有全部非空成员成功解析才一次发布完整名单并返回 Resolved。
	 * 结果只说明本次原输入的解析，不保证 PawnClass/能力配置或角色装配成功。
	 * 调用方须消费 Status；Invalid 不能使用默认编队或部分名单掩盖。
	 */
	FGGYGOSquadPresetRosterResolveResult ResolveActivePresetRoster(TArray<UGGYGOPawnData*>& OutRoster) const;

	/** 解析指定预设；任意越界索引均为 Invalid，包括 INDEX_NONE。输出与失败契约同上。 */
	FGGYGOSquadPresetRosterResolveResult ResolvePresetRoster(int32 PresetIndex, TArray<UGGYGOPawnData*>& OutRoster) const;
	//~UObject interface
	virtual void Serialize(FArchive& Ar) override;
	//~End of UObject interface

	//~ULocalPlayerSaveGame interface
	/** Primary native source admission only; true means the native request was accepted, not saved successfully. */
	virtual bool SaveGameToSlotForLocalPlayer() override;
	virtual bool AsyncSaveGameToSlotForLocalPlayer() override;
	virtual int32 GetLatestDataVersion() const override;
	virtual void HandlePostLoad() override;
	//~End of ULocalPlayerSaveGame interface

protected:
	/** 玩家保存的编队。顺序即面板显示顺序。 */
	UPROPERTY()
	TArray<FGGYGOSquadPreset> Presets;

	/**
	 * 出战编队的序号。
	 *
	 * 存序号而不是存一份编队副本：副本会与被编辑的原编队脱钩，
	 * 玩家改了出战编队的成员却发现进关卡还是旧阵容。
	 */
	UPROPERTY()
	int32 ActivePresetIndex = INDEX_NONE;

private:
	/** Shared synchronous admission for the explicitly requested native save mode; no save state or executor. */
	bool RequestSaveForOriginalSource(bool bAsync);

	/** 唯一同步解析实现；bResolveActivePreset 仅区分正常无预设与显式非法索引。 */
	FGGYGOSquadPresetRosterResolveResult ResolvePresetRosterInternal(int32 RequestedPresetIndex,
		bool bResolveActivePreset, TArray<UGGYGOPawnData*>& OutRoster) const;

	/** Derived facts from the last actual object loading archive; never serialized or used as readiness. */
	bool bObservedLoadingArchive = false;
	bool bLoadingArchiveHadError = false;
};

/** One synchronous load outcome. Only a successful result owns a scoped candidate. */
enum class EGGYGOSquadPresetLoadStatus : uint8
{
	Loaded,
	CreatedForMissingSlot,
	Invalid
};

struct GGYGO_API FGGYGOSquadPresetLoadResult
{
	EGGYGOSquadPresetLoadStatus Status = EGGYGOSquadPresetLoadStatus::Invalid;
	TWeakObjectPtr<const ULocalPlayer> OriginalLocalPlayer;
	TWeakObjectPtr<UGameInstance> OriginalGameInstance;
	FPlatformUserId OriginalPlatformUserId = PLATFORMUSERID_NONE;
	int32 OriginalPlatformUserIndex = INDEX_NONE;
	FString SlotName;
	FString LocalPlayerPath;
	FString GameInstancePath;
	FString PresetsPath;
	FString Error;
	TStrongObjectPtr<UGGYGOSquadPresets> Candidate;
};
