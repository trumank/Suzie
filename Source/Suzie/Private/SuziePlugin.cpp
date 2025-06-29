#include "SuziePlugin.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Blueprint.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "HAL/PlatformFileManager.h"
#include "Editor/EditorEngine.h"
#include "Framework/Commands/UICommandList.h"
#include "Engine/EngineTypes.h"
#include "PropertyEditorModule.h"
#include "SuzieDecompressionHelper.h"
#include "UObject/PropertyOptional.h"
#include "Widgets/Docking/SDockTab.h"
#include "UObject/UObjectAllocator.h"

DEFINE_LOG_CATEGORY(LogSuzie);

#define LOCTEXT_NAMESPACE "FSuziePluginModule"

void FSuziePluginModule::StartupModule()
{
    UE_LOG(LogSuzie, Display, TEXT("Suzie plugin starting"));

    ProcessAllJsonClassDefinitions();
}

void FSuziePluginModule::ShutdownModule()
{
    UE_LOG(LogSuzie, Display, TEXT("Suzie plugin shutting down"));
}

void FSuziePluginModule::ProcessAllJsonClassDefinitions()
{
    
    // Define where we expect JSON class definitions to be
    const FString JsonClassesPath = FPaths::ProjectContentDir() / TEXT("DynamicClasses");
    
    // Check if directory exists
    if (!FPlatformFileManager::Get().GetPlatformFile().DirectoryExists(*JsonClassesPath))
    {
        UE_LOG(LogSuzie, Warning, TEXT("JSON Classes directory not found: %s"), *JsonClassesPath);
        return;
    }
    
    // Find all JSON files and compressed JSON files
    TArray<FString> JsonFileNames;
    IFileManager::Get().FindFiles(JsonFileNames, *JsonClassesPath, TEXT("*.json"));

    TArray<FString> CompressedJsonFileNames;
    IFileManager::Get().FindFiles(CompressedJsonFileNames, *JsonClassesPath, TEXT("*.json.gz"));
    
    UE_LOG(LogSuzie, Display, TEXT("Found %d JSON class definition files"), JsonFileNames.Num() + CompressedJsonFileNames.Num());

    // This can potentially take some time so show a progress task
    const int32 TotalAmountOfWork = JsonFileNames.Num() + CompressedJsonFileNames.Num();
    FScopedSlowTask GenerateDynamicClassesTask(TotalAmountOfWork, LOCTEXT("GeneratingDynamicClasses", "Suzie: Generating Dynamic Classes"));
    GenerateDynamicClassesTask.Visibility = ESlowTaskVisibility::Important;
    GenerateDynamicClassesTask.ForceRefresh();
    
    // Process each JSON file
    for (const FString& JsonFileName : JsonFileNames)
    {
        GenerateDynamicClassesTask.EnterProgressFrame(1, FText::Format(LOCTEXT("ProcessingJsonFile", "Generating classes for file {0}"), FText::AsCultureInvariant(JsonFileName)));
        GenerateDynamicClassesTask.ForceRefresh();
        UE_LOG(LogSuzie, Display, TEXT("Processing JSON class definition: %s"), *JsonFileName);
    
        // Read the JSON file
        FString JsonContent;
        if (!FFileHelper::LoadFileToString(JsonContent, *(JsonClassesPath / JsonFileName)))
        {
            UE_LOG(LogSuzie, Error, TEXT("Failed to read JSON file: %s"), *JsonFileName);
            return;
        }
    
        // Parse the JSON
        TSharedPtr<FJsonObject> JsonObject;
        TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(JsonContent);
        if (!FJsonSerializer::Deserialize(JsonReader, JsonObject) || !JsonObject.IsValid())
        {
            UE_LOG(LogSuzie, Error, TEXT("Failed to parse JSON in file: %s"), *JsonFileName);
            continue;
        }
        CreateDynamicClassesForJsonObject(JsonObject);
    }
    
    // Process each compressed JSON file
    for (const FString& CompressedJsonFileName : CompressedJsonFileNames)
    {
        GenerateDynamicClassesTask.EnterProgressFrame(1, FText::Format(LOCTEXT("ProcessingJsonFile", "Generating classes for file {0}"), FText::AsCultureInvariant(CompressedJsonFileName)));
        GenerateDynamicClassesTask.ForceRefresh();
        UE_LOG(LogSuzie, Display, TEXT("Processing compressed JSON class definition: %s"), *CompressedJsonFileName);

        // Read binary file contents
        TArray<uint8> CompressedFileContents;
        if (!FFileHelper::LoadFileToArray(CompressedFileContents, *(JsonClassesPath / CompressedJsonFileName)))
        {
            UE_LOG(LogSuzie, Error, TEXT("Failed to read compressed JSON file: %s"), *CompressedJsonFileName);
            return;
        }

        // Attempt to decompress the file as Gzip archive
        TArray<uint8> DecompressedFileContents;
        if (!FSuzieDecompressionHelper::DecompressMemoryGzip(CompressedFileContents, DecompressedFileContents))
        {
            UE_LOG(LogSuzie, Error, TEXT("Failed to decompress compressed JSON file as valid GZIP: %s"), *CompressedJsonFileName);
            return;
        }

        // Parse the binary stream into the string. UE will attempt to guess the encoding for us
        FString JsonContent;
        FFileHelper::BufferToString(JsonContent, DecompressedFileContents.GetData(), DecompressedFileContents.Num());
        
        // Parse the JSON
        TSharedPtr<FJsonObject> JsonObject;
        TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(JsonContent);
        if (!FJsonSerializer::Deserialize(JsonReader, JsonObject) || !JsonObject.IsValid())
        {
            UE_LOG(LogSuzie, Error, TEXT("Failed to parse compressed JSON in file: %s"), *CompressedJsonFileName);
            continue;
        }
        CreateDynamicClassesForJsonObject(JsonObject);
    }
}

void FSuziePluginModule::CreateDynamicClassesForJsonObject(const TSharedPtr<FJsonObject>& RootObject)
{
    const TSharedPtr<FJsonObject>* Objects;
    if (!RootObject->TryGetObjectField(TEXT("objects"), Objects))
    {
        UE_LOG(LogSuzie, Error, TEXT("Missing 'objects' map"));
        return;
    }

    // Create class generation context
    FDynamicClassGenerationContext ClassGenerationContext;
    ClassGenerationContext.GlobalObjectMap = *Objects;

    // Create classes, script structs and global delegate functions
    for (auto It = (*Objects)->Values.CreateConstIterator(); It; ++It)
    {
        FString ObjectPath = It.Key();
        FString Type = It.Value()->AsObject()->GetStringField(TEXT("type"));
        if (Type == TEXT("Class"))
        {
            UE_LOG(LogSuzie, Verbose, TEXT("Creating class %s"), *ObjectPath);
            FindOrCreateClass(ClassGenerationContext, ObjectPath);
        }
        else if (Type == TEXT("ScriptStruct"))
        {
            UE_LOG(LogSuzie, Verbose, TEXT("Creating struct %s"), *ObjectPath);
            FindOrCreateScriptStruct(ClassGenerationContext, ObjectPath);
        }
        else if (Type == TEXT("Enum"))
        {
            UE_LOG(LogSuzie, Verbose, TEXT("Creating enum %s"), *ObjectPath);
            FindOrCreateEnum(ClassGenerationContext, ObjectPath);
        }
        else if (Type == TEXT("Function"))
        {
            UE_LOG(LogSuzie, VeryVerbose, TEXT("Creating function %s"), *ObjectPath);
            FindOrCreateFunction(ClassGenerationContext, ObjectPath);
        }
    }

    // Construct classes that have been created but have not been constructed yet due to nobody referencing them
    while (!ClassGenerationContext.ClassesPendingConstruction.IsEmpty())
    {
        TArray<FString> ClassPathsPendingConstruction;
        ClassGenerationContext.ClassesPendingConstruction.GenerateValueArray(ClassPathsPendingConstruction);
        for (const FString& ClassPath : ClassPathsPendingConstruction)
        {
            FindOrCreateClass(ClassGenerationContext, ClassPath);
        }
    }

    // Finalize all classes that we have created now. This includes assembling reference streams, creating default subobjects and populating them with data
    TArray<UClass*> ClassesPendingFinalization;
    ClassGenerationContext.ClassesPendingFinalization.GenerateKeyArray(ClassesPendingFinalization);
    for (UClass* ClassPendingFinalization : ClassesPendingFinalization)
    {
        FinalizeClass(ClassGenerationContext, ClassPendingFinalization);
    }
}

UPackage* FSuziePluginModule::FindOrCreatePackage(FDynamicClassGenerationContext& Context, const FString& PackageName)
{
    int32 UnusedCharacterIndex;
    checkf(!PackageName.FindChar('.', UnusedCharacterIndex) && !PackageName.FindChar(':', UnusedCharacterIndex),
        TEXT("Invalid package name: %s"), *PackageName);
    
    UPackage* Package = CreatePackage(*PackageName);
    Package->SetPackageFlags(PKG_CompiledIn);
    return Package;
}

UClass* FSuziePluginModule::GetPlaceholderNonNativePropertyOwnerClass()
{
    static UBlueprintGeneratedClass* PlaceholderNonNativeOwnerClass = nullptr;
    if (PlaceholderNonNativeOwnerClass == nullptr)
    {
        PlaceholderNonNativeOwnerClass = NewObject<UBlueprintGeneratedClass>(GetTransientPackage(), TEXT("SuziePlaceholderBlueprintClass"), RF_Public | RF_Transient | RF_MarkAsRootSet);
        PlaceholderNonNativeOwnerClass->SetSuperStruct(UObject::StaticClass());
        PlaceholderNonNativeOwnerClass->ClassFlags = CLASS_Abstract | CLASS_Hidden | CLASS_Transient;

        PlaceholderNonNativeOwnerClass->Bind();
        PlaceholderNonNativeOwnerClass->StaticLink(true);
    }
    return PlaceholderNonNativeOwnerClass;
}

