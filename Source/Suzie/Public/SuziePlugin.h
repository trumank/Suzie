#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Styling/SlateStyle.h"
#include "Framework/Commands/UICommandList.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSuzie, Log, All);

struct FDynamicClassGenerationContext
{
	TSharedPtr<FJsonObject> GlobalObjectMap;
    TMap<UClass*, FString> ClassesPendingConstruction;
    TArray<UClass*> ClassesPendingFinalization;
};

class FSuziePluginModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    TSharedPtr<FUICommandList> PluginCommands;
    TSharedPtr<FSlateStyleSet> PluginStyle;

    UClass* FindOrCreateUnregisteredClass(FDynamicClassGenerationContext& Context, const FString& ClassPath);
    UClass* FindOrCreateClass(FDynamicClassGenerationContext& Context, const FString& ClassPath);
    UScriptStruct* FindOrCreateScriptStruct(FDynamicClassGenerationContext& Context, const FString& StructPath);
    void FinalizeClass(UClass* Class);

    void ProcessAllJsonClassDefinitions();

    static void ParseObjectPath(const FString& ObjectPath, FString& OutPackageName, FString& OutObjectName);
    static TSet<FString> ParseFlags(const FString& Flags);

    void AddPropertyToStruct(FDynamicClassGenerationContext& Context, UStruct* Struct, const TSharedPtr<FJsonObject>& PropertyJson, EPropertyFlags ExtraPropertyFlags = CPF_None);
    void AddFunctionToClass(FDynamicClassGenerationContext& Context, UClass* Class, const FString& FunctionPath, const TSharedPtr<FJsonObject>& FunctionJson, EFunctionFlags ExtraFunctionFlags = FUNC_None);

    FProperty* BuildProperty(FDynamicClassGenerationContext& Context, FFieldVariant Owner, const TSharedPtr<FJsonObject>& PropertyJson, EPropertyFlags ExtraPropertyFlags = CPF_None);
};
