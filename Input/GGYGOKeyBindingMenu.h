/**
 * @file GGYGOKeyBindingMenu.h
 * @brief 键盘改键界面（Slate）：读写 Enhanced Input 用户设置里的玩家键位
 */
#pragma once

#include "Widgets/SCompoundWidget.h"

class SVerticalBox;
class STextBlock;
class UEnhancedInputUserSettings;
struct FInputChord;

/**
 * 改键界面。行来自当前键位档案里已登记的可改键映射（GetPlayerMappingRows），只改第一槽位。
 *
 * 选中新键：已被别的映射占用时拒绝并提示，否则 MapPlayerKey，随后 ApplySettings 让映射重建、
 * AsyncSaveSettings 存档。恢复默认对当前档案执行 ResetKeyProfileToDefault。
 * 界面只持有用户设置的弱引用；关闭由所有者（PlayerController）经 OnCloseRequested 执行。
 */
class GGYGO_API SGGYGOKeyBindingMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGGYGOKeyBindingMenu) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UEnhancedInputUserSettings>, Settings)
		SLATE_EVENT(FSimpleDelegate, OnCloseRequested)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void RebuildRows();
	FInputChord GetCurrentChord(FName MappingName) const;
	void HandleKeySelected(const FInputChord& Chord, FName MappingName);
	FReply HandleResetClicked();
	FReply HandleCloseClicked();
	/** 应用到映射并存档；改键与恢复默认共用。 */
	void ApplyAndSave(UEnhancedInputUserSettings* Settings);

	TWeakObjectPtr<UEnhancedInputUserSettings> WeakSettings;
	FSimpleDelegate OnCloseRequested;
	TSharedPtr<SVerticalBox> RowBox;
	TSharedPtr<STextBlock> StatusText;
};