UClass* FSuziePluginModule::FindOrCreateUnregisteredClass(FDynamicClassGenerationContext& Context, const FString& ClassPath)
{
    // Attempt to find an existing class first
    if (UClass* ExistingClass = FindObject<UClass>(nullptr, *ClassPath))
    {
        return ExistingClass;
    }
    
    const TSharedPtr<FJsonObject> ClassDefinition = Context.GlobalObjectMap->GetObjectField(ClassPath);
    checkf(ClassDefinition.IsValid(), TEXT("Failed to find class object by path %s"), *ClassPath);
    
    const FString ObjectType = ClassDefinition->GetStringField(TEXT("type"));
    checkf(ObjectType == TEXT("Class"), TEXT("FindOrCreateUnregisteredClass expected Class object %s, got object of type %s"), *ClassPath, *ObjectType);
    
    const FString ParentClassPath = ClassDefinition->GetStringField(TEXT("super_struct"));
    UClass* ParentClass = FindOrCreateClass(Context, ParentClassPath);
    if (!ParentClass)
    {
        UE_LOG(LogSuzie, Error, TEXT("Parent class not found: %s"), *ParentClassPath);
        return nullptr;
    }
    
    FString PackageName;
    FString ClassName;
    ParseObjectPath(ClassPath, PackageName, ClassName);

    // DeferredRegister for UClass will automatically find the package by name, but we should still prime it before that
    FindOrCreatePackage(Context, PackageName);

    // Note that only flags that are set manually (e.g. non-computed flags) should be listed here
    static const TArray<TPair<FString, EClassFlags>> ClassFlagNameLookup = {
        {TEXT("CLASS_Abstract"), CLASS_Abstract},
        {TEXT("CLASS_EditInlineNew"), CLASS_EditInlineNew},
        {TEXT("CLASS_NotPlaceable"), CLASS_NotPlaceable},
        {TEXT("CLASS_CollapseCategories"), CLASS_CollapseCategories},
        {TEXT("CLASS_Const"), CLASS_Const},
        {TEXT("CLASS_DefaultToInstanced"), CLASS_DefaultToInstanced},
        {TEXT("CLASS_Interface"), CLASS_Interface},
    };

    // Convert class flag names to the class flags bitmask
    EClassFlags ClassFlags = CLASS_Native | CLASS_Intrinsic;
    const TSet<FString> ClassFlagNames = ParseFlags(ClassDefinition->GetStringField(TEXT("class_flags")));
    for (const auto& [ClassFlagName, ClassFlagBit] : ClassFlagNameLookup)
    {
        if (ClassFlagNames.Contains(ClassFlagName))
        {
            ClassFlags |= ClassFlagBit;
        }
    }
    
    // UE does not provide a copy constructor for that type, but it is a very much memcpy-able POD type
    FUObjectCppClassStaticFunctions ClassStaticFunctions;
    memcpy(&ClassStaticFunctions, &ParentClass->CppClassStaticFunctions, sizeof(ClassStaticFunctions));
    
    //Code below is taken from GetPrivateStaticClassBody
    //Allocate memory from ObjectAllocator for class object and call class constructor directly
    UClass* ConstructedClassObject = static_cast<UClass*>(GUObjectAllocator.AllocateUObject(sizeof(UClass), alignof(UClass), true));
    ::new (ConstructedClassObject)UClass(
        EC_StaticConstructor,
        *ClassName,
        ParentClass->GetStructureSize(),
        ParentClass->GetMinAlignment(),
        ClassFlags,
        CASTCLASS_None,
        UObject::StaticConfigName(),
        RF_Public | RF_MarkAsNative | RF_MarkAsRootSet,
        &FSuziePluginModule::PolymorphicClassConstructorInvocationHelper,
        ParentClass->ClassVTableHelperCtorCaller,
        MoveTemp(ClassStaticFunctions));

    //Set super structure and ClassWithin (they are required prior to registering)
    ConstructedClassObject->SetSuperStruct(ParentClass);
    ConstructedClassObject->ClassWithin = UObject::StaticClass();
    ConstructedClassObject->TotalFieldCount = ParentClass->TotalFieldCount;

    //Field with cpp type info only exists in editor, in shipping SetCppTypeInfoStatic is empty
    static const FCppClassTypeInfoStatic TypeInfoStatic{false};
    ConstructedClassObject->SetCppTypeInfoStatic(&TypeInfoStatic);
    
    //Register pending object, apply class flags, set static type info and link it
    ConstructedClassObject->RegisterDependencies();
    ConstructedClassObject->DeferredRegister(UClass::StaticClass(), *PackageName, *ClassName);

    Context.ClassesPendingConstruction.Add(ConstructedClassObject, ClassPath);
    
    UE_LOG(LogSuzie, Verbose, TEXT("Created dynamic class: %s"), *ClassName);
    return ConstructedClassObject;
}

// Accessor to allow calling SetOffset_Internal on property objects
class FPropertyAccessor : public FProperty
{
public:
    FPropertyAccessor() = delete;
    static void SetPropertyOffsetDirect(FProperty* InProperty, const int32 NewOffset)
    {
        static_cast<FPropertyAccessor*>(InProperty)->SetOffset_Internal(NewOffset);
    }
};

// Internal property type injected into DestructorLink of dynamic classes to force the destruction of their properties (despite the class being marked as native)
class FDynamicClassDestructorCallProperty : public FProperty
{
    TArray<const FProperty*> PropertiesToDestroy;
public:
    explicit FDynamicClassDestructorCallProperty(UClass* InOwner, const TArray<const FProperty*>& InPropertiesToDestroy) :
        FProperty(InOwner, TEXT("DynamicClassDestructorCall"), RF_Public),
        PropertiesToDestroy(InPropertiesToDestroy)
    {
        PropertyFlags |= CPF_ZeroConstructor;
        SetElementSize(0);
    }
    virtual void LinkInternal(FArchive& Ar) override {}
    virtual void DestroyValueInternal(void* Dest) const override {}
    virtual bool ContainsClearOnFinishDestroyInternal(TArray<const FStructProperty*>& EncounteredStructProps) const override { return true; }
    
    virtual void FinishDestroyInternal(void* Data) const override
    {
        checkf(GetOffset_ForInternal() == 0, TEXT("Dynamic class destructor call property expected to be at offset 0 in the class"));
        for (const FProperty* Property : PropertiesToDestroy)
        {
            Property->DestroyValue_InContainer(Data);
        }
    }
};

// Note that new objects can be created from other threads, but we only touch this map when creating dynamic classes,
// so we do not need an explicit mutex to guard the access to it during class initialization
static TMap<UClass*, FDynamicClassConstructionData> DynamicClassConstructionData;

UClass* FSuziePluginModule::FindOrCreateClass(FDynamicClassGenerationContext& Context, const FString& ClassPath)
{
    // Return existing class if exists
    UClass* NewClass = FindObject<UClass>(nullptr, *ClassPath);

    // If class already exists and is not pending constructed, we do not need to do anything
    if (NewClass && !Context.ClassesPendingConstruction.Contains(NewClass))
    {
        return NewClass;
    }

    // If we have not created the class yet, create it now
    if (NewClass == nullptr)
    {
        NewClass = FindOrCreateUnregisteredClass(Context, ClassPath);
        if (NewClass == nullptr)
        {
            UE_LOG(LogSuzie, Error, TEXT("Failed to create dynamic class: %s"), *ClassPath);
            return nullptr;
        }
    }

    // Remove the class from the pending construction set to prevent possible re-entry
    Context.ClassesPendingConstruction.Remove(NewClass);

    const TSharedPtr<FJsonObject> ClassDefinition = Context.GlobalObjectMap->GetObjectField(ClassPath);

    TArray<const FProperty*> PropertiesWithDestructor;
    TArray<const FProperty*> PropertiesWithConstructor;
    FArchive EmptyPropertyLinkArchive;

    // Add properties to the class
    const TArray<TSharedPtr<FJsonValue>>& Properties = ClassDefinition->GetArrayField(TEXT("properties"));
    for (const TSharedPtr<FJsonValue>& PropertyDescriptor : Properties)
    {
        // We want all properties to be editable, visible and blueprint assignable
        const EPropertyFlags ExtraPropertyFlags = CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintAssignable;
        if (FProperty* CreatedProperty = AddPropertyToStruct(Context, NewClass, PropertyDescriptor->AsObject(), ExtraPropertyFlags))
        {
            // Because this is a native class, we have to link the property offset manually here rather than expecting StaticLink to do it for us
            NewClass->PropertiesSize = CreatedProperty->Link(EmptyPropertyLinkArchive);
            NewClass->MinAlignment = FMath::Max(NewClass->MinAlignment, CreatedProperty->GetMinAlignment());
            NewClass->TotalFieldCount++;

            // Add property into the constructor/destructor lists based on its flags
            if (!CreatedProperty->HasAnyPropertyFlags(CPF_IsPlainOldData | CPF_NoDestructor))
            {
                PropertiesWithDestructor.Add(CreatedProperty);
            }
            if (!CreatedProperty->HasAnyPropertyFlags(CPF_ZeroConstructor))
            {
                PropertiesWithConstructor.Add(CreatedProperty);
            }
        }
    }

    // Add functions to the class
    const TArray<TSharedPtr<FJsonValue>>& Children = ClassDefinition->GetArrayField(TEXT("children"));
    for (const TSharedPtr<FJsonValue>& FunctionObjectPathValue : Children)
    {
        FString ChildPath = FunctionObjectPathValue->AsString();
        const TSharedPtr<FJsonObject> ChildObject = Context.GlobalObjectMap->GetObjectField(ChildPath);
        if (ChildObject && ChildObject->GetStringField(TEXT("type")) == TEXT("Function"))
        {
            AddFunctionToClass(Context, NewClass, ChildPath);
        }
    }

    // Mark all dynamic classes as blueprintable and blueprint types, otherwise we will not be able to use them
    NewClass->SetMetaData(FBlueprintMetadata::MD_AllowableBlueprintVariableType, TEXT("true"));
    NewClass->SetMetaData(FBlueprintMetadata::MD_IsBlueprintBase, TEXT("true"));

    if (NewClass->IsChildOf<UActorComponent>())
    {
        NewClass->SetMetaData(FBlueprintMetadata::MD_BlueprintSpawnableComponent, TEXT("true"));
    }

    // Bind parent class to this class and link properties to calculate their runtime derived data
    NewClass->Bind();
    NewClass->StaticLink();
    NewClass->SetSparseClassDataStruct(NewClass->GetSparseClassDataArchetypeStruct());

    // If we have properties that need destructor call, we add a synthetic property of custom type to DestructorLink
    if (!PropertiesWithDestructor.IsEmpty())
    {
        FProperty* DestructorCallProperty = new FDynamicClassDestructorCallProperty(NewClass, PropertiesWithDestructor);
        DestructorCallProperty->DestructorLinkNext = NewClass->DestructorLink;
        NewClass->DestructorLink = DestructorCallProperty;
    }

    // Stash the properties that need to be constructed on the class data so polymorphic constructor can access them easily
    FDynamicClassConstructionData& ClassConstructionData = DynamicClassConstructionData.FindOrAdd(NewClass);
    ClassConstructionData.PropertiesToConstruct = PropertiesWithConstructor;

    const FString ClassDefaultObjectPath = ClassDefinition->GetStringField(TEXT("class_default_object"));
    
    // Class default object can be created at this point
    Context.ClassesPendingFinalization.Add(NewClass, ClassDefaultObjectPath);
    
    return NewClass;
}

