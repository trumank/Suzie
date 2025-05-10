using UnrealBuildTool;
public class Suzie : ModuleRules
{
	public Suzie(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(new string[] {}); 
		
		PrivateIncludePaths.AddRange(new string[] {});


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"DeveloperSettings",
				"Engine",
				"Blutility",
				"Json",
				"UnrealEd",
				"Projects",
				"Slate",
				"SlateCore",
				"EditorStyle",
				"InputCore",
				"ToolMenus",
				"PropertyEditor"}
			);

		DynamicallyLoadedModuleNames.AddRange(new string[] {});
	}
}
