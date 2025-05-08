#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Styling/SlateStyle.h"
#include "Framework/Commands/UICommandList.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSuzie, Log, All);

class FSuziePluginModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    TSharedPtr<FUICommandList> PluginCommands;
    TSharedPtr<FSlateStyleSet> PluginStyle;
    TArray<UClass*> PendingDynamicClasses;
    TSet<UClass*> PendingConstruction;

    UClass* GetUnregisteredClass(const TSharedPtr<FJsonObject>& Objects, const FString& ClassPath);
    UClass* GetRegisteredClass(const TSharedPtr<FJsonObject>& Objects, const FString& ClassPath);

    void ProcessAllJsonClassDefinitions();

    static void ParseObjectPath(const FString& ObjectPath, FString& OutPackageName, FString& OutObjectName);
    static TSet<FString> ParseFlags(const FString& Flags);

    void AddPropertyToClass(const TSharedPtr<FJsonObject>& Objects, UClass* Class, const TSharedPtr<FJsonObject>& PropertyJson);

    FProperty* BuildProperty(const TSharedPtr<FJsonObject>& Objects, FFieldVariant Owner, const TSharedPtr<FJsonObject>& PropertyJson);

    void FinalizeAllDynamicClasses();
    void FinalizeClass(UClass* Class);
};