UScriptStruct* FSuziePluginModule::FindOrCreateScriptStruct(FDynamicClassGenerationContext& Context, const FString& StructPath)
{
    // Check if we have already created this struct
    if (UScriptStruct* ExistingScriptStruct = FindObject<UScriptStruct>(nullptr, *StructPath))
    {
        return ExistingScriptStruct;
    }

    const TSharedPtr<FJsonObject> StructDefinition = Context.GlobalObjectMap->GetObjectField(StructPath);
    checkf(StructDefinition.IsValid(), TEXT("Failed to find script struct object by path %s"), *StructPath);
    
    const FString ObjectType = StructDefinition->GetStringField(TEXT("type"));
    checkf(ObjectType == TEXT("ScriptStruct"), TEXT("FindOrCreateScriptStruct expected ScriptStruct object %s, got object of type %s"), *StructPath, *ObjectType);

    // Resolve parent struct for this struct before we attempt to create this struct
    UScriptStruct* SuperScriptStruct = nullptr;
    FString ParentStructPath;
    if (StructDefinition->TryGetStringField(TEXT("super_struct"), ParentStructPath))
    {
        SuperScriptStruct = FindOrCreateScriptStruct(Context, ParentStructPath);
        if (SuperScriptStruct == nullptr)
        {
            UE_LOG(LogSuzie, Error, TEXT("Parent script struct not found: %s"), *ParentStructPath);
            return nullptr;
        }
    }
    
    FString PackageName;
    FString ObjectName;
    ParseObjectPath(StructPath, PackageName, ObjectName);

    // Create a package for the struct or reuse the existing package. Make sure it's marked as Native package
    UPackage* Package = FindOrCreatePackage(Context, PackageName);
    
    UScriptStruct* NewStruct = NewObject<UScriptStruct>(Package, *ObjectName, RF_Public | RF_MarkAsRootSet);

    // Set super script struct and copy inheritable flags first if this struct has a parent (most structs do not)
    if (SuperScriptStruct != nullptr)
    {
        NewStruct->SetSuperStruct(SuperScriptStruct);
        NewStruct->StructFlags = (EStructFlags) ((int32)NewStruct->StructFlags | (SuperScriptStruct->StructFlags & STRUCT_Inherit));
    }

    // Note that only flags that are set manually (e.g. non-computed flags) should be listed here
    static const TArray<TPair<FString, EStructFlags>> StructFlagNameLookup = {
        {TEXT("STRUCT_Atomic"), STRUCT_Atomic},
        {TEXT("STRUCT_Immutable"), STRUCT_Immutable},
    };

    // Convert struct flag names to the struct flags bitmask
    const TSet<FString> StructFlagNames = ParseFlags(StructDefinition->GetStringField(TEXT("struct_flags")));
    for (const auto& [StructFlagName, StructFlagBit] : StructFlagNameLookup)
    {
        if (StructFlagNames.Contains(StructFlagName))
        {
            NewStruct->StructFlags = (EStructFlags)((int32)NewStruct->StructFlags | StructFlagBit);
        }
    }

    // Initialize properties for the struct
    TArray<TSharedPtr<FJsonValue>> Properties = StructDefinition->GetArrayField(TEXT("properties"));
    for (const TSharedPtr<FJsonValue>& PropertyDescriptor : Properties)
    {
        // We want all properties to be editable, visible and blueprint assignable
        const EPropertyFlags ExtraPropertyFlags = CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintAssignable;
        AddPropertyToStruct(Context, NewStruct, PropertyDescriptor->AsObject(), ExtraPropertyFlags);
    }
    
    // Mark all dynamic script structs as blueprint types
    NewStruct->SetMetaData(FBlueprintMetadata::MD_AllowableBlueprintVariableType, TEXT("true"));

    // Bind the newly created struct and link it to assign property offsets and calculate the size
    NewStruct->Bind();
    NewStruct->PrepareCppStructOps();
    NewStruct->StaticLink(true);

    // The engine does not gracefully handle empty structs, so force the struct size to be at least one byte
    if (NewStruct->GetPropertiesSize() == 0)
    {
        NewStruct->MinAlignment = 1;
        NewStruct->SetPropertiesSize(1);
    }
    
    UE_LOG(LogSuzie, Verbose, TEXT("Created struct: %s"), *ObjectName);

    // Struct properties using this struct can be created at this point
    return NewStruct;
}

UEnum* FSuziePluginModule::FindOrCreateEnum(FDynamicClassGenerationContext& Context, const FString& EnumPath)
{
    // Check if we have already created this enum
    if (UEnum* ExistingEnum = FindObject<UEnum>(nullptr, *EnumPath))
    {
        return ExistingEnum;
    }

    const TSharedPtr<FJsonObject> EnumDefinition = Context.GlobalObjectMap->GetObjectField(EnumPath);
    checkf(EnumDefinition.IsValid(), TEXT("Failed to find enum object by path %s"), *EnumPath);
    
    const FString ObjectType = EnumDefinition->GetStringField(TEXT("type"));
    checkf(ObjectType == TEXT("Enum"), TEXT("FindOrCreateEnum expected Enum object %s, got object of type %s"), *EnumPath, *ObjectType);

    FString PackageName;
    FString ObjectName;
    ParseObjectPath(EnumPath, PackageName, ObjectName);

    // Create a package for the struct or reuse the existing package. Make sure it's marked as Native package
    UPackage* Package = FindOrCreatePackage(Context, PackageName);
    
    UEnum* NewEnum = NewObject<UEnum>(Package, *ObjectName, RF_Public | RF_MarkAsRootSet);

    // Set CppType. It is generally not used by the engine, but is useful to determine whenever enum is namespaced or not for CppForm deduction
    NewEnum->CppType = EnumDefinition->GetStringField(TEXT("cpp_type"));

    TArray<TPair<FName, int64>> EnumNames;
    bool bContainsFullyQualifiedNames = false;

    // Parse enum constant names and values
    TArray<TSharedPtr<FJsonValue>> EnumNameJsonEntries = EnumDefinition->GetArrayField(TEXT("names"));
    for (const TSharedPtr<FJsonValue>& EnumNameAndValueArrayValue : EnumNameJsonEntries)
    {
        const TArray<TSharedPtr<FJsonValue>>& EnumNameAndValueArray = EnumNameAndValueArrayValue->AsArray();
        if (EnumNameAndValueArray.Num() == 2)
        {
            const FString EnumConstantName = EnumNameAndValueArray[0]->AsString();
            // TODO: Using numbers to represent enumeration values is not safe, large int64 values cannot be adequately represented as json double precision numbers
            const int64 EnumConstantValue = EnumNameAndValueArray[1]->AsNumber();

            EnumNames.Add({FName(*EnumConstantName), EnumConstantValue});
            bContainsFullyQualifiedNames |= EnumConstantName.Contains(TEXT("::"));
        }
    }

    // TODO: CppForm and Flags are not currently dumped, but we can assume flags None for most enums and guess CppForm based on names and CppType
    const bool bCppTypeIsNamespaced = NewEnum->CppType.Contains(TEXT("::"));
    const UEnum::ECppForm EnumCppForm = bContainsFullyQualifiedNames ? (bCppTypeIsNamespaced ? UEnum::ECppForm::Namespaced : UEnum::ECppForm::EnumClass) : UEnum::ECppForm::Regular;
    const EEnumFlags EnumFlags = EEnumFlags::None;

    // We do not need to generate _MAX key because it will always be present in the enum definition
    NewEnum->SetEnums(EnumNames, EnumCppForm, EnumFlags, false);

    // Mark all dynamic enums as blueprint types
    NewEnum->SetMetaData(*FBlueprintMetadata::MD_AllowableBlueprintVariableType.ToString(), TEXT("true"));
    
    UE_LOG(LogSuzie, Verbose, TEXT("Created enum: %s"), *ObjectName);

    return NewEnum;
}

