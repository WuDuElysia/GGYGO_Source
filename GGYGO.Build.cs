// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GGYGO : ModuleRules
{
	public GGYGO(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			// Lyra 风格组件化基础：UGameFrameworkComponentManager、UPawnComponent、InitState 状态机。
			"ModularGameplay",
			// Experience / GameFeature 插件化数据驱动。
			"GameFeatures",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			// 解耦的 Gameplay 消息广播；模块来自 Plugins/GameplayMessageRouter。
			"GameplayMessageRuntime",
		});

		// 头文件与实现文件按职责子目录并列，模块根是唯一 include 根。
		PublicIncludePaths.Add(ModuleDirectory);

		// 测试脚手架：模块根下的 Tests 与生成器工具头可被相对解析。
		// Automation 框架（IMPLEMENT_SIMPLE_AUTOMATION_TEST / FAutomationTestBase）与
		// FRandomStream 均位于 Core/Engine，已在上方依赖中，无需额外运行时模块。

		// 仅在带编辑器的构建中链接 Automation 控制器模块，避免影响 Shipping/Game 目标。
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("AutomationController");
		}


		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
