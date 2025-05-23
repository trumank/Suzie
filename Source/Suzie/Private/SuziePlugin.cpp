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
    FString JsonClassesPath = FPaths::ProjectContentDir() / TEXT("DynamicClasses");
    
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

        // Create classes
        for (auto It = (*Objects)->Values.CreateConstIterator(); It; ++It)
        {
            FString ObjectPath = It.Key();
            FString Type = It.Value()->AsObject()->GetStringField(TEXT("type"));
            if (Type == "Class")
            {
                UE_LOG(LogSuzie, Display, TEXT("Creating class %s"), *ObjectPath);
                FSuziePluginModule::GetRegisteredClass(*Objects, ObjectPath);
            }
            else if (Type == "ScriptStruct")
            {
                UE_LOG(LogSuzie, Display, TEXT("Creating struct %s"), *ObjectPath);
                FSuziePluginModule::GetStruct(*Objects, ObjectPath);
            }
        }
    }
    
    // After all classes are created, finalize them
    FinalizeAllDynamicClasses();
}

UClass* FSuziePluginModule::GetUnregisteredClass(const TSharedPtr<FJsonObject>& Objects, const FString& ClassPath)
{
    if (UClass* NewClass = FindObject<UClass>(ANY_PACKAGE, *ClassPath)) return NewClass;
    
    const TSharedPtr<FJsonObject> ClassDefinition = Objects->GetObjectField(ClassPath);
    
    const FString ParentClassPath = ClassDefinition->GetStringField(TEXT("super_struct"));
    UClass* ParentClass = GetRegisteredClass(Objects, ParentClassPath);
    if (!ParentClass)
    {
        UE_LOG(LogSuzie, Error, TEXT("Parent class not found: %s"), *ParentClassPath);
        return nullptr;
    }
    
    FString PackageName;
    FString ClassName;
    ParseObjectPath(ClassPath, PackageName, ClassName);

    EClassFlags ClassFlags = CLASS_Intrinsic;

    TSet<FString> FFlags = ParseFlags(ClassDefinition->GetStringField(TEXT("class_flags")));
    if (FFlags.Contains(FString(TEXT("CLASS_Interface")))) ClassFlags |= CLASS_Interface;

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
        RF_Public | RF_Standalone | RF_Transient | RF_MarkAsNative | RF_MarkAsRootSet,
        ParentClass->ClassConstructor,
        ParentClass->ClassVTableHelperCtorCaller,
        MoveTemp(ParentClass->CppClassStaticFunctions));

    //Set super structure and ClassWithin (they are required prior to registering)
    ConstructedClassObject->SetSuperStruct(ParentClass);
    ConstructedClassObject->ClassWithin = UObject::StaticClass();

    //ConstructedClassObject->RegisterDependencies();
    FCppClassTypeInfoStatic TypeInfoStatic = {false};

#if WITH_EDITOR
    //Field with cpp type info only exists in editor, in shipping SetCppTypeInfoStatic is empty
    ConstructedClassObject->SetCppTypeInfoStatic(&TypeInfoStatic);
#endif
    //Register pending object, apply class flags, set static type info and link it
    ConstructedClassObject->RegisterDependencies();
    ConstructedClassObject->DeferredRegister(UClass::StaticClass(), *PackageName, *ClassName);

    PendingConstruction.Add(ConstructedClassObject);
    
    UE_LOG(LogSuzie, Display, TEXT("Created dynamic class: %s"), *ClassName);
    return ConstructedClassObject;
}