UFunction* FSuziePluginModule::FindOrCreateFunction(FDynamicClassGenerationContext& Context, const FString& FunctionPath)
{
    // Check if the function already exists
    if (UFunction* ExistingFunction = FindObject<UFunction>(nullptr, *FunctionPath))
    {
        return ExistingFunction;
    }
    
    FString ClassPathOrPackageName;
    FString ObjectName;
    ParseObjectPath(FunctionPath, ClassPathOrPackageName, ObjectName);

    // Function can be outered either to a class or to a package, we can decide based on whenever there is a separator in the path
    UObject* FunctionOuterObject;
    int32 PackageNameSeparatorIndex{};
    if (ClassPathOrPackageName.FindChar('.', PackageNameSeparatorIndex))
    {
        // This is a class path because it is at least two levels deep. We do not need our outer to be registered, just to exist
        FunctionOuterObject = FindOrCreateUnregisteredClass(Context, ClassPathOrPackageName);
    }
    else
    {
        // This is a package and this function is a top level function (most likely a delegate signature)
        FunctionOuterObject = FindOrCreatePackage(Context, ClassPathOrPackageName);
    }

    // Check if the function already exists in its parent object
    if (UFunction* ExistingFunction = FindObjectFast<UFunction>(FunctionOuterObject, *ObjectName))
    {
        return ExistingFunction;
    }

    // Note that only flags that are set manually (e.g. non-computed flags) should be listed here
    static const TArray<TPair<FString, EFunctionFlags>> FunctionFlagNameLookup = {
        {TEXT("FUNC_Final"), FUNC_Final},
        {TEXT("FUNC_BlueprintAuthorityOnly"), FUNC_BlueprintAuthorityOnly},
        {TEXT("FUNC_BlueprintCosmetic"), FUNC_BlueprintCosmetic},
        {TEXT("FUNC_Net"), FUNC_Net},
        {TEXT("FUNC_NetReliable"), FUNC_NetReliable},
        {TEXT("FUNC_NetRequest"), FUNC_NetRequest},
        {TEXT("FUNC_Exec"), FUNC_Exec},
        {TEXT("FUNC_Event"), FUNC_Event},
        {TEXT("FUNC_NetResponse"), FUNC_NetResponse},
        {TEXT("FUNC_Static"), FUNC_Static},
        {TEXT("FUNC_NetMulticast"), FUNC_NetMulticast},
        {TEXT("FUNC_UbergraphFunction"), FUNC_UbergraphFunction},
        {TEXT("FUNC_MulticastDelegate"), FUNC_MulticastDelegate},
        {TEXT("FUNC_Public"), FUNC_Public},
        {TEXT("FUNC_Private"), FUNC_Private},
        {TEXT("FUNC_Protected"), FUNC_Protected},
        {TEXT("FUNC_Delegate"), FUNC_Delegate},
        {TEXT("FUNC_NetServer"), FUNC_NetServer},
        {TEXT("FUNC_NetClient"), FUNC_NetClient},
        {TEXT("FUNC_BlueprintCallable"), FUNC_BlueprintCallable},
        {TEXT("FUNC_BlueprintEvent"), FUNC_BlueprintEvent},
        {TEXT("FUNC_BlueprintPure"), FUNC_BlueprintPure},
        {TEXT("FUNC_EditorOnly"), FUNC_EditorOnly},
        {TEXT("FUNC_Const"), FUNC_Const},
        {TEXT("FUNC_NetValidate"), FUNC_NetValidate},
        {TEXT("FUNC_HasOutParms"), FUNC_HasOutParms},
        {TEXT("FUNC_HasDefaults"), FUNC_HasDefaults},
    };

    const TSharedPtr<FJsonObject> FunctionDefinition = Context.GlobalObjectMap->GetObjectField(FunctionPath);
    checkf(FunctionDefinition.IsValid(), TEXT("Failed to find function object by path %s"), *FunctionPath);
    
    const FString ObjectType = FunctionDefinition->GetStringField(TEXT("type"));
    checkf(ObjectType == TEXT("Function"), TEXT("FindOrCreateFunction expected Function object %s, got object of type %s"), *FunctionPath, *ObjectType);
    
    // Convert struct flag names to the struct flags bitmask
    const TSet<FString> FunctionFlagNames = ParseFlags(FunctionDefinition->GetStringField(TEXT("function_flags")));
    EFunctionFlags FunctionFlags = FUNC_None;
    for (const auto& [FunctionFlagName, FunctionFlagBit] : FunctionFlagNameLookup)
    {
        if (FunctionFlagNames.Contains(FunctionFlagName))
        {
            FunctionFlags |= FunctionFlagBit;
        }
    }

    // Have to temporarily mark the function as RF_ArchetypeObject to be able to create functions with UPackage as outer
    UFunction* NewFunction = NewObject<UFunction>(FunctionOuterObject, *ObjectName, RF_Public | RF_MarkAsRootSet | RF_ArchetypeObject);
    NewFunction->ClearFlags(RF_ArchetypeObject);
    NewFunction->FunctionFlags |= FunctionFlags;

    // Since this function is not marked as Native, we have to initialize Script bytecode for it
    // Most basic valid kismet bytecode for a function would be EX_Return EX_Nothing EX_EndOfScript, so generate that
    NewFunction->Script.Append({EX_Return, EX_Nothing, EX_EndOfScript});

    // Create function parameter properties (and function return value property)
    TArray<TSharedPtr<FJsonValue>> Properties = FunctionDefinition->GetArrayField(TEXT("properties"));
    for (const TSharedPtr<FJsonValue>& PropertyDescriptor : Properties)
    {
        AddPropertyToStruct(Context, NewFunction, PropertyDescriptor->AsObject());
    }

    // This function will always be linked as a last element of the list, so it has no next element
    NewFunction->Next = nullptr;

    // Bind the function and calculate property layout and function locals size
    NewFunction->Bind();
    NewFunction->StaticLink(true);

    // Do some tagging of the function for convenience based on parameter types and names
    for (TFieldIterator<FProperty> PropertyIterator(NewFunction); PropertyIterator; ++PropertyIterator)
    {
        const FProperty* Property = *PropertyIterator;
        if (!Property->HasAllPropertyFlags(CPF_Parm) || Property->HasAnyPropertyFlags(CPF_ReturnParm)) continue;

        // Object properties called WorldContext/WorldContextObject are automatically tagged as world context for convenience
        if (Property->IsA<FObjectProperty>() && (Property->GetFName() == TEXT("WorldContext") || Property->GetFName() == TEXT("WorldContextObject")))
        {
            NewFunction->SetMetaData(FBlueprintMetadata::MD_WorldContext, *Property->GetName());
        }
        // Latent Info struct parameter properties should always be tagged as LatentInfo and indicate async BP functions
        if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property); StructProperty && StructProperty->Struct == FLatentActionInfo::StaticStruct())
        {
            NewFunction->SetMetaData(FBlueprintMetadata::MD_LatentInfo, *Property->GetName());
            NewFunction->SetMetaData(FBlueprintMetadata::MD_Latent, TEXT("true"));
        }
    }

    UE_LOG(LogSuzie, VeryVerbose, TEXT("Created function %s in outer %s"), *ObjectName, *FunctionOuterObject->GetName());
    return NewFunction;
}

void FSuziePluginModule::ParseObjectPath(const FString& ObjectPath, FString& OutOuterObjectPath, FString& OutObjectName)
{
    int32 ObjectNameSeparatorIndex;
    if (ObjectPath.FindLastChar(':', ObjectNameSeparatorIndex))
    {
        // There is a sub-object separator in the path name, string past it is the object name
        OutOuterObjectPath = ObjectPath.Mid(0, ObjectNameSeparatorIndex);
        OutObjectName = ObjectPath.Mid(ObjectNameSeparatorIndex + 1);
    }
    else if (ObjectPath.FindLastChar('.', ObjectNameSeparatorIndex))
    {
        // This is a top level object (or this is a legacy path), string past the asset name separator is the object name
        OutOuterObjectPath = ObjectPath.Mid(0, ObjectNameSeparatorIndex);
        OutObjectName = ObjectPath.Mid(ObjectNameSeparatorIndex + 1);
    }
    else
    {
        // This is a top level object (UPackage) name
        OutOuterObjectPath = TEXT("");
        OutObjectName = ObjectPath;
    }
}

TSet<FString> FSuziePluginModule::ParseFlags(const FString& Flags)
{
    TArray<FString> FlagsArray;
    Flags.ParseIntoArray(FlagsArray, TEXT(" | "), true);
    TSet<FString> ReturnFlags;
    for (auto Flag : FlagsArray) ReturnFlags.Add(Flag);
    return ReturnFlags;
}

FProperty* FSuziePluginModule::AddPropertyToStruct(FDynamicClassGenerationContext& Context, UStruct* Struct, const TSharedPtr<FJsonObject>& PropertyJson, const EPropertyFlags ExtraPropertyFlags)
{
    if (FProperty* NewProperty = BuildProperty(Context, Struct, PropertyJson, ExtraPropertyFlags))
    {
        // This property will always be linked as a last element of the list, so it has no next element
        NewProperty->Next = nullptr;

        // Link new property to the end of the linked property list
        if (Struct->ChildProperties != nullptr)
        {
            FField* CurrentProperty = Struct->ChildProperties;
            while (CurrentProperty->Next)
            {
                CurrentProperty = CurrentProperty->Next;
            }
            CurrentProperty->Next = NewProperty;
        }
        else
        {
            // This is the first property in the struct, assign it as a head of the linked property list
            Struct->ChildProperties = NewProperty;
        }
        UE_LOG(LogSuzie, VeryVerbose, TEXT("Added property %s to struct %s"), *NewProperty->GetName(), *Struct->GetName());
        return NewProperty;
    }
    return nullptr;
}

void FSuziePluginModule::AddFunctionToClass(FDynamicClassGenerationContext& Context, UClass* Class, const FString& FunctionPath, const EFunctionFlags ExtraFunctionFlags)
{
    if (UFunction* NewFunction = FindOrCreateFunction(Context, FunctionPath))
    {
        // Append additional flags to the function
        NewFunction->FunctionFlags |= ExtraFunctionFlags;
        
        // This function will always be linked as a last element of the list, so it has no next element
        NewFunction->Next = nullptr;
    
        // Link new function to the end of the linked property list
        if (Class->Children != nullptr)
        {
            UField* CurrentFunction = Class->Children;
            while (CurrentFunction->Next)
            {
                CurrentFunction = CurrentFunction->Next;
            }
            CurrentFunction->Next = NewFunction;
        }
        else
        {
            // This is the first function in the class, assign it as a head of the linked function list
            Class->Children = NewFunction;
        }

        // Add the function to the function lookup for the class
        Class->AddFunctionToFunctionMap(NewFunction, NewFunction->GetFName());

        UE_LOG(LogSuzie, VeryVerbose, TEXT("Added function %s to class %s"), *NewFunction->GetName(), *Class->GetName());
    }
}

