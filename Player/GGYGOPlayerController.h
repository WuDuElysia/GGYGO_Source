/**
 * @file GGYGOPlayerController.h
 * @brief 玩家控制器
 *
 * 包围原生输入系统创建，并在正确的时机驱动 ASC 消费本帧的输入缓存。
 * 创建执行阶段归本Controller；输入出生资格和物理来源分别归Input资源与生产者。
 *
 * ## 为什么必须由 Controller 驱动，且必须在 PostProcessInput 里
 * ASC 把输入分成 pressed / held / released 三个缓存，需要有人每帧统一消费。
 * 时机很关键 —— 必须在**所有** Enhanced Input 事件都派发完之后：
 * 若在派发中途消费，同一帧内后到的按下事件就会被推迟到下一帧，
 * 表现为偶发的一帧输入延迟。
 *
 * `PostProcessInput` 正是引擎保证的"本帧输入全部处理完"的点，
 * 而它只存在于 PlayerController 上，组件的 Tick 拿不到这个时机。
 */
#pragma once

#include "GameFramework/PlayerController.h"

#include "GGYGOPlayerController.generated.h"

class APawn;
class SWidget;
class UGameViewportClient;
class UEnhancedInputLocalPlayerSubsystem;
class UEnhancedPlayerInput;
class UGGYGOAbilitySystemComponent;
class UGGYGOKeySettingsPage;
class UGGYGOLocalPlayer;
class UInputAction;
class UInputComponent;
class UInputMappingContext;
class ULocalPlayer;
class UObject;

