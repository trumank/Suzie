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
    // Lookup of dynamic classes that are currently being constructed by FindOrCreateUnregisteredClass
    // Needed to handle edge case of re-entry when a parent class declares a function that takes a child class as an argument
    // We do not support this case fully, but we need to track it to avoid creating the same class multiple times
    TSet<FString> UnregisteredDynamicClassConstructionStack;
};

struct FDynamicObjectConstructionData
{
    FName ObjectName;
    UClass* ObjectClass{};
    EObjectFlags ObjectFlags{};
};

struct FNestedDefaultSubobjectOverrideData
{
    TArray<FName> SubobjectPath;
    UClass* OverridenClass{};
};

struct FDynamicClassConstructionData
{
    // List of properties (not including super class properties) that must be constructed with InitializeValue call
    TArray<const FProperty*> PropertiesToConstruct;
    // Names of default subobjects that our native parent class defines but that we do not want to be created
    TArray<FName> SuppressedDefaultSubobjects;
    // Note that this will also contain all subobjects defined in parent classes
    TArray<FDynamicObjectConstructionData> DefaultSubobjects;
    // Overrides for nested default subobjects. Note that top level subobjects will not be included here
    TArray<FNestedDefaultSubobjectOverrideData> DefaultSubobjectOverrides;
    // Archetype to use for constructing the object when no archetype has been provided or the provided archetype was a CDO
    UObject* DefaultObjectArchetype{};
};

struct FDynamicClassConstructionIntermediates
{
    UObject* ConstructedObject{};
    const FDynamicClassConstructionData* ConstructionData{};
    UObject* ArchetypeObject{};
    TMap<UObject*, UObject*> TemplateToSubobjectMap;
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
    static UClass* GetPlaceholderNonNativePropertyOwnerClass();
    UClass* FindOrCreateUnregisteredClass(FDynamicClassGenerationContext& Context, const FString& ClassPath);
    UClass* FindOrCreateClass(FDynamicClassGenerationContext& Context, const FString& ClassPath);
    UScriptStruct* FindOrCreateScriptStruct(FDynamicClassGenerationContext& Context, const FString& StructPath);
    UEnum* FindOrCreateEnum(FDynamicClassGenerationContext& Context, const FString& EnumPath);
    UFunction* FindOrCreateFunction(FDynamicClassGenerationContext& Context, const FString& FunctionPath);

    static UClass* GetNativeParentClassForDynamicClass(const UClass* InDynamicClass);
    static UClass* GetDynamicParentClassForBlueprintClass(UClass* InBlueprintClass);
    static void PolymorphicClassConstructorInvocationHelper(const FObjectInitializer& ObjectInitializer);
    static void ExecutePolymorphicClassConstructorFrameForDynamicClass(const FObjectInitializer& ObjectInitializer, const UClass* DynamicClass);

    static bool ParseObjectConstructionData(const FDynamicClassGenerationContext& Context, const FString& ObjectPath, FDynamicObjectConstructionData& ObjectConstructionData);
    void DeserializeStructProperties(const UStruct* Struct, void* StructData, const TSharedPtr<FJsonObject>& PropertyValues);
    static void DeserializeEnumValue(const FNumericProperty* UnderlyingProperty, void* PropertyValuePtr, const UEnum* Enum, const TSharedPtr<FJsonValue>& JsonPropertyValue);
    void DeserializePropertyValue(const FProperty* Property, void* PropertyValuePtr, const TSharedPtr<FJsonValue>& JsonPropertyValue);
    void CollectNestedDefaultSubobjectTypeOverrides(FDynamicClassGenerationContext& Context, TArray<FName> SubobjectNameStack, const FString& SubobjectPath, TArray<FNestedDefaultSubobjectOverrideData>& OutSubobjectOverrideData);
    void DeserializeObjectAndSubobjectPropertyValuesRecursive(const FDynamicClassGenerationContext& Context, UObject* Object, const TSharedPtr<FJsonObject>& ObjectDefinition);
    void FinalizeClass(FDynamicClassGenerationContext& Context, UClass* Class);

    void CreateDynamicClassesForJsonObject(const TSharedPtr<FJsonObject>& RootObject);
    void ProcessAllJsonClassDefinitions();

    static void ParseObjectPath(const FString& ObjectPath, FString& OutOuterObjectPath, FString& OutObjectName);
    static TSet<FString> ParseFlags(const FString& Flags);

    FProperty* AddPropertyToStruct(FDynamicClassGenerationContext& Context, UStruct* Struct, const TSharedPtr<FJsonObject>& PropertyJson, EPropertyFlags ExtraPropertyFlags = CPF_None);
    void AddFunctionToClass(FDynamicClassGenerationContext& Context, UClass* Class, const FString& FunctionPath, EFunctionFlags ExtraFunctionFlags = FUNC_None);

    FProperty* BuildProperty(FDynamicClassGenerationContext& Context, FFieldVariant Owner, const TSharedPtr<FJsonObject>& PropertyJson, EPropertyFlags ExtraPropertyFlags = CPF_None);
};
