#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Framework/Commands/UICommandList.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSuzie, Log, All);

class USuzieSettings;
class SDockTab;

/**
 * Main module class for the Suzie plugin
 */
class FSuziePluginModule : public IModuleInterface
{
public:
    /** IModuleInterface implementation */
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    
    /** Processes all JSON class definitions */
    void ProcessAllJsonClassDefinitions();

    /** Helper method to get the config file path */
    FString GetConfigFilePath() const;

private:
    TSharedPtr<FUICommandList> PluginCommands;
    TSharedPtr<class FSuzieStyle> Style;
    TArray<UClass*> PendingDynamicClasses;
    TSet<UClass*> PendingConstruction;
    USuzieSettings* Settings = nullptr;

    bool CreateClassesFromJson(const TSharedPtr<FJsonObject>& Objects);
    /** Helper method to process a single JSON file */
    bool ProcessJsonFile(const FString& JsonFilePath);
    UClass* GetUnregisteredClass(const TSharedPtr<FJsonObject>& Objects, const FString& ClassPath);
    UClass* GetRegisteredClass(const TSharedPtr<FJsonObject>& Objects, const FString& ClassPath);
    
    UScriptStruct* GetStruct(const TSharedPtr<FJsonObject>& Objects, const FString& StructPath);

    static void ParseObjectPath(const FString& ObjectPath, FString& OutPackageName, FString& OutObjectName);
    static TSet<FString> ParseFlags(const FString& Flags);

    void AddPropertyToClass(const TSharedPtr<FJsonObject>& Objects, UClass* Class, const TSharedPtr<FJsonObject>& PropertyJson);
    void AddFunctionToClass(const TSharedPtr<FJsonObject>& Objects, UClass* Class, FString FunctionPath, const TSharedPtr<FJsonObject>& FunctionJson);

    FProperty* BuildProperty(const TSharedPtr<FJsonObject>& Objects, FFieldVariant Owner, const TSharedPtr<FJsonObject>& PropertyJson);

    void FinalizeAllDynamicClasses();
    void FinalizeClass(UClass* Class);
};
