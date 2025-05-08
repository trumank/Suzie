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
				"Engine",
				"Blutility",
				"Json",
				"UnrealEd",
				"Projects",
			}
			);

		DynamicallyLoadedModuleNames.AddRange(new string[] {});
	}
}
