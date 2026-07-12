// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class GGYGO : ModuleRules
{
	public GGYGO(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "GameplayAbilities", "GameplayTags", "GameplayTasks" });

		PrivateDependencyModuleNames.AddRange(new string[] { "Json" });

		PublicIncludePaths.AddRange(new string[] {
			Path.Combine(ModuleDirectory, "Public"),
			Path.Combine(ModuleDirectory, "Public/Abilities"),
			Path.Combine(ModuleDirectory, "Public/Attributes"),
			Path.Combine(ModuleDirectory, "Public/Movement"),
			Path.Combine(ModuleDirectory, "Public/StateMachine"),
			Path.Combine(ModuleDirectory, "Public/Combat"),
			Path.Combine(ModuleDirectory, "Public/Camera"),
			Path.Combine(ModuleDirectory, "Public/Core")
});

		// 测试脚手架：让 Private/Tests 下的测试与生成器工具头可被相对解析。
		// Automation 框架（IMPLEMENT_SIMPLE_AUTOMATION_TEST / FAutomationTestBase）与
		// FRandomStream 均位于 Core/Engine，已在上方依赖中，无需额外运行时模块。
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private/Tests"));

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
