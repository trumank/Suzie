#include "Settings/SuzieSettings.h"

USuzieSettings::USuzieSettings()
{
    // Default settings
    JsonClassesDirectory.Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("DynamicClasses"));
    bLoadAllFiles = false;
    
    // Ensure the plugin config directory exists
    FString PluginConfigDir = FPaths::ProjectPluginsDir() / TEXT("Suzie/Config/");
    if (!FPaths::DirectoryExists(PluginConfigDir))
    {
        FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*PluginConfigDir);
    }
    
    // Make sure the JSON directory exists
    if (!JsonClassesDirectory.Path.IsEmpty() && !FPaths::DirectoryExists(JsonClassesDirectory.Path))
    {
        FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*JsonClassesDirectory.Path);
    }
}

FName USuzieSettings::GetCategoryName() const
{
    return FName("Plugins");
}

FText USuzieSettings::GetSectionText() const
{
    return FText::FromString("Suzie");
}

#if WITH_EDITOR
FText USuzieSettings::GetSectionDescription() const
{
    return FText::FromString("Configure the Suzie plugin for injecting reflection data from JSON files.");
}
#endif