UClass* FSuziePluginModule::GetRegisteredClass(const TSharedPtr<FJsonObject>& Objects, const FString& ClassPath)
{
    // Return existing class if exists
    UClass* NewClass = FindObject<UClass>(ANY_PACKAGE, *ClassPath);
    if (NewClass && !PendingConstruction.Contains(NewClass)) return NewClass;

    // Class is not constructed/registered so do so now
    const TSharedPtr<FJsonObject> ClassDefinition = Objects->GetObjectField(ClassPath);

    if (!NewClass) NewClass = GetUnregisteredClass(Objects, ClassPath);

    FString PackageName;
    FString ClassName;
    ParseObjectPath(ClassPath, PackageName, ClassName);
    
    if (!NewClass)
    {
        UE_LOG(LogSuzie, Error, TEXT("Failed to create dynamic class: %s"), *ClassPath);
        return nullptr;
    }

    // Add properties to class
    TArray<TSharedPtr<FJsonValue>> Properties = ClassDefinition->GetArrayField(TEXT("properties"));
    for (auto Prop : Properties)
    {
        FSuziePluginModule::AddPropertyToClass(Objects, NewClass, Prop->AsObject());
    }

    // Add functions to class
    TArray<TSharedPtr<FJsonValue>> Children = ClassDefinition->GetArrayField(TEXT("children"));
    for (auto Child : Children)
    {
        FString ChildPath = Child->AsString();
        TSharedPtr<FJsonObject> ChildObject = Objects->GetObjectField(ChildPath);
        if (ChildObject->GetStringField(TEXT("type")) == "Function")
        {
            FSuziePluginModule::AddFunctionToClass(Objects, NewClass, ChildPath, ChildObject);
        }
    }

    // NewClass has since been constructed so don't do it again (TODO can this happen?)
    if (!PendingConstruction.Contains(NewClass)) return NewClass;
    
    NewClass->SetMetaData(FName("Blueprintable"), TEXT("true"));
    NewClass->SetMetaData(FName("BlueprintType"), TEXT("true"));

    NewClass->Bind();

    NewClass->ClassFlags &= (~CLASS_Intrinsic);
    NewClass->StaticLink(true);
    NewClass->ClassFlags |= CLASS_Intrinsic;
    
    NewClass->SetSparseClassDataStruct(NewClass->GetSparseClassDataArchetypeStruct());
    
    NewClass->RegisterDependencies();

    PendingConstruction.Remove(NewClass);
    PendingDynamicClasses.Add(NewClass);

    return NewClass;
}