FProperty* FSuziePluginModule::BuildProperty(FDynamicClassGenerationContext& Context, FFieldVariant Owner, const TSharedPtr<FJsonObject>& PropertyJson, EPropertyFlags ExtraPropertyFlags)
{
    // Note that only flags that are set manually (e.g. non-computed flags) should be listed here
    static const TArray<TPair<FString, EPropertyFlags>> PropertyFlagNameLookup = {
        {TEXT("CPF_Edit"), CPF_Edit},
        {TEXT("CPF_ConstParm"), CPF_ConstParm},
        {TEXT("CPF_BlueprintVisible"), CPF_BlueprintVisible},
        {TEXT("CPF_ExportObject"), CPF_ExportObject},
        {TEXT("CPF_BlueprintReadOnly"), CPF_BlueprintReadOnly},
        {TEXT("CPF_Net"), CPF_Net},
        {TEXT("CPF_EditFixedSize"), CPF_EditFixedSize},
        {TEXT("CPF_Parm"), CPF_Parm},
        {TEXT("CPF_OutParm"), CPF_OutParm},
        {TEXT("CPF_ReturnParm"), CPF_ReturnParm},
        {TEXT("CPF_DisableEditOnTemplate"), CPF_DisableEditOnTemplate},
        {TEXT("CPF_NonNullable"), CPF_NonNullable},
        {TEXT("CPF_Transient"), CPF_Transient},
        {TEXT("CPF_RequiredParm"), CPF_RequiredParm},
        {TEXT("CPF_DisableEditOnInstance"), CPF_DisableEditOnInstance},
        {TEXT("CPF_EditConst"), CPF_EditConst},
        {TEXT("CPF_DisableEditOnInstance"), CPF_DisableEditOnInstance},
        {TEXT("CPF_InstancedReference"), CPF_InstancedReference},
        {TEXT("CPF_DuplicateTransient"), CPF_DuplicateTransient},
        {TEXT("CPF_SaveGame"), CPF_SaveGame},
        {TEXT("CPF_NoClear"), CPF_NoClear},
        {TEXT("CPF_SaveGame"), CPF_SaveGame},
        {TEXT("CPF_ReferenceParm"), CPF_ReferenceParm},
        {TEXT("CPF_BlueprintAssignable"), CPF_BlueprintAssignable},
        {TEXT("CPF_Deprecated"), CPF_Deprecated},
        {TEXT("CPF_RepSkip"), CPF_RepSkip},
        {TEXT("CPF_Deprecated"), CPF_Deprecated},
        {TEXT("CPF_RepNotify"), CPF_RepNotify},
        {TEXT("CPF_Interp"), CPF_Interp},
        {TEXT("CPF_NonTransactional"), CPF_NonTransactional},
        {TEXT("CPF_EditorOnly"), CPF_EditorOnly},
        {TEXT("CPF_AutoWeak"), CPF_AutoWeak},
        // CPF_ContainsInstancedReference is actually computed, but it is set by the compiler and not in runtime,
        // so we need to either carry it over (like we do here), or manually set it on container properties when their
        // elements have CPF_ContainsInstancedReference
        {TEXT("CPF_ContainsInstancedReference"), CPF_ContainsInstancedReference},
        {TEXT("CPF_AssetRegistrySearchable"), CPF_AssetRegistrySearchable},
        {TEXT("CPF_SimpleDisplay"), CPF_SimpleDisplay},
        {TEXT("CPF_AdvancedDisplay"), CPF_AdvancedDisplay},
        {TEXT("CPF_Protected"), CPF_Protected},
        {TEXT("CPF_BlueprintCallable"), CPF_BlueprintCallable},
        {TEXT("CPF_BlueprintAuthorityOnly"), CPF_BlueprintAuthorityOnly},
        {TEXT("CPF_TextExportTransient"), CPF_TextExportTransient},
        {TEXT("CPF_NonPIEDuplicateTransient"), CPF_NonPIEDuplicateTransient},
        {TEXT("CPF_PersistentInstance"), CPF_PersistentInstance},
        {TEXT("CPF_UObjectWrapper"), CPF_UObjectWrapper},
        {TEXT("CPF_NativeAccessSpecifierPublic"), CPF_NativeAccessSpecifierPublic},
        {TEXT("CPF_NativeAccessSpecifierProtected"), CPF_NativeAccessSpecifierProtected},
        {TEXT("CPF_NativeAccessSpecifierPrivate"), CPF_NativeAccessSpecifierPrivate},
        {TEXT("CPF_SkipSerialization"), CPF_SkipSerialization},
        {TEXT("CPF_TObjectPtr"), CPF_TObjectPtr},
        {TEXT("CPF_AllowSelfReference"), CPF_AllowSelfReference},
        // This is set automatically for most property types, but Kismet Compiler also tags properties with this manually so carry over the flag just in case
        {TEXT("CPF_HasGetValueTypeHash"), CPF_HasGetValueTypeHash},
    };

    // Convert struct flag names to the struct flags bitmask
    const TSet<FString> PropertyFlagNames = ParseFlags(PropertyJson->GetStringField(TEXT("flags")));
    EPropertyFlags PropertyFlags = ExtraPropertyFlags;
    for (const auto& [PropertyFlagName, PropertyFlagBit] : PropertyFlagNameLookup)
    {
        if (PropertyFlagNames.Contains(PropertyFlagName))
        {
            PropertyFlags |= PropertyFlagBit;
        }
    }

    const FString PropertyName = PropertyJson->GetStringField(TEXT("name"));
    const FString PropertyType = PropertyJson->GetStringField(TEXT("type"));

    FProperty* NewProperty = CastField<FProperty>(FField::Construct(FName(*PropertyType), Owner, FName(*PropertyName), RF_Public));
    if (NewProperty == nullptr)
    {
        UE_LOG(LogSuzie, Warning, TEXT("Failed to create property of type %s: not supported"), *PropertyType);
        return nullptr;
    }
    
    NewProperty->ArrayDim = PropertyJson->GetIntegerField(TEXT("array_dim"));
    NewProperty->PropertyFlags |= PropertyFlags;

    if (FObjectPropertyBase* ObjectPropertyBase = CastField<FObjectPropertyBase>(NewProperty))
    {
        UClass* PropertyClass = FindOrCreateUnregisteredClass(Context, *PropertyJson->GetStringField(TEXT("property_class")));
        // Fall back to UObject class if property class could not be found
        ObjectPropertyBase->PropertyClass = PropertyClass ? PropertyClass : UObject::StaticClass();
        
        // Class properties additionally define MetaClass value
        if (FClassProperty* ClassProperty = CastField<FClassProperty>(NewProperty))
        {
            UClass* MetaClass = FindOrCreateUnregisteredClass(Context, *PropertyJson->GetStringField(TEXT("meta_class")));
            // Fall back to UObject meta-class if meta-class could not be found
            ClassProperty->MetaClass = MetaClass ? MetaClass : UObject::StaticClass();
        }
        else if (FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(NewProperty))
        {
            UClass* MetaClass = FindOrCreateUnregisteredClass(Context, *PropertyJson->GetStringField(TEXT("meta_class")));
            // Fall back to UObject meta-class if meta-class could not be found
            SoftClassProperty->MetaClass = MetaClass ? MetaClass : UObject::StaticClass();
        }
    }
    else if (FInterfaceProperty* InterfaceProperty = CastField<FInterfaceProperty>(NewProperty))
    {
        UClass* InterfaceClass = FindOrCreateUnregisteredClass(Context, *PropertyJson->GetStringField(TEXT("interface_class")));
        // Fall back to UInterface if interface class could not be found
        InterfaceProperty->InterfaceClass = InterfaceClass ? InterfaceClass : UInterface::StaticClass();
    }
    else if (FStructProperty* StructProperty = CastField<FStructProperty>(NewProperty))
    {
        UScriptStruct* Struct = FindOrCreateScriptStruct(Context, PropertyJson->GetStringField(TEXT("struct")));
        // Fall back to FVector if struct class could not be found
        StructProperty->Struct = Struct ? Struct : TBaseStructure<FVector>::Get();
    }
    else if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(NewProperty))
    {
        UEnum* Enum = FindOrCreateEnum(Context, PropertyJson->GetStringField(TEXT("enum")));
        // Fall back to EMovementMode if enum class could not be found
        EnumProperty->SetEnum(Enum ? Enum : StaticEnum<EMovementMode>());

        FProperty* UnderlyingProp = BuildProperty(Context, EnumProperty, PropertyJson->GetObjectField(TEXT("container")));
        EnumProperty->AddCppProperty(UnderlyingProp);
    }
    else if (FByteProperty* ByteProperty = CastField<FByteProperty>(NewProperty))
    {
        // Not all byte properties are enumerations so this field might not be set or be null
        if (PropertyJson->HasTypedField<EJson::String>(TEXT("enum")))
        {
            UEnum* Enum = FindOrCreateEnum(Context, PropertyJson->GetStringField(TEXT("enum")));
            // Fall back to EMovementMode if enum class could not be found
            ByteProperty->Enum = Enum ? Enum : StaticEnum<EMovementMode>();
        }
    }
    else if (FDelegateProperty* DelegateProperty = CastField<FDelegateProperty>(NewProperty))
    {
        UFunction* SignatureFunction = FindOrCreateFunction(Context, PropertyJson->GetStringField(TEXT("signature_function")));
        // Fall back to FOnTimelineEvent delegate signature in the engine if real delegate signature could not be found
        DelegateProperty->SignatureFunction = SignatureFunction ? SignatureFunction : FindObject<UFunction>(nullptr, TEXT("/Script/Engine.OnTimelineEvent__DelegateSignature"));
    }
    else if (FMulticastDelegateProperty* MulticastDelegateProperty = CastField<FMulticastDelegateProperty>(NewProperty))
    {
        UFunction* SignatureFunction = FindOrCreateFunction(Context, PropertyJson->GetStringField(TEXT("signature_function")));
        // Fall back to FOnTimelineEvent delegate signature in the engine if real delegate signature could not be found
        MulticastDelegateProperty->SignatureFunction = SignatureFunction ? SignatureFunction : FindObject<UFunction>(nullptr, TEXT("/Script/Engine.OnTimelineEvent__DelegateSignature"));
    }
    else if (FFieldPathProperty* FieldPathProperty = CastField<FFieldPathProperty>(NewProperty))
    {
        if (PropertyJson->HasTypedField<EJson::String>(TEXT("property_class")))
        {
            FFieldClass* const* PropertyClassPtr = FFieldClass::GetNameToFieldClassMap().Find(TEXT("property_class"));
            // Fall back to FProperty if property class could not be found
            FieldPathProperty->PropertyClass = PropertyClassPtr ? *PropertyClassPtr : FProperty::StaticClass();   
        }
    }
    else
    {
        // TODO: These can be handled together without special casing them by dumping array of FField::GetInnerFields instead of individual fields
        if (FOptionalProperty* OptionalProperty = CastField<FOptionalProperty>(NewProperty))
        {
            FProperty* ValueProperty = BuildProperty(Context, NewProperty, PropertyJson->GetObjectField(TEXT("inner")));
            OptionalProperty->AddCppProperty(ValueProperty);
        }
        else if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(NewProperty))
        {
            FProperty* Inner = BuildProperty(Context, NewProperty, PropertyJson->GetObjectField(TEXT("inner")));
            ArrayProperty->AddCppProperty(Inner);
        }
        else if (FSetProperty* SetProperty = CastField<FSetProperty>(NewProperty))
        {
            FProperty* KeyProp = BuildProperty(Context, NewProperty, PropertyJson->GetObjectField(TEXT("key_prop")));
            SetProperty->AddCppProperty(KeyProp);
        }
        else if (FMapProperty* MapProperty = CastField<FMapProperty>(NewProperty))
        {
            FProperty* KeyProp = BuildProperty(Context, NewProperty, PropertyJson->GetObjectField(TEXT("key_prop")));
            FProperty* ValueProp = BuildProperty(Context, NewProperty, PropertyJson->GetObjectField(TEXT("value_prop")));
        
            MapProperty->AddCppProperty(KeyProp);
            MapProperty->AddCppProperty(ValueProp);
        }
    }

    return NewProperty;
}