UCLASS(Config = Game, meta = (ShortTooltip = "GGYGO 玩家控制器"))
class GGYGO_API AGGYGOPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AGGYGOPlayerController(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** Native creation only; rejection lasts for this Actor and never grants input qualification. */
	virtual void InitInputSystem() override;
	virtual void Destroyed() override;

	/** 取当前操控角色的 ASC。未附身或角色没有 ASC 时返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|PlayerController")
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponent() const;

	// 下面几个是编队的调试入口，在编成面板做出来之前也是唯一的编队操作方式。
	//
	// 声明不能用 `#if !UE_BUILD_SHIPPING` 包起来：UHT 不接受预处理块里的
	// `UFUNCTION`（只放行 `WITH_EDITORONLY_DATA`）。所以条件编译只包在
	// 实现里，Shipping 下这几个函数存在但是空的。

	/**
	 * 打印本地玩家保存的编队与出战选择。
	 *
	 * 控制台：`GGYGODumpSquadPresets`
	 */
	UFUNCTION(Exec)
	void GGYGODumpSquadPresets();

	/**
	 * 往指定编队的指定位置放一个角色，编队与位置不存在则补齐，改完立即存盘。
	 *
	 * 控制台：`GGYGOSetSquadMember 0 0 DA_Pawn_Pyrios`
	 *
	 * @param PawnDataName 角色配置资产名（`GGYGOPawnData` 的 PrimaryAssetId 名字部分）。
	 *                     传空字符串表示把该位置清空。
	 */
	UFUNCTION(Exec)
	void GGYGOSetSquadMember(int32 PresetIndex, int32 MemberIndex, const FString& PawnDataName);

	/**
	 * 选择出战编队，改完立即存盘。下一次进关卡生效 —— 局内不会换队。
	 *
	 * 控制台：`GGYGOSetActiveSquadPreset 0`
	 */
	UFUNCTION(Exec)
	void GGYGOSetActiveSquadPreset(int32 PresetIndex);

	/**
	 * 删除全部编队并存盘，用于回到"未编成"状态验证默认编队的回落。
	 *
	 * 控制台：`GGYGOClearSquadPresets`
	 */
	UFUNCTION(Exec)
	void GGYGOClearSquadPresets();

	/**
	 * 按序号请求切换出战角色，经编队组件的 Server RPC 由服务器校验并执行；监听服务器与客户端都可用。
	 * 调试入口；正式按键是下面的上一个／下一个。
	 *
	 * 控制台：`GGYGOSwitchToSlot 1`
	 */
	UFUNCTION(Exec)
	void GGYGOSwitchToSlot(int32 SlotIndex);

	/**
	 * 打开本地玩家的键盘改键界面：列出已登记 IMC 中可改键的映射，改动立即生效并存档
	 * （Enhanced Input 用户设置）。打开期间输入只给界面。已打开时不重复创建。
	 * 使用生产 Controller 蓝图配置的共享页面，控制台 `GGYGOOpenKeySettings` 也走此入口。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Input")
	void OpenKeySettings();

	/** 关闭改键界面并恢复游戏输入；未打开时不做任何事。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Input")
	void CloseKeySettings();

	/** 控制台：`GGYGOOpenKeySettings`。 */
	UFUNCTION(Exec)
	void GGYGOOpenKeySettings();

protected:
	//~APlayerController interface
	virtual void SetupInputComponent() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void PostProcessInput(const float DeltaTime, const bool bGamePaused) override;
	//~End of APlayerController interface

	/**
	 * 换人输入属于 Controller：它跨 Pawn 存在，不随 Hero 输入会话在换人中途退役。
	 * 默认键位见所配 IMC（IMC_Squad）；两个 InputAction 带 PlayerMappableKeySettings，可在改键界面修改。
	 * 三项都不配表示本 Controller 不提供换人键；只配一部分是配置错误，整组不安装。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "GGYGO|Input|Squad")
	TObjectPtr<const UInputMappingContext> SquadSwitchInputMapping;

	UPROPERTY(EditDefaultsOnly, Category = "GGYGO|Input|Squad")
	TObjectPtr<const UInputAction> SwitchToPreviousMemberAction;

	UPROPERTY(EditDefaultsOnly, Category = "GGYGO|Input|Squad")
	TObjectPtr<const UInputAction> SwitchToNextMemberAction;

	UPROPERTY(EditDefaultsOnly, Category = "GGYGO|Input|Squad")
	int32 SquadSwitchInputMappingPriority = 0;

	/**
	 * 登记到 Enhanced Input 用户设置的 IMC：其中设置了 PlayerMappableKeySettings 的映射出现在改键界面，
	 * 存档的改键在映射重建时生效。只登记，不激活；激活仍由各自拥有者（Hero、上面的换人 IMC）负责。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "GGYGO|Input")
	TArray<TObjectPtr<const UInputMappingContext>> PlayerMappableInputMappings;

	/** 由生产 Controller 蓝图选择共享改键页。 */
	UPROPERTY(EditDefaultsOnly, Category = "GGYGO|Input")
	TSubclassOf<UGGYGOKeySettingsPage> KeySettingsPageClass;

private:
	void ReleaseKeySettingsPage(bool bReturnToGame);
	void HandleKeySettingsInputRemoving(UEnhancedPlayerInput* OriginalInput, TWeakObjectPtr<UGGYGOKeySettingsPage> OriginalPage);
	/** 本地 Controller 的 InputComponent 建好后绑定换人键并注册 IMC；同一 InputComponent 只安装一次。 */
	void InstallSquadSwitchInput();
	/** 在原 LocalPlayer 退役或 Controller EndPlay 时归还本 Controller 的 IMC。 */
	void ReleaseSquadSwitchMapping();
	void HandleInputMappingsRemoving(UEnhancedPlayerInput* OriginalInput);
	/** 把 PlayerMappableInputMappings 登记到本地玩家的用户设置；设置未启用时诊断一次，按默认键位运行。 */
	void RegisterPlayerMappableInput();
	void Input_SwitchToPreviousMember();
	void Input_SwitchToNextMember();
	/** 发给本玩家编队的服务器请求；目标由服务器选择。 */
	void RequestAdjacentSquadMember(int32 Direction);

	/** 已绑定换人键的 InputComponent；InitInputSystem 可重复进入 SetupInputComponent。 */
	TWeakObjectPtr<UInputComponent> SquadSwitchBoundComponent;
	/** AddMappingContext 对原 PlayerInput 的一份注册，随原 LocalPlayer 或本 Controller 退役。 */
	struct FSquadSwitchMappingRegistration
	{
		TWeakObjectPtr<UGGYGOLocalPlayer> LocalPlayer;
		TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> Subsystem;
		TWeakObjectPtr<UEnhancedPlayerInput> PlayerInput;
		TWeakObjectPtr<const UInputMappingContext> Mapping;
		FDelegateHandle RemovalHandle;
	};
	FSquadSwitchMappingRegistration SquadSwitchMappingRegistration;
	bool bEndingSquadSwitchInput = false;

	/** 原页面和它在原视口中的 Slate 根，只由 Controller 归还。 */
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOKeySettingsPage> KeySettingsPage;
	TSharedPtr<SWidget> KeySettingsRoot;
	TWeakObjectPtr<UGGYGOLocalPlayer> KeySettingsPlayer;
	TWeakObjectPtr<UGameViewportClient> KeySettingsViewport;
	FDelegateHandle KeySettingsRemovalHandle;
	bool bOpeningKeySettings = false;
	bool bReportedUserSettingsUnavailable = false;

	enum class ENativeInputInitializationStage : uint8 { Idle, Running, Rejected };
	ENativeInputInitializationStage NativeInputInitializationStage = ENativeInputInitializationStage::Idle;
	/** Diagnostic suppression only; nested calls never alter the outer execution stage. */
	bool bReportedNativeInputInitializationBusy = false;
};
