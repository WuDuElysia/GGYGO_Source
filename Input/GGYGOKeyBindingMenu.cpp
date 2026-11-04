/**
 * @file GGYGOKeyBindingMenu.cpp
 * @brief 键盘改键界面实现
 */
#include "Input/GGYGOKeyBindingMenu.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Framework/Commands/InputChord.h"
#include "GameplayTagContainer.h"
#include "Styling/CoreStyle.h"
#include "UserSettings/EnhancedInputUserSettings.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SInputKeySelector.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GGYGOKeyBindingMenu"

namespace
{
	const FPlayerKeyMapping* FindFirstSlotMapping(const FKeyMappingRow& Row)
	{
		for (const FPlayerKeyMapping& Mapping : Row.Mappings)
		{
			if (Mapping.GetSlot() == EPlayerMappableKeySlot::First) { return &Mapping; }
		}
		return nullptr;
	}
}

void SGGYGOKeyBindingMenu::Construct(const FArguments& InArgs)
{
	WeakSettings = InArgs._Settings;
	OnCloseRequested = InArgs._OnCloseRequested;

	ChildSlot
	.HAlign(HAlign_Center)
	.VAlign(VAlign_Center)
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
		.BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.03f, 0.9f))
		.Padding(24.0f)
		[
			SNew(SBox)
			.WidthOverride(560.0f)
			.MaxDesiredHeight(640.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Title", "按键设置"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 20))
				]
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(RowBox, SVerticalBox)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SAssignNew(StatusText, STextBlock)
					.Text(LOCTEXT("Hint", "点击按键后按下新键；Esc 取消选择。"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("Reset", "恢复默认"))
						.OnClicked(this, &SGGYGOKeyBindingMenu::HandleResetClicked)
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("Close", "关闭"))
						.OnClicked(this, &SGGYGOKeyBindingMenu::HandleCloseClicked)
					]
				]
			]
		]
	];
	RebuildRows();
}

void SGGYGOKeyBindingMenu::RebuildRows()
{
	RowBox->ClearChildren();
	const UEnhancedInputUserSettings* Settings = WeakSettings.Get();
	const UEnhancedPlayerMappableKeyProfile* Profile = Settings ? Settings->GetCurrentKeyProfile() : nullptr;
	if (!Profile)
	{
		StatusText->SetText(LOCTEXT("NoProfile", "没有可用的键位档案，无法改键。"));
		return;
	}

	struct FRowView { FName MappingName; FText Category; FText DisplayName; };
	TArray<FRowView> Views;
	for (const TPair<FName, FKeyMappingRow>& Pair : Profile->GetPlayerMappingRows())
	{
		if (const FPlayerKeyMapping* Mapping = FindFirstSlotMapping(Pair.Value))
		{
			Views.Add({ Pair.Key, Mapping->GetDisplayCategory(), Mapping->GetDisplayName() });
		}
	}
	Views.Sort([](const FRowView& A, const FRowView& B)
	{
		const int32 ByCategory = A.Category.CompareTo(B.Category);
		return ByCategory != 0 ? ByCategory < 0 : A.MappingName.LexicalLess(B.MappingName);
	});
	for (const FRowView& View : Views)
	{
		const FName MappingName = View.MappingName;
		RowBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(View.Category.IsEmpty() ? View.DisplayName
					: FText::Format(LOCTEXT("RowLabel", "{0} · {1}"), View.Category, View.DisplayName))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 13))
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(180.0f)
				[
					SNew(SInputKeySelector)
					.SelectedKey(this, &SGGYGOKeyBindingMenu::GetCurrentChord, MappingName)
					.OnKeySelected(this, &SGGYGOKeyBindingMenu::HandleKeySelected, MappingName)
					.AllowModifierKeys(false)
					.AllowGamepadKeys(false)
					.EscapeCancelsSelection(true)
				]
			]
		];
	}
}

FInputChord SGGYGOKeyBindingMenu::GetCurrentChord(FName MappingName) const
{
	const UEnhancedInputUserSettings* Settings = WeakSettings.Get();
	const FPlayerKeyMapping* Mapping = Settings
		? Settings->FindCurrentMappingForSlot(MappingName, EPlayerMappableKeySlot::First) : nullptr;
	return Mapping ? FInputChord(Mapping->GetCurrentKey()) : FInputChord();
}

void SGGYGOKeyBindingMenu::HandleKeySelected(const FInputChord& Chord, FName MappingName)
{
	UEnhancedInputUserSettings* Settings = WeakSettings.Get();
	UEnhancedPlayerMappableKeyProfile* Profile = Settings ? Settings->GetCurrentKeyProfile() : nullptr;
	if (!Profile || !Chord.Key.IsValid()) { return; }

	// 一个键只服务一个映射：被占用时不改，避免一键同时触发两个动作。
	TArray<FName> Users;
	Profile->GetMappingNamesForKey(Chord.Key, Users);
	Users.Remove(MappingName);
	if (!Users.IsEmpty())
	{
		const FPlayerKeyMapping* Other = Settings->FindCurrentMappingForSlot(Users[0], EPlayerMappableKeySlot::First);
		StatusText->SetText(FText::Format(LOCTEXT("KeyInUse", "{0} 已被「{1}」使用，未修改。"),
			Chord.Key.GetDisplayName(), Other ? Other->GetDisplayName() : FText::FromName(Users[0])));
		return;
	}

	FMapPlayerKeyArgs Args;
	Args.MappingName = MappingName;
	Args.Slot = EPlayerMappableKeySlot::First;
	Args.NewKey = Chord.Key;
	FGameplayTagContainer Failure;
	Settings->MapPlayerKey(Args, Failure);
	if (!Failure.IsEmpty())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("[Input/KeyBinding] Mapping=%s Key=%s rejected: %s"),
			*MappingName.ToString(), *Chord.Key.ToString(), *Failure.ToStringSimple());
		StatusText->SetText(LOCTEXT("MapFailed", "改键失败，详见日志。"));
		return;
	}
	ApplyAndSave(Settings);
	StatusText->SetText(FText::Format(LOCTEXT("Mapped", "已改为 {0}。"), Chord.Key.GetDisplayName()));
}

FReply SGGYGOKeyBindingMenu::HandleResetClicked()
{
	if (UEnhancedInputUserSettings* Settings = WeakSettings.Get())
	{
		FGameplayTagContainer Failure;
		Settings->ResetKeyProfileToDefault(Settings->GetCurrentKeyProfileIdentifier(), Failure);
		if (!Failure.IsEmpty())
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("[Input/KeyBinding] Reset rejected: %s"), *Failure.ToStringSimple());
			StatusText->SetText(LOCTEXT("ResetFailed", "恢复默认失败，详见日志。"));
			return FReply::Handled();
		}
		ApplyAndSave(Settings);
		StatusText->SetText(LOCTEXT("ResetDone", "已恢复默认键位。"));
	}
	return FReply::Handled();
}

FReply SGGYGOKeyBindingMenu::HandleCloseClicked()
{
	OnCloseRequested.ExecuteIfBound();
	return FReply::Handled();
}

FReply SGGYGOKeyBindingMenu::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	// 选键中的 Esc 由 SInputKeySelector 自己处理；这里只在界面本身有焦点时关闭。
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		OnCloseRequested.ExecuteIfBound();
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

void SGGYGOKeyBindingMenu::ApplyAndSave(UEnhancedInputUserSettings* Settings)
{
	Settings->ApplySettings();
	Settings->AsyncSaveSettings();
}

#undef LOCTEXT_NAMESPACE