bool FSuziePluginModule::ParseObjectConstructionData(const FDynamicClassGenerationContext& Context, const FString& ObjectPath, FDynamicObjectConstructionData& ObjectConstructionData)
{
    // Retrieve the data for the object
    const TSharedPtr<FJsonObject> ObjectDefinition = Context.GlobalObjectMap->GetObjectField(ObjectPath);
    checkf(ObjectDefinition.IsValid(), TEXT("Failed to find data object by path %s"), *ObjectPath);

    FString OuterObjectPath;
    FString ObjectName;
    ParseObjectPath(ObjectPath, OuterObjectPath, ObjectName);
    ObjectConstructionData.ObjectName = FName(*ObjectName);

    // Find the class of this object
    const FString ObjectClassPath = ObjectDefinition->GetStringField(TEXT("class"));
    ObjectConstructionData.ObjectClass = FindObject<UClass>(nullptr, *ObjectClassPath);
    if (ObjectConstructionData.ObjectClass == nullptr)
    {
        UE_LOG(LogSuzie, Warning, TEXT("Failed to parse data object %s because its class %s was not found"), *ObjectPath, *ObjectClassPath);
        return false;
    }

    // Parse object flags. Flags determine how the object should be created
    static const TArray<TPair<FString, EObjectFlags>> ObjectFlagNameLookup = {
        {TEXT("RF_Public"), RF_Public},
        {TEXT("RF_Standalone"), RF_Standalone},
        {TEXT("RF_Transient"), RF_Transient},
        {TEXT("RF_Transactional"), RF_Transactional},
        {TEXT("RF_ArchetypeObject"), RF_ArchetypeObject},
        {TEXT("RF_ClassDefaultObject"), RF_ClassDefaultObject},
        {TEXT("RF_DefaultSubObject"), RF_DefaultSubObject},
    };

    // Convert struct flag names to the struct flags bitmask
    const TSet<FString> ObjectFlagNames = ParseFlags(ObjectDefinition->GetStringField(TEXT("object_flags")));
    ObjectConstructionData.ObjectFlags = RF_NoFlags;
    for (const auto& [ObjectFlagName, ObjectFlagBitmask] : ObjectFlagNameLookup)
    {
        if (ObjectFlagNames.Contains(ObjectFlagName))
        {
            ObjectConstructionData.ObjectFlags |= ObjectFlagBitmask;
        }
    }
    return true;
}

void FSuziePluginModule::DeserializeEnumValue(const FNumericProperty* UnderlyingProperty, void* PropertyValuePtr, const UEnum* Enum, const TSharedPtr<FJsonValue>& JsonPropertyValue)
{
    if (JsonPropertyValue->Type == EJson::String)
    {
        // If this is a string value, it is a name of the enum constant that we need to parse to an enum value
        int32 EnumIndex = Enum->GetIndexByNameString(JsonPropertyValue->AsString(), EGetByNameFlags::None);

        // Validate that the value was actually valid, and if it was not, reset to the first enum constant
        if (EnumIndex == INDEX_NONE)
        {
            UE_LOG(LogSuzie, Warning, TEXT("Unknown enum constant name %s for enum %s when parsing value of property %s"),
                *JsonPropertyValue->AsString(), *Enum->GetPathName(), *UnderlyingProperty->GetPathName());
            EnumIndex = 0;
        }

        // Retrieve the enum value for the constant index and set it to the property
        const int64 EnumValue = Enum->GetValueByIndex(EnumIndex);
        UnderlyingProperty->SetIntPropertyValue(PropertyValuePtr, EnumValue);
    }
    else
    {
        // If this is a numeric value, it could be a direct enum value as integer, so set it directly without parsing
        int64 EnumValue = (int64)JsonPropertyValue->AsNumber();

        // Validate the value as a valid enum constant and fallback to first enum constant if it is not
        if (!Enum->IsValidEnumValue(EnumValue))
        {
            UE_LOG(LogSuzie, Warning, TEXT("Invalid enum constant value %lld for enum %s when parsing value of property %s"),
                EnumValue, *Enum->GetPathName(), *UnderlyingProperty->GetPathName());
            EnumValue = Enum->GetValueByIndex(0);
        }
        UnderlyingProperty->SetIntPropertyValue(PropertyValuePtr, EnumValue);
    }
}

void FSuziePluginModule::DeserializePropertyValue(const FProperty* Property, void* PropertyValuePtr, const TSharedPtr<FJsonValue>& JsonPropertyValue)
{
    if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
    {
        // We do not actually have to load or look for object pointed by soft object properties, we can just set the value as object path instead
        const FSoftObjectPtr SoftObjectPtr(FSoftObjectPath(JsonPropertyValue->AsString()));
        SoftObjectProperty->SetPropertyValue(PropertyValuePtr, SoftObjectPtr);
    }
    else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
    {
        if (!JsonPropertyValue->IsNull())
        {
            // For all other object properties, we must already have the object pointed at in memory, we will not load any objects here
            UObject* Object = StaticFindObject(ObjectProperty->PropertyClass, nullptr, *JsonPropertyValue->AsString());
            ObjectProperty->SetObjectPropertyValue(PropertyValuePtr, Object);
        }
    }
    else if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
    {
        // Bool properties need special handling because they are represented as JSON booleans
        BoolProperty->SetPropertyValue(PropertyValuePtr, JsonPropertyValue->AsBool());
    }
    else if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property); NumericProperty && !NumericProperty->IsEnum())
    {
        if (JsonPropertyValue->Type == EJson::Number)
        {
            // If this is a floating point property, just set it to the JSON value
            if (NumericProperty->IsFloatingPoint())
            {
                NumericProperty->SetFloatingPointPropertyValue(PropertyValuePtr, JsonPropertyValue->AsNumber());
            }
            else
            {
                // This is an integer property otherwise. Whenever its signed or unsigned does not matter here,
                // because for really large values they will be saved as text and not double
                NumericProperty->SetIntPropertyValue(PropertyValuePtr, (int64)JsonPropertyValue->AsNumber());
            }
        }
        else
        {
            // This is a string representation of the number, let the numeric property parse it
            NumericProperty->SetNumericPropertyValueFromString(PropertyValuePtr, *JsonPropertyValue->AsString());
        }
    }
    else if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
    {
        NameProperty->SetPropertyValue(PropertyValuePtr, FName(*JsonPropertyValue->AsString()));
    }
    else if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
    {
        StrProperty->SetPropertyValue(PropertyValuePtr, JsonPropertyValue->AsString());
    }
    else if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
    {
        // TODO: Implement once dump format is known
        TextProperty->SetPropertyValue(PropertyValuePtr, FText::AsCultureInvariant(JsonPropertyValue->AsString()));
    }
    else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property); EnumProperty && EnumProperty->GetEnum())
    {
        DeserializeEnumValue(EnumProperty->GetUnderlyingProperty(), PropertyValuePtr, EnumProperty->GetEnum(), JsonPropertyValue);
    }
    else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty && ByteProperty->Enum)
    {
        // Non enum byte properties are handled above as FNumericProperty case
        DeserializeEnumValue(ByteProperty, PropertyValuePtr, ByteProperty->Enum, JsonPropertyValue);
    }
    else if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property); StructProperty && StructProperty->Struct)
    {
        // Deserialize nested struct properties payload
        DeserializeStructProperties(StructProperty->Struct, PropertyValuePtr, JsonPropertyValue->AsObject());
    }
    else if (const FFieldPathProperty* FieldPathProperty = CastField<FFieldPathProperty>(Property))
    {
        const TFieldPath<FProperty> FieldPath(*JsonPropertyValue->AsString());
        FieldPathProperty->SetPropertyValue(PropertyValuePtr, FieldPath);
    }
    else if (const FOptionalProperty* OptionalProperty = CastField<FOptionalProperty>(Property))
    {
        // If JSON property value is null, optional property is unset
        if (JsonPropertyValue->Type == EJson::Null)
        {
            OptionalProperty->MarkUnset(PropertyValuePtr);
        }
        else
        {
            // Deserialize the inner property value otherwise
            void* ValuePropertyValuePtr = OptionalProperty->MarkSetAndGetInitializedValuePointerToReplace(PropertyValuePtr);
            DeserializePropertyValue(OptionalProperty->GetValueProperty(), ValuePropertyValuePtr, JsonPropertyValue);
        }
    }
    else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
    {
        const TArray<TSharedPtr<FJsonValue>>& ArrayElementJsonValues = JsonPropertyValue->AsArray();
        FScriptArrayHelper ArrayValueHelper(ArrayProperty, PropertyValuePtr);

        ArrayValueHelper.Resize(ArrayElementJsonValues.Num());
        for (int32 ElementIndex = 0; ElementIndex < ArrayElementJsonValues.Num(); ElementIndex++)
        {
            void* ElementValuePtr = ArrayValueHelper.GetElementPtr(ElementIndex);
            DeserializePropertyValue(ArrayProperty->Inner, ElementValuePtr, ArrayElementJsonValues[ElementIndex]);
        }
    }
    else if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
    {
        const TArray<TSharedPtr<FJsonValue>>& SetElementJsonValues = JsonPropertyValue->AsArray();
        FScriptSetHelper SetValueHelper(SetProperty, PropertyValuePtr);
        
        for (const TSharedPtr<FJsonValue>& ElementJsonValue : SetElementJsonValues)
        {
            const int32 NewElementIndex = SetValueHelper.AddDefaultValue_Invalid_NeedsRehash();
            void* ElementValuePtr = SetValueHelper.GetElementPtr(NewElementIndex);
            DeserializePropertyValue(SetProperty->ElementProp, ElementValuePtr, ElementJsonValue);
        }
        SetValueHelper.Rehash();
    }
    else if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
    {
        const TArray<TSharedPtr<FJsonValue>>& MapPairJsonValues = JsonPropertyValue->AsArray();
        FScriptMapHelper MapValueHelper(MapProperty, PropertyValuePtr);
        
        for (const TSharedPtr<FJsonValue>& ElementJsonValue : MapPairJsonValues)
        {
            const int32 NewPairIndex = MapValueHelper.AddDefaultValue_Invalid_NeedsRehash();
            void* KeyElementPtr = MapValueHelper.GetKeyPtr(NewPairIndex);
            void* ValueElementPtr = MapValueHelper.GetValuePtr(NewPairIndex);

            const TArray<TSharedPtr<FJsonValue>>& PairValue = ElementJsonValue->AsArray();
            if (PairValue.Num() == 2)
            {
                DeserializePropertyValue(MapProperty->KeyProp, KeyElementPtr, PairValue[0]);
                DeserializePropertyValue(MapProperty->ValueProp, ValueElementPtr, PairValue[1]);
            }
        }
        MapValueHelper.Rehash();
    }
}

