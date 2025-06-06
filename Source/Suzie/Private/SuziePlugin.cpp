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
    
    // Find all JSON files
    TArray<FString> JsonFiles;
    FPlatformFileManager::Get().GetPlatformFile().FindFiles(JsonFiles, *JsonClassesPath, TEXT("json"));
    
    UE_LOG(LogSuzie, Display, TEXT("Found %d JSON class definition files"), JsonFiles.Num());
    
    // Process each JSON file
    for (const FString& JsonFilePath : JsonFiles)
    {
        UE_LOG(LogSuzie, Display, TEXT("Processing JSON class definition: %s"), *JsonFilePath);
    
        // Read the JSON file
        FString JsonContent;
        if (!FFileHelper::LoadFileToString(JsonContent, *JsonFilePath))
        {
            UE_LOG(LogSuzie, Error, TEXT("Failed to read JSON file: %s"), *JsonFilePath);
            return;
        }
    
        // Parse the JSON
        TSharedPtr<FJsonObject> JsonObject;
        TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(JsonContent);
        if (!FJsonSerializer::Deserialize(JsonReader, JsonObject) || !JsonObject.IsValid())
        {
            UE_LOG(LogSuzie, Error, TEXT("Failed to parse JSON in file: %s"), *JsonFilePath);
            continue;
        }

        const TSharedPtr<FJsonObject>* Objects;
        if (!JsonObject->TryGetObjectField(TEXT("objects"), Objects))
        {
            UE_LOG(LogSuzie, Error, TEXT("Missing 'objects' map"));
            continue;
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
                UE_LOG(LogSuzie, Display, TEXT("Creating class %s"), *ObjectPath);
                FindOrCreateClass(ClassGenerationContext, ObjectPath);
            }
            else if (Type == TEXT("ScriptStruct"))
            {
                UE_LOG(LogSuzie, Display, TEXT("Creating struct %s"), *ObjectPath);
                FindOrCreateScriptStruct(ClassGenerationContext, ObjectPath);
            }
            else if (Type == TEXT("Enum"))
            {
                UE_LOG(LogSuzie, Display, TEXT("Creating enum %s"), *ObjectPath);
                FindOrCreateEnum(ClassGenerationContext, ObjectPath);
            }
            // TODO: Support delegate and multicast delegate properties here (top level functions)
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
        for (UClass* ClassPendingFinalization : ClassGenerationContext.ClassesPendingFinalization)
        {
            FinalizeClass(ClassPendingFinalization);
        }
    }
}