UScriptStruct* FSuziePluginModule::GetStruct(const TSharedPtr<FJsonObject>& Objects, const FString& StructPath)
{
    UScriptStruct* NewStruct = FindObject<UScriptStruct>(ANY_PACKAGE, *StructPath);
    if (NewStruct) return NewStruct;
    
    FString PackageName;
    FString ObjectName;
    ParseObjectPath(StructPath, PackageName, ObjectName);

    UPackage* Package = CreatePackage(*PackageName);

    TSharedPtr<FJsonObject> StructJson = Objects->GetObjectField(StructPath);
    
    NewStruct = NewObject<UScriptStruct>(
        Package,
        *ObjectName,
        RF_Public | RF_Standalone | RF_Transient
    );

    //NewStruct->StructFlags = static_cast<EStructFlags>(NewStruct->StructFlags | STRUCT_Native);
    
    TArray<TSharedPtr<FJsonValue>> Properties = StructJson->GetArrayField(TEXT("properties"));
    for (auto Prop : Properties)
    {
        TSharedPtr<FJsonObject> PropertyJson = Prop->AsObject();
        if (FProperty* NewProperty = BuildProperty(Objects, NewStruct, PropertyJson))
        {
            TSet<FString> PropertyFlags = ParseFlags(PropertyJson->GetStringField(TEXT("flags")));

            NewProperty->PropertyFlags |= CPF_BlueprintVisible | CPF_BlueprintAssignable;

            //if (PropertyFlags.Contains(FString(TEXT("CPF_Parm")))) NewProperty->PropertyFlags |= CPF_Parm;
            //if (PropertyFlags.Contains(FString(TEXT("CPF_OutParm")))) NewProperty->PropertyFlags |= CPF_OutParm;
            //if (PropertyFlags.Contains(FString(TEXT("CPF_ReturnParm")))) NewProperty->PropertyFlags |= CPF_ReturnParm;
            //if (PropertyFlags.Contains(FString(TEXT("CPF_ReferenceParm")))) NewProperty->PropertyFlags |= CPF_ReferenceParm;
            
            NewProperty->Next = NewStruct->ChildProperties;
            NewStruct->ChildProperties = NewProperty;
        }
    }

    NewStruct->RegisterDependencies();
    
    NewStruct->SetMetaData(FName("Blueprintable"), TEXT("true"));
    NewStruct->SetMetaData(FName("BlueprintType"), TEXT("true"));
    
    NewStruct->Bind();
    NewStruct->StaticLink(true);
    
    NewStruct->AddToRoot();
    
    UE_LOG(LogSuzie, Display, TEXT("Created struct: %s"), *ObjectName);
    return NewStruct;
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

void FSuziePluginModule::AddPropertyToClass(const TSharedPtr<FJsonObject>& Objects, UClass* Class, const TSharedPtr<FJsonObject>& PropertyJson)
{
    if (FProperty* NewProperty = BuildProperty(Objects, Class, PropertyJson))
    {
        NewProperty->Next = Class->ChildProperties;
        Class->ChildProperties = NewProperty;
        
        UE_LOG(LogSuzie, Display, TEXT("Added property %s to class %s"), *NewProperty->GetName(), *Class->GetName());
    }
}

void FSuziePluginModule::AddFunctionToClass(const TSharedPtr<FJsonObject>& Objects, UClass* Class, FString FunctionPath, const TSharedPtr<FJsonObject>& FunctionJson)
{
    FString PackageName;
    FString ObjectName;
    ParseObjectPath(FunctionPath, PackageName, ObjectName);

    EFunctionFlags FunctionFlags = FUNC_Public | FUNC_BlueprintCallable | FUNC_Native;
    
    TSet<FString> FFlags = ParseFlags(FunctionJson->GetStringField(TEXT("function_flags")));
    if (FFlags.Contains(FString(TEXT("FUNC_BlueprintPure")))) FunctionFlags |= FUNC_BlueprintPure;
    if (FFlags.Contains(FString(TEXT("FUNC_Const")))) FunctionFlags |= FUNC_Const;
    if (FFlags.Contains(FString(TEXT("FUNC_Final")))) FunctionFlags |= FUNC_Final;

    UFunction* NewFunction = NewObject<UFunction>(Class, *ObjectName, RF_Public | RF_Standalone);
    NewFunction->FunctionFlags = FunctionFlags;
    NewFunction->SetNativeFunc(
        [](UObject* Context, FFrame& Stack, void* const Z_Param__Result)
        {
            UE_LOG(LogSuzie, Display, TEXT("Dynamic function called"));
            P_FINISH;
        }
    );

    TArray<TSharedPtr<FJsonValue>> Properties = FunctionJson->GetArrayField(TEXT("properties"));
    for (auto Prop : Properties)
    {
        TSharedPtr<FJsonObject> PropertyJson = Prop->AsObject();
        if (FProperty* NewProperty = BuildProperty(Objects, NewFunction, PropertyJson))
        {
            TSet<FString> PropertyFlags = ParseFlags(PropertyJson->GetStringField(TEXT("flags")));

            NewProperty->PropertyFlags |= CPF_NativeAccessSpecifierPublic;

            if (PropertyFlags.Contains(FString(TEXT("CPF_Parm")))) NewProperty->PropertyFlags |= CPF_Parm;
            if (PropertyFlags.Contains(FString(TEXT("CPF_ConstParm")))) NewProperty->PropertyFlags |= CPF_ConstParm;
            if (PropertyFlags.Contains(FString(TEXT("CPF_OutParm")))) NewProperty->PropertyFlags |= CPF_OutParm;
            if (PropertyFlags.Contains(FString(TEXT("CPF_ReturnParm")))) NewProperty->PropertyFlags |= CPF_ReturnParm;
            if (PropertyFlags.Contains(FString(TEXT("CPF_ReferenceParm")))) NewProperty->PropertyFlags |= CPF_ReferenceParm;
            
            NewProperty->Next = NewFunction->ChildProperties;
            NewFunction->ChildProperties = NewProperty;
            
            UE_LOG(LogSuzie, Display, TEXT("Added function parm %s"), *NewProperty->GetName());
        }
    }

    NewFunction->RegisterDependencies();

    NewFunction->Next = Class->Children;
    Class->Children = NewFunction;
    
    Class->AddNativeFunction(*ObjectName, NewFunction->GetNativeFunc());
    Class->AddFunctionToFunctionMap(NewFunction, FName(*ObjectName));

    NewFunction->Bind();
    NewFunction->StaticLink(true);

    UE_LOG(LogSuzie, Display, TEXT("Added function %s to class %s"), *ObjectName, *Class->GetName());
}

FProperty* FSuziePluginModule::BuildProperty(const TSharedPtr<FJsonObject>& Objects, FFieldVariant Owner, const TSharedPtr<FJsonObject>& PropertyJson)
{
    FString PropertyName = PropertyJson->GetStringField(TEXT("name"));
    FString PropertyType = PropertyJson->GetStringField(TEXT("type"));

    EPropertyFlags PropertyFlags = CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintAssignable;
    
    FProperty* NewProperty = nullptr;
    
    if (PropertyType == TEXT("Object"))
    {
        auto P = new FObjectProperty(Owner, *PropertyName, RF_Public);
        UClass* InnerClass = GetUnregisteredClass(Objects, *PropertyJson->GetStringField(TEXT("class")));
        check(InnerClass != nullptr);
        P->PropertyClass = InnerClass;
        NewProperty = P;
    }
    else if (PropertyType == TEXT("SoftObject"))
    {
        auto P = new FSoftObjectProperty(Owner, *PropertyName, RF_Public);
        UClass* InnerClass = GetUnregisteredClass(Objects, *PropertyJson->GetStringField(TEXT("class")));
        check(InnerClass != nullptr);
        P->PropertyClass = InnerClass;
        NewProperty = P;
    }
    else if (PropertyType == TEXT("WeakObject"))
    {
        auto P = new FWeakObjectProperty(Owner, *PropertyName, RF_Public);
        UClass* InnerClass = GetUnregisteredClass(Objects, *PropertyJson->GetStringField(TEXT("class")));
        check(InnerClass != nullptr);
        P->PropertyClass = InnerClass;
        NewProperty = P;
    }
    else if (PropertyType == TEXT("Struct"))
    {
        auto P = new FStructProperty(Owner, *PropertyName, RF_Public);
        if (UScriptStruct* Struct = GetStruct(Objects, PropertyJson->GetStringField(TEXT("struct"))))
        {
            P->Struct = Struct;
            NewProperty = P;
        }
    }
    else if (PropertyType == TEXT("Array"))
    {
        auto P = new FArrayProperty(Owner, *PropertyName, RF_Public);
        if (FProperty* Inner = BuildProperty(Objects, P, PropertyJson->GetObjectField(TEXT("inner"))))
        {
            P->Inner = Inner;
            NewProperty = P;
        }
    }
    else if (PropertyType == TEXT("Set"))
    {
        auto P = new FSetProperty(Owner, *PropertyName, RF_Public);
        if (FProperty* Inner = BuildProperty(Objects, P, PropertyJson->GetObjectField(TEXT("key_prop"))))
        {
            P->ElementProp = Inner;
            NewProperty = P;
        }
    }
    else if (PropertyType == TEXT("Map"))
    {
        auto P = new FMapProperty(Owner, *PropertyName, RF_Public);
        FProperty* KeyProp = BuildProperty(Objects, P, PropertyJson->GetObjectField(TEXT("key_prop")));
        FProperty* ValueProp = BuildProperty(Objects, P, PropertyJson->GetObjectField(TEXT("value_prop")));
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
    
    if (NewProperty)
    {
        NewProperty->ArrayDim = PropertyJson->GetIntegerField(TEXT("array_dim"));
        NewProperty->PropertyFlags |= PropertyFlags;
    }
    else
    {
        UE_LOG(LogSuzie, Warning, TEXT("Failed to create property of type %s: not supported"), 
               *PropertyType);
    }
    return NewProperty;
}

void FSuziePluginModule::FinalizeAllDynamicClasses()
{
    // Finalize all pending classes
    for (UClass* DynamicClass : PendingDynamicClasses)
    {
        FinalizeClass(DynamicClass);
    }
    
    // Clear the list
    PendingDynamicClasses.Empty();
    
    // Register with editor
    FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
    PropertyModule.NotifyCustomizationModuleChanged();
    
    // Notify asset registry
    //FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    //AssetRegistryModule.Get().ScanPathsSynchronous({DynamicClassPackageName.ToString()});
}

void FSuziePluginModule::FinalizeClass(UClass* Class)
{
    Class->AssembleReferenceTokenStream(true);

    //Make sure default class object is initialized
    Class->GetDefaultObject();
    
    UE_LOG(LogSuzie, Display, TEXT("Finalized class: %s"), *Class->GetName());
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSuziePluginModule, DynamicReflectionPlugin)