void FSuziePluginModule::DeserializeStructProperties(const UStruct* Struct, void* StructData, const TSharedPtr<FJsonObject>& PropertyValues)
{
    for (TFieldIterator<FProperty> PropertyIterator(Struct, EFieldIterationFlags::IncludeAll); PropertyIterator; ++PropertyIterator)
    {
        const FProperty* Property = *PropertyIterator;
        if (!PropertyValues->HasField(Property->GetName())) continue;

        const TSharedPtr<FJsonValue> PropertyJsonValue = PropertyValues->Values.FindChecked(Property->GetName());
        if (Property->ArrayDim != 1)
        {
            // Handle static array properties here to avoid special handling in DeserializePropertyValue
            const TArray<TSharedPtr<FJsonValue>>& StaticArrayPropertyJsonValues = PropertyJsonValue->AsArray();
            for (int32 ArrayIndex = 0; ArrayIndex < FMath::Min(Property->ArrayDim, StaticArrayPropertyJsonValues.Num()); ArrayIndex++)
            {
                void* ElementValuePtr = Property->ContainerPtrToValuePtr<void>(StructData, ArrayIndex);
                const TSharedPtr<FJsonValue> ElementJsonValue = StaticArrayPropertyJsonValues[ArrayIndex];
                DeserializePropertyValue(Property, ElementValuePtr, ElementJsonValue);
            }
        }
        else
        {
            // This is a normal non-static-array property that can be serialized through DeserializePropertyValue
            void* PropertyValuePtr = Property->ContainerPtrToValuePtr<void>(StructData);
            DeserializePropertyValue(Property, PropertyValuePtr, PropertyJsonValue);
        }
    }
}

UClass* FSuziePluginModule::GetNativeParentClassForDynamicClass(const UClass* InDynamicClass)
{
    // Find native parent class for this polymorphic class, skipping any generated class parents
    UClass* NativeParentClass = InDynamicClass ? InDynamicClass->GetSuperClass() : nullptr;
    while (NativeParentClass && NativeParentClass->ClassConstructor == &FSuziePluginModule::PolymorphicClassConstructorInvocationHelper)
    {
        NativeParentClass = NativeParentClass->GetSuperClass();
    }
    return NativeParentClass;
}

UClass* FSuziePluginModule::GetDynamicParentClassForBlueprintClass(UClass* InBlueprintClass)
{
    // Find the polymorphic class we are currently constructing, in case this is a derived blueprint class
    UClass* CurrentClass = InBlueprintClass;
    while (CurrentClass->ClassConstructor == &FSuziePluginModule::PolymorphicClassConstructorInvocationHelper && !DynamicClassConstructionData.Contains(CurrentClass))
    {
        CurrentClass = CurrentClass->GetSuperClass();
    }
    return CurrentClass;
}

// Mirrors layout of first 3 members of FObjectInitializer
struct FObjectInitializerAccessStub
{
    UObject* Obj;
    UObject* ObjectArchetype;
    bool bCopyTransientsFromClassDefaults;
};

void FSuziePluginModule::PolymorphicClassConstructorInvocationHelper(const FObjectInitializer& ObjectInitializer)
{
    const UClass* NativeParentClass = GetNativeParentClassForDynamicClass(ObjectInitializer.GetClass());
    const UClass* TopLevelDynamicClass = GetDynamicParentClassForBlueprintClass(ObjectInitializer.GetClass());

    // We must have valid construction data for all dynamic classes
    const FDynamicClassConstructionData* TopLevelClassConstructionData = DynamicClassConstructionData.Find(TopLevelDynamicClass);
    checkf(TopLevelClassConstructionData, TEXT("Failed to find dynamic class construction data for dynamic class %s"), *TopLevelDynamicClass->GetPathName());

    // Run logic necessary for the top level dynamic class object. That includes setting up defautl subobject overrides and the active archetype to use for property copying
    {
        // If no explicit archetype has been provided for this object construction, or archetype is a CDO of the current class, set it to the default object archetype instead
        // This will ensure that correct property values are copied from the CDO for all object properties and subobjects are created using correct templates and not their CDO values
        // This has to be done before we call the parent constructor and create any default subobjects
        if ((ObjectInitializer.GetArchetype() == nullptr || ObjectInitializer.GetArchetype() == ObjectInitializer.GetClass()->ClassDefaultObject) && TopLevelClassConstructionData->DefaultObjectArchetype)
        {
            FObjectInitializerAccessStub* ObjectInitializerAccess = reinterpret_cast<FObjectInitializerAccessStub*>(&ObjectInitializer.Get());
            ObjectInitializerAccess->ObjectArchetype = TopLevelClassConstructionData->DefaultObjectArchetype;
            ObjectInitializerAccess->bCopyTransientsFromClassDefaults = true; // we want to copy the transient property values from archetype as well
        }
        
        // Before we execute the class constructor of our parent native class, apply overrides to subobject types that the parent class might create
        for (const FDynamicObjectConstructionData& SubobjectConstructionData : TopLevelClassConstructionData->DefaultSubobjects)
        {
            // ReSharper disable once CppExpressionWithoutSideEffects
            ObjectInitializer.SetDefaultSubobjectClass(SubobjectConstructionData.ObjectName, SubobjectConstructionData.ObjectClass);
        }
        // Disable creation of certain subobjects that this class does not want to have
        for (const FName& DisabledSubobjectName : TopLevelClassConstructionData->SuppressedDefaultSubobjects)
        {
            // ReSharper disable once CppExpressionWithoutSideEffects
            ObjectInitializer.DoNotCreateDefaultSubobject(DisabledSubobjectName);
        }
        
        // Also apply overrides for nested subobject types. These are very rare but should be handled regardless
        // TODO: We do not handle disabled nested default subobjects currently. Case is extremely rare and nested subobjects are extremely rare themselves, so this can be revised later
        for (const FNestedDefaultSubobjectOverrideData& SubobjectOverrideData : TopLevelClassConstructionData->DefaultSubobjectOverrides)
        {
            // ReSharper disable once CppExpressionWithoutSideEffects
            ObjectInitializer.SetNestedDefaultSubobjectClass(SubobjectOverrideData.SubobjectPath, SubobjectOverrideData.OverridenClass);
        }
    }
    
    // Run the constructor for that parent native class now to get an initialized object of the parent class type and parent default subobjects
    NativeParentClass->ClassConstructor(ObjectInitializer);

    // Gather all dynamic classes that contribute to the object being constructed, starting at the top level one
    TArray<const UClass*, TInlineAllocator<8>> DynamicClassHierarchyTree;
    const UClass* CurrentDynamicClass = TopLevelDynamicClass;
    while (CurrentDynamicClass->ClassConstructor == &FSuziePluginModule::PolymorphicClassConstructorInvocationHelper)
    {
        DynamicClassHierarchyTree.Add(CurrentDynamicClass);
        CurrentDynamicClass = CurrentDynamicClass->GetSuperClass();
    }

    // Run constructors for each dynamic class in reverse order, e.g. from the furthest parent to the top level class
    for (int32 i = DynamicClassHierarchyTree.Num() - 1; i >= 0; i--)
    {
        ExecutePolymorphicClassConstructorFrameForDynamicClass(ObjectInitializer, DynamicClassHierarchyTree[i]);
    }
}