UClass* FSuziePluginModule::FindOrCreateUnregisteredClass(FDynamicClassGenerationContext& Context, const FString& ClassPath)
{
    // Attempt to find an existing class first
    if (UClass* ExistingClass = FindObject<UClass>(nullptr, *ClassPath))
    {
        return ExistingClass;
    }
    
    const TSharedPtr<FJsonObject> ClassDefinition = Context.GlobalObjectMap->GetObjectField(ClassPath);
    
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
    EClassFlags ClassFlags = CLASS_None;
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
        RF_Public | RF_Transient | RF_MarkAsRootSet,
        ParentClass->ClassConstructor,
        ParentClass->ClassVTableHelperCtorCaller,
        MoveTemp(ClassStaticFunctions));

    //Set super structure and ClassWithin (they are required prior to registering)
    ConstructedClassObject->SetSuperStruct(ParentClass);
    ConstructedClassObject->ClassWithin = UObject::StaticClass();

    //Field with cpp type info only exists in editor, in shipping SetCppTypeInfoStatic is empty
    static const FCppClassTypeInfoStatic TypeInfoStatic{false};
    ConstructedClassObject->SetCppTypeInfoStatic(&TypeInfoStatic);
    
    //Register pending object, apply class flags, set static type info and link it
    ConstructedClassObject->RegisterDependencies();
    ConstructedClassObject->DeferredRegister(UClass::StaticClass(), *PackageName, *ClassName);

    Context.ClassesPendingConstruction.Add(ConstructedClassObject, ClassPath);
    
    UE_LOG(LogSuzie, Display, TEXT("Created dynamic class: %s"), *ClassName);
    return ConstructedClassObject;
}

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

    // Add properties to the class
    const TArray<TSharedPtr<FJsonValue>>& Properties = ClassDefinition->GetArrayField(TEXT("properties"));
    for (const TSharedPtr<FJsonValue>& PropertyDescriptor : Properties)
    {
        // We want all properties to be editable, visible and blueprint assignable
        const EPropertyFlags ExtraPropertyFlags = CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintAssignable;
        AddPropertyToStruct(Context, NewClass, PropertyDescriptor->AsObject(), ExtraPropertyFlags);
    }

    // Add functions to the class
    const TArray<TSharedPtr<FJsonValue>>& Children = ClassDefinition->GetArrayField(TEXT("children"));
    for (const TSharedPtr<FJsonValue>& FunctionObjectPathValue : Children)
    {
        FString ChildPath = FunctionObjectPathValue->AsString();
        TSharedPtr<FJsonObject> ChildObject = Context.GlobalObjectMap->GetObjectField(ChildPath);
        if (ChildObject->GetStringField(TEXT("type")) == "Function")
        {
            AddFunctionToClass(Context, NewClass, ChildPath, ChildObject);
        }
    }

    // Mark all dynamic classes as blueprintable and blueprint types, otherwise we will not be able to use them
    NewClass->SetMetaData(FName("Blueprintable"), TEXT("true"));
    NewClass->SetMetaData(FName("BlueprintType"), TEXT("true"));

    // Bind parent class to this class and link properties to calculate the class size
    NewClass->Bind();
    NewClass->StaticLink(true);
    NewClass->SetSparseClassDataStruct(NewClass->GetSparseClassDataArchetypeStruct());

    // Class default object can be created at this point
    Context.ClassesPendingFinalization.Add(NewClass);
    
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

    // Resolve parent struct for this struct before we attempt to create this struct
    UScriptStruct* SuperScriptStruct = nullptr;
    if (StructDefinition->HasField(TEXT("super_struct")))
    {
        const FString ParentStructPath = StructDefinition->GetStringField(TEXT("super_struct"));
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
    UPackage* Package = CreatePackage(*PackageName);
    Package->SetPackageFlags(PKG_CompiledIn);
    
    UScriptStruct* NewStruct = NewObject<UScriptStruct>(Package, *ObjectName, RF_Public | RF_Transient | RF_MarkAsRootSet);

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
    NewStruct->SetMetaData(FName("BlueprintType"), TEXT("true"));

    // Bind the newly created struct and link it to assign property offsets and calculate the size
    NewStruct->Bind();
    NewStruct->PrepareCppStructOps();
    NewStruct->StaticLink(true);
    
    UE_LOG(LogSuzie, Display, TEXT("Created struct: %s"), *ObjectName);

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

    FString PackageName;
    FString ObjectName;
    ParseObjectPath(EnumPath, PackageName, ObjectName);

    // Create a package for the struct or reuse the existing package. Make sure it's marked as Native package
    UPackage* Package = CreatePackage(*PackageName);
    Package->SetPackageFlags(PKG_CompiledIn);
    
    UEnum* NewEnum = NewObject<UEnum>(Package, *ObjectName, RF_Public | RF_Transient | RF_MarkAsRootSet);

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
    NewEnum->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
    
    UE_LOG(LogSuzie, Display, TEXT("Created enum: %s"), *ObjectName);

    return NewEnum;
}

void FSuziePluginModule::ParseObjectPath(const FString& ObjectPath, FString& OutPackageName, FString& OutObjectName)
{
    int32 DotPosition = ObjectPath.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    
    if (DotPosition != INDEX_NONE)
    {
        OutPackageName = ObjectPath.Left(DotPosition);
        OutObjectName = ObjectPath.Mid(DotPosition + 1);
    }
    else
    {
        OutPackageName = TEXT("");
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

void FSuziePluginModule::AddPropertyToStruct(FDynamicClassGenerationContext& Context, UStruct* Struct, const TSharedPtr<FJsonObject>& PropertyJson, const EPropertyFlags ExtraPropertyFlags)
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
        UE_LOG(LogSuzie, Display, TEXT("Added property %s to struct %s"), *NewProperty->GetName(), *Struct->GetName());
    }
}

