#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Styling/SlateStyle.h"
#include "Framework/Commands/UICommandList.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSuzie, Log, All);

struct FDynamicClassGenerationContext
{
    // Key is the path of the object
	TSharedPtr<FJsonObject> GlobalObjectMap;
    // Value is the class path of the class
    TMap<UClass*, FString> ClassesPendingConstruction;
    // Value is the object path of the class default object
    TMap<UClass*, FString> ClassesPendingFinalization;
};

struct FDynamicObjectConstructionData
{
    FName ObjectName;
    UClass* ObjectClass{};
    EObjectFlags ObjectFlags{};
};

struct FDynamicClassConstructionData
{
    // Note that this will also contain all subobjects defined in parent classes
    TArray<FDynamicObjectConstructionData> DefaultSubobjects;
};

class FSuziePluginModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    TSharedPtr<FUICommandList> PluginCommands;
    TSharedPtr<FSlateStyleSet> PluginStyle;

    UPackage* FindOrCreatePackage(FDynamicClassGenerationContext& Context, const FString& PackageName);
    UClass* FindOrCreateUnregisteredClass(FDynamicClassGenerationContext& Context, const FString& ClassPath);
    UClass* FindOrCreateClass(FDynamicClassGenerationContext& Context, const FString& ClassPath);
    UScriptStruct* FindOrCreateScriptStruct(FDynamicClassGenerationContext& Context, const FString& StructPath);
    UEnum* FindOrCreateEnum(FDynamicClassGenerationContext& Context, const FString& EnumPath);
    UFunction* FindOrCreateFunction(FDynamicClassGenerationContext& Context, const FString& FunctionPath);

    static void PolymorphicClassConstructorInvocationHelper(const FObjectInitializer& ObjectInitializer);
    static void PolymorphicClassConstructorPostPropertyInitCallback(UObject* ConstructedObject, const TMap<UObject*, UObject*>& TemplateToSubobjectMap);

    static bool ParseObjectConstructionData(const FDynamicClassGenerationContext& Context, const FString& ObjectPath, FDynamicObjectConstructionData& ObjectConstructionData);
    void DeserializeStructProperties(const UStruct* Struct, void* StructData, const TSharedPtr<FJsonObject>& PropertyValues);
    static void DeserializeEnumValue(const FNumericProperty* UnderlyingProperty, void* PropertyValuePtr, const UEnum* Enum, const TSharedPtr<FJsonValue>& JsonPropertyValue);
    void DeserializePropertyValue(const FProperty* Property, void* PropertyValuePtr, const TSharedPtr<FJsonValue>& JsonPropertyValue);
    void FinalizeClass(FDynamicClassGenerationContext& Context, UClass* Class);
    
    void ProcessAllJsonClassDefinitions();

    static void ParseObjectPath(const FString& ObjectPath, FString& OutOuterObjectPath, FString& OutObjectName);
    static TSet<FString> ParseFlags(const FString& Flags);

    void AddPropertyToStruct(FDynamicClassGenerationContext& Context, UStruct* Struct, const TSharedPtr<FJsonObject>& PropertyJson, EPropertyFlags ExtraPropertyFlags = CPF_None);
    void AddFunctionToClass(FDynamicClassGenerationContext& Context, UClass* Class, const FString& FunctionPath, EFunctionFlags ExtraFunctionFlags = FUNC_None);

    FProperty* BuildProperty(FDynamicClassGenerationContext& Context, FFieldVariant Owner, const TSharedPtr<FJsonObject>& PropertyJson, EPropertyFlags ExtraPropertyFlags = CPF_None);
};
