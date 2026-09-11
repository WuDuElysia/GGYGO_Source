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
 * 没注册时解析结果为空，装配会回落到默认编队。
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

#include "GGYGOSquadPresets.generated.h"

class APlayerController;
class UGGYGOPawnData;
class ULocalPlayer;
class UObject;

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

	/** 成员角色，按出场顺序。长度不超过 `GGYGO_MAX_SQUAD_SIZE`。 */
	UPROPERTY(BlueprintReadWrite, Category = "GGYGO|Squad")
	TArray<FPrimaryAssetId> Members;

	/** 一个有效成员都没有（全空或长度为 0）。这种编队不能出战。 */
	bool HasAnyMember() const;
};

/**
 * 编队预设的存档。每个本地玩家一份。
 *
 * 派生 `ULocalPlayerSaveGame` 而不是裸 `USaveGame`：它把"槽位名按玩家区分"、
 * 异步保存、以及版本迁移（`GetLatestDataVersion` + `HandlePostLoad`）都处理好了，
 * 分屏时两个玩家各自的编队不会互相覆盖。
 */
UCLASS(BlueprintType)
class GGYGO_API UGGYGOSquadPresets : public ULocalPlayerSaveGame
{
	GENERATED_BODY()

public:
	/** 存档槽位名。 */
	static const FString SaveSlotName;

	/**
	 * 同步读盘并新建一份编队预设对象。
	 *
	 * **每次调用都返回新对象**，所以业务代码不要直连这里 —— 两个调用方各拿一份
	 * 副本时，一边的修改会被另一边的存盘覆盖。走 `UGGYGOLocalPlayer::GetSquadPresets()`
	 * 才有缓存。本函数是那个缓存的填充来源。
	 *
	 * @return 失败返回 nullptr（LocalPlayer 为空时）。
	 */
	static UGGYGOSquadPresets* LoadOrCreateForLocalPlayer(const ULocalPlayer* LocalPlayer);

	/**
	 * 取某个玩家的编队预设，经 `UGGYGOLocalPlayer` 的缓存。
	 *
	 * 服务器上的远程玩家没有本地玩家，返回 nullptr —— 他们的编队存在自己的
	 * 磁盘上，读不到。这是正常路径而不是错误。
	 */
	static UGGYGOSquadPresets* GetForPlayerController(const APlayerController* PlayerController);

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
	 * 替换一套编队的成员。超过 `GGYGO_MAX_SQUAD_SIZE` 的部分被截断。
	 *
	 * @return 是否写入了。越界返回 false。
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
	 * 把出战编队解析成可直接喂给 `SetRoster` 的 PawnData 列表。
	 *
	 * **同步加载**这一套里的角色资产。放在玩家进入关卡时执行一次，
	 * 几份资产的同步加载换来的是"装配时名单已就绪"，
	 * 异步则要让装配等回调，把 GameMode 的生成流程拆成两段。
	 *
	 * 解析不出的 Id（角色被删、未注册 PrimaryAssetType）被跳过。
	 *
	 * @return 解析出的角色数。为 0 时调用方应回落到默认编队。
	 */
	int32 ResolveActivePresetRoster(TArray<UGGYGOPawnData*>& OutRoster) const;

	/** 解析指定编队。面板预览用。 */
	int32 ResolvePresetRoster(int32 PresetIndex, TArray<UGGYGOPawnData*>& OutRoster) const;

	//~ULocalPlayerSaveGame interface
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
};