void FSuziePluginModule::AddFunctionToClass(FDynamicClassGenerationContext& Context, UClass* Class, const FString& FunctionPath, const TSharedPtr<FJsonObject>& FunctionJson, EFunctionFlags ExtraFunctionFlags)
{
    FString PackageName;
    FString ObjectName;
    ParseObjectPath(FunctionPath, PackageName, ObjectName);

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
    
    // Convert struct flag names to the struct flags bitmask
    const TSet<FString> FunctionFlagNames = ParseFlags(FunctionJson->GetStringField(TEXT("function_flags")));
    EFunctionFlags FunctionFlags = ExtraFunctionFlags;
    for (const auto& [FunctionFlagName, FunctionFlagBit] : FunctionFlagNameLookup)
    {
        if (FunctionFlagNames.Contains(FunctionFlagName))
        {
            FunctionFlags |= FunctionFlagBit;
        }
    }

    UFunction* NewFunction = NewObject<UFunction>(Class, *ObjectName, RF_Public | RF_Transient | RF_MarkAsRootSet);
    NewFunction->FunctionFlags |= FunctionFlags;

    // Since this function is not marked as Native, we have to initialize Script bytecode for it
    // Most basic valid kismet bytecode for a function would be EX_Return EX_Nothing EX_EndOfScript, so generate that
    NewFunction->Script.Append({EX_Return, EX_Nothing, EX_EndOfScript});

    // Create function parameter properties (and function return value property)
    TArray<TSharedPtr<FJsonValue>> Properties = FunctionJson->GetArrayField(TEXT("properties"));
    for (const TSharedPtr<FJsonValue>& PropertyDescriptor : Properties)
    {
        AddPropertyToStruct(Context, NewFunction, PropertyDescriptor->AsObject());
    }

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
    Class->AddFunctionToFunctionMap(NewFunction, FName(*ObjectName));

    // Bind the function and calculate property layout and function locals size
    NewFunction->Bind();
    NewFunction->StaticLink(true);

    UE_LOG(LogSuzie, Display, TEXT("Added function %s to class %s"), *ObjectName, *Class->GetName());
}