void FSuziePluginModule::ExecutePolymorphicClassConstructorFrameForDynamicClass(const FObjectInitializer& ObjectInitializer, const UClass* DynamicClass)
{
    // We must have valid construction data for all dynamic classes
    const FDynamicClassConstructionData* ClassConstructionData = DynamicClassConstructionData.Find(DynamicClass);
    checkf(ClassConstructionData, TEXT("Failed to find dynamic class construction data for dynamic class %s"), *DynamicClass->GetPathName());

    // Run property initializers for properties defined in this class that need constructor calls
    for (const FProperty* Property : ClassConstructionData->PropertiesToConstruct)
    {
        Property->InitializeValue_InContainer(ObjectInitializer.GetObj());
    }
    
    // Create missing default subobjects for this dynamic class type
    for (const FDynamicObjectConstructionData& SubobjectConstructionData : ClassConstructionData->DefaultSubobjects)
    {
        if (StaticFindObjectFast(SubobjectConstructionData.ObjectClass, ObjectInitializer.GetObj(), SubobjectConstructionData.ObjectName) == nullptr)
        {
            ObjectInitializer.CreateDefaultSubobject(ObjectInitializer.GetObj(),
                SubobjectConstructionData.ObjectName, UObject::StaticClass(), SubobjectConstructionData.ObjectClass,
                true, EnumHasAnyFlags(SubobjectConstructionData.ObjectFlags, RF_Transient));
        }
    }
}

void FSuziePluginModule::CollectNestedDefaultSubobjectTypeOverrides(FDynamicClassGenerationContext& Context, TArray<FName> SubobjectNameStack, const FString& SubobjectPath, TArray<FNestedDefaultSubobjectOverrideData>& OutSubobjectOverrideData)
{
    const TSharedPtr<FJsonObject> ObjectDefinition = Context.GlobalObjectMap->GetObjectField(SubobjectPath);
    checkf(ObjectDefinition.IsValid(), TEXT("Failed to find subobject object by path %s"), *SubobjectPath);
    
    // Parse construction data for this object first. Skip if this is not a subobject
    FDynamicObjectConstructionData ObjectConstructionData;
    if (!ParseObjectConstructionData(Context, SubobjectPath, ObjectConstructionData) || !EnumHasAnyFlags(ObjectConstructionData.ObjectFlags, RF_DefaultSubObject))
    {
        return;
    }
    // Class of the overriden default subobject might not have been finalized yet, in which case we have to finalize it now to have its archetype with correct values
    if (Context.ClassesPendingFinalization.Contains(ObjectConstructionData.ObjectClass))
    {
        FinalizeClass(Context, ObjectConstructionData.ObjectClass);
    }

    // Add the name of this object to the stack. If this is not a top level subobject, add it to the override list
    SubobjectNameStack.Add(ObjectConstructionData.ObjectName);
    if (SubobjectNameStack.Num() > 1)
    {
        OutSubobjectOverrideData.Add({SubobjectNameStack, ObjectConstructionData.ObjectClass});
    }

    // Iterate over children and collect nested default subobject overrides for them
    if (ObjectDefinition->HasTypedField<EJson::Array>(TEXT("children")))
    {
        const TArray<TSharedPtr<FJsonValue>>& Children = ObjectDefinition->GetArrayField(TEXT("children"));
        for (const TSharedPtr<FJsonValue>& ChildJsonValue : Children)
        {
            // CollectNestedDefaultSubobjectTypeOverrides will discard children that are not actually subobjects
            const FString ChildPath = ChildJsonValue->AsString();
            CollectNestedDefaultSubobjectTypeOverrides(Context, SubobjectNameStack, ChildPath, OutSubobjectOverrideData);
        }
    }
}

void FSuziePluginModule::DeserializeObjectAndSubobjectPropertyValuesRecursive(const FDynamicClassGenerationContext& Context, UObject* Object, const TSharedPtr<FJsonObject>& ObjectDefinition)
{
    // Deserialize property values for this object first
    if (ObjectDefinition->HasTypedField<EJson::Object>(TEXT("property_values")))
    {
        const TSharedPtr<FJsonObject> PropertyValues = ObjectDefinition->GetObjectField(TEXT("property_values"));
        DeserializeStructProperties(Object->GetClass(), Object, PropertyValues);
    }

    // Iterate over children and deserialize values for the ones that already exist as default subobjects
    if (ObjectDefinition->HasTypedField<EJson::Array>(TEXT("children")))
    {
        const TArray<TSharedPtr<FJsonValue>>& Children = ObjectDefinition->GetArrayField(TEXT("children"));
        for (const TSharedPtr<FJsonValue>& ChildJsonValue : Children)
        {
            const FString ChildPath = ChildJsonValue->AsString();
            
            // Parse object construction data and check if it is a default subobject
            FDynamicObjectConstructionData ObjectConstructionData;
            if (ParseObjectConstructionData(Context, ChildPath, ObjectConstructionData) && EnumHasAnyFlags(ObjectConstructionData.ObjectFlags, RF_DefaultSubObject))
            {
                const TSharedPtr<FJsonObject> SubobjectDefinition = Context.GlobalObjectMap->GetObjectField(ChildPath);
                UObject* SubobjectInstance = StaticFindObjectFast(ObjectConstructionData.ObjectClass, Object, ObjectConstructionData.ObjectName);

                // If we have a constructed subobject instance, deserialize the properties into that instance
                if (SubobjectDefinition && SubobjectInstance && SubobjectInstance->HasAnyFlags(RF_DefaultSubObject))
                {
                    DeserializeObjectAndSubobjectPropertyValuesRecursive(Context, SubobjectInstance, SubobjectDefinition);   
                }
            }
        }
    }
}

void FSuziePluginModule::FinalizeClass(FDynamicClassGenerationContext& Context, UClass* Class)
{
    // Skip this class if it has already been finalized as a dependency of its child class
    if (!Context.ClassesPendingFinalization.Contains(Class))
    {
        return;
    }

    // Find the definition for the class default object
    const FString ClassDefaultObjectPath = Context.ClassesPendingFinalization.FindAndRemoveChecked(Class);

    // Finalize our parent class first since we require parent class CDO to be populated before CDO for this class can be created
    UClass* ParentClass = Class->GetSuperClass();
    if (ParentClass && Context.ClassesPendingFinalization.Contains(ParentClass))
    {
        FinalizeClass(Context, ParentClass);
    }

    const TSharedPtr<FJsonObject> ClassDefaultObjectDefinition = Context.GlobalObjectMap->GetObjectField(ClassDefaultObjectPath);
    checkf(ClassDefaultObjectDefinition.IsValid(), TEXT("Failed to find default object by path %s"), *ClassDefaultObjectPath);

    // Iterate child objects of the class default object to find default subobjects that we want to construct before we deserialize the data
    FDynamicClassConstructionData& ClassConstructionData = DynamicClassConstructionData.FindOrAdd(Class);
    TSet<FName> CreatedDefaultSubobjects;
    
    const TArray<TSharedPtr<FJsonValue>>& Children = ClassDefaultObjectDefinition->GetArrayField(TEXT("children"));
    for (const TSharedPtr<FJsonValue>& ChildObjectPathValue : Children)
    {
        FString ChildPath = ChildObjectPathValue->AsString();
        FDynamicObjectConstructionData ChildObjectConstructionData;
        if (ParseObjectConstructionData(Context, ChildPath, ChildObjectConstructionData) && EnumHasAnyFlags(ChildObjectConstructionData.ObjectFlags, RF_DefaultSubObject))
        {
            // Class of our default subobject might not have been finalized yet, in which case we have to finalize it now to have its archetype with correct values
            if (Context.ClassesPendingFinalization.Contains(ChildObjectConstructionData.ObjectClass))
            {
                FinalizeClass(Context, ChildObjectConstructionData.ObjectClass);
            }
            ClassConstructionData.DefaultSubobjects.Add(ChildObjectConstructionData);
            CreatedDefaultSubobjects.Add(ChildObjectConstructionData.ObjectName);
            
            // Collect subobject overrides for this subobject
            CollectNestedDefaultSubobjectTypeOverrides(Context, TArray<FName>(), ChildPath, ClassConstructionData.DefaultSubobjectOverrides);
        }
    }

    // Iterate default subobjects of our parent native class. If we have not created one of them, it means it has been explicitly disabled
    // TODO: This does not handle disabled nested default subobjects.
    const UClass* NativeParentClass = GetNativeParentClassForDynamicClass(Class);
    ForEachObjectWithOuter(NativeParentClass->GetDefaultObject(), [&](const UObject* ArchetypeDefaultSubobject)
    {
        if (ArchetypeDefaultSubobject->HasAnyFlags(RF_DefaultSubObject) && !CreatedDefaultSubobjects.Contains(ArchetypeDefaultSubobject->GetFName()))
        {
            ClassConstructionData.SuppressedDefaultSubobjects.Add(ArchetypeDefaultSubobject->GetFName());
        }
    }, false);
    
    // Assemble reference token stream for garbage collector
    Class->AssembleReferenceTokenStream(true);
    // Create class default object now that we have class object construction data
    UObject* ClassDefaultObject = Class->GetDefaultObject(true);

    // Recursively deserialize property values for the default object and its subobjects (and their nested subobjects)
    DeserializeObjectAndSubobjectPropertyValuesRecursive(Context, ClassDefaultObject, ClassDefaultObjectDefinition);

    // Create an archetype by duplicating the CDO. We will use that archetype instead of CDO for priming the instances with correct values
    // Do not create archetypes for NetConnection-derived classes, they have faulty shutdown logic leading to a crash on exit
    if (!Class->IsChildOf<UNetConnection>())
    {
        const FString ArchetypeObjectName = TEXT("InitializationArchetype__") + Class->GetName();
        {
            FScopedAllowAbstractClassAllocation AllowAbstract;
            ClassConstructionData.DefaultObjectArchetype = DuplicateObject(ClassDefaultObject, ClassDefaultObject->GetOuter(), *ArchetypeObjectName);
        }
        ClassConstructionData.DefaultObjectArchetype->ClearFlags(RF_ClassDefaultObject);
        ClassConstructionData.DefaultObjectArchetype->SetFlags(RF_Public | RF_ArchetypeObject | RF_Transactional);
        ClassConstructionData.DefaultObjectArchetype->AddToRoot();
    }
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSuziePluginModule, Suzie);