FProperty* FSuziePluginModule::BuildProperty(FDynamicClassGenerationContext& Context, FFieldVariant Owner, const TSharedPtr<FJsonObject>& PropertyJson, EPropertyFlags ExtraPropertyFlags)
{
    const FString PropertyName = PropertyJson->GetStringField(TEXT("name"));
    const FString PropertyType = PropertyJson->GetStringField(TEXT("type"));

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
    };

    // Convert struct flag names to the struct flags bitmask
    const TSet<FString> PropertyFlagNames = ParseFlags(PropertyJson->GetStringField(TEXT("flags")));
    EPropertyFlags PropertyFlags = ExtraPropertyFlags;
    for (const auto& [PropertyFlagName, PropertyFlagBit] : PropertyFlagNameLookup)
    {
        if (PropertyFlagName.Contains(PropertyFlagName))
        {
            PropertyFlags |= PropertyFlagBit;
        }
    }
    
    
    FProperty* NewProperty = nullptr;
    
    if (PropertyType == TEXT("Object"))
    {
        auto P = new FObjectProperty(Owner, *PropertyName, RF_Public);
        UClass* InnerClass = FindOrCreateUnregisteredClass(Context, *PropertyJson->GetStringField(TEXT("class")));
        check(InnerClass != nullptr);
        P->PropertyClass = InnerClass;
        NewProperty = P;
    }
    else if (PropertyType == TEXT("SoftObject"))
    {
        auto P = new FSoftObjectProperty(Owner, *PropertyName, RF_Public);
        UClass* InnerClass = FindOrCreateUnregisteredClass(Context, *PropertyJson->GetStringField(TEXT("class")));
        check(InnerClass != nullptr);
        P->PropertyClass = InnerClass;
        NewProperty = P;
    }
    else if (PropertyType == TEXT("WeakObject"))
    {
        auto P = new FWeakObjectProperty(Owner, *PropertyName, RF_Public);
        UClass* InnerClass = FindOrCreateUnregisteredClass(Context, *PropertyJson->GetStringField(TEXT("class")));
        check(InnerClass != nullptr);
        P->PropertyClass = InnerClass;
        NewProperty = P;
    }
    else if (PropertyType == TEXT("Struct"))
    {
        auto P = new FStructProperty(Owner, *PropertyName, RF_Public);
        if (UScriptStruct* Struct = FindOrCreateScriptStruct(Context, PropertyJson->GetStringField(TEXT("struct"))))
        {
            P->Struct = Struct;
            NewProperty = P;
        }
    }
    else if (PropertyType == TEXT("Array"))
    {
        auto P = new FArrayProperty(Owner, *PropertyName, RF_Public);
        if (FProperty* Inner = BuildProperty(Context, P, PropertyJson->GetObjectField(TEXT("inner"))))
        {
            P->Inner = Inner;
            NewProperty = P;
        }
    }
    else if (PropertyType == TEXT("Set"))
    {
        auto P = new FSetProperty(Owner, *PropertyName, RF_Public);
        if (FProperty* Inner = BuildProperty(Context, P, PropertyJson->GetObjectField(TEXT("key_prop"))))
        {
            P->ElementProp = Inner;
            NewProperty = P;
        }
    }
    else if (PropertyType == TEXT("Map"))
    {
        auto P = new FMapProperty(Owner, *PropertyName, RF_Public);
        FProperty* KeyProp = BuildProperty(Context, P, PropertyJson->GetObjectField(TEXT("key_prop")));
        FProperty* ValueProp = BuildProperty(Context, P, PropertyJson->GetObjectField(TEXT("value_prop")));
        if (KeyProp && ValueProp)
        {
            P->KeyProp = KeyProp;
            P->ValueProp = ValueProp;
            NewProperty = P;
        }
    }
    else if (PropertyType == TEXT("Bool"))
    {
        NewProperty = new FBoolProperty(Owner, *PropertyName, RF_Public);
    }
    else if (PropertyType == TEXT("Float"))
    {
        NewProperty = new FFloatProperty(Owner, *PropertyName, RF_Public);
    }
    else if (PropertyType == TEXT("Int"))
    {
        NewProperty = new FIntProperty(Owner, *PropertyName, RF_Public);
    }
    else if (PropertyType == TEXT("UInt32"))
    {
        NewProperty = new FUInt32Property(Owner, *PropertyName, RF_Public);
    }
    else if (PropertyType == TEXT("Str"))
    {
        NewProperty = new FStrProperty(Owner, *PropertyName, RF_Public);
    }
    else if (PropertyType == TEXT("Name"))
    {
        NewProperty = new FNameProperty(Owner, *PropertyName, RF_Public);
    }
    else if (PropertyType == TEXT("Text"))
    {
        NewProperty = new FTextProperty(Owner, *PropertyName, RF_Public);
    }
    // TODO: Support delegate and multicast delegate properties here
    // TODO: A whole bunch of properties are missing (double, numerous integral, optional, interface, etc)
    
    if (NewProperty)
    {
        NewProperty->ArrayDim = PropertyJson->GetIntegerField(TEXT("array_dim"));
        NewProperty->PropertyFlags |= PropertyFlags;
    }
    else
    {
        UE_LOG(LogSuzie, Warning, TEXT("Failed to create property of type %s: not supported"), *PropertyType);
    }
    return NewProperty;
}

void FSuziePluginModule::FinalizeClass(UClass* Class)
{
    // Assemble reference token stream for garbage collector
    Class->AssembleReferenceTokenStream(true);

    //Make sure default class object is created
    Class->GetDefaultObject();
    // TODO: Create class default subobjects, deserialize class default object property defaults
    
    UE_LOG(LogSuzie, Display, TEXT("Finalized class: %s"), *Class->GetName());
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSuziePluginModule, DynamicReflectionPlugin)
