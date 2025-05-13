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
#include "Editor.h"
#include "Framework/Commands/UICommandList.h"
#include "Engine/EngineTypes.h"
#include "PropertyEditorModule.h"
#include "Widgets/Docking/SDockTab.h"
#include "UObject/UObjectAllocator.h"
#include "Settings/SuzieSettings.h"
#include "SuzieStyle.h"
#include "SuzieSettingsUI.h"
#include "SuzieUICommands.h"
#include "LevelEditor.h"
#include "ToolMenus.h"

DEFINE_LOG_CATEGORY(LogSuzie);

static const FName SuzieSettingsTabName(TEXT("SuzieSettings"));

#define LOCTEXT_NAMESPACE "FSuziePluginModule"

void FSuziePluginModule::StartupModule()
{
    // Set up the custom config path for the Suzie plugin
    FString PluginConfigDir = FPaths::ProjectPluginsDir() / TEXT("Suzie/Config/");
    if (!FPaths::DirectoryExists(PluginConfigDir))
    {
        FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*PluginConfigDir);
    }
    FString SuzieConfigFile = GetConfigFilePath();
    GConfig->LoadFile(*SuzieConfigFile);
    
    // Load settings
    Settings = GetMutableDefault<USuzieSettings>();
    check(Settings);

    // Make sure the settings are loaded from config
    Settings->LoadConfig(nullptr, *SuzieConfigFile);
    
    // Initialize style
    FSuzieStyle::Initialize();
    FSuzieStyle::ReloadTextures();
    
    // Register commands
    FSuzieUICommands::Register();
    
    // Initialize command list
    PluginCommands = MakeShareable(new FUICommandList());
    
    PluginCommands->MapAction(
        FSuzieUICommands::Get().OpenSettings,
        FExecuteAction::CreateLambda([]() {
            FGlobalTabmanager::Get()->TryInvokeTab(FName("SuzieSettings"));
        }),
        FCanExecuteAction());
    
    // Register in Level Editor toolbar
    FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
    
    // Add toolbar button extension
    TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
    ToolbarExtender->AddToolBarExtension(
        TEXT("Play"),
        EExtensionHook::After,
        PluginCommands,
        FToolBarExtensionDelegate::CreateLambda([](FToolBarBuilder& Builder) {
            Builder.AddToolBarButton(
                FSuzieUICommands::Get().OpenSettings,
                NAME_None,
                LOCTEXT("SuzieButtonLabel", "Suzie"),
                LOCTEXT("SuzieButtonTooltip", "Open Suzie Settings"),
                FSlateIcon(FSuzieStyle::GetStyleSetName(), "Suzie.PluginIcon")
            );
        }));
    LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);

    // Register settings tab spawner
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(SuzieSettingsTabName,
        FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&) {
            return SNew(SDockTab)
                .TabRole(ETabRole::NomadTab)
                .Label(LOCTEXT("SuzieSettingsTabTitle", "Suzie Settings"))
                [SNew(SSuzieSettingsUI)];
        }))
        .SetDisplayName(LOCTEXT("SuzieSettingsTabTitle", "Suzie Settings"))
        .SetMenuType(ETabSpawnerMenuType::Hidden);

    // Process JSON class definitions
    ProcessAllJsonClassDefinitions();
}



void FSuziePluginModule::ShutdownModule()
{
    // Unregister tab spawner
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(FName("SuzieSettings"));
    
    // Unregister style
    FSuzieStyle::Shutdown();
    
    // Unregister commands
    FSuzieUICommands::Unregister();
}

bool FSuziePluginModule::ProcessJsonFile(const FString& JsonFilePath)
{
    // Read the JSON file
    FString JsonContent;
    if (!FFileHelper::LoadFileToString(JsonContent, *JsonFilePath))
    {
        UE_LOG(LogSuzie, Error, TEXT("Failed to read JSON file: %s"), *JsonFilePath);
        return false;
    }

    // Parse the JSON
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(JsonContent);
    if (!FJsonSerializer::Deserialize(JsonReader, JsonObject) || !JsonObject.IsValid())
    {
        UE_LOG(LogSuzie, Error, TEXT("Failed to parse JSON in file: %s"), *JsonFilePath);
        return false;
    }

    const TSharedPtr<FJsonObject>* Objects;
    if (!JsonObject->TryGetObjectField(TEXT("objects"), Objects))
    {
        UE_LOG(LogSuzie, Error, TEXT("Missing 'objects' map"));
        return false;
    }

    // Create classes from the parsed JSON objects
    return CreateClassesFromJson(*Objects);
}

bool FSuziePluginModule::CreateClassesFromJson(const TSharedPtr<FJsonObject>& Objects)
{
    // Create classes and structs
    for (auto It = Objects.Get()->Values.CreateConstIterator(); It; ++It)
    {
        FString ObjectPath = It.Key();
        FString Type = It.Value()->AsObject()->GetStringField(TEXT("type"));
        if (Type == "Class")
        {
            UE_LOG(LogSuzie, Display, TEXT("Creating class %s"), *ObjectPath);
            GetRegisteredClass(Objects, ObjectPath);
        }
        else if (Type == "ScriptStruct")
        {
            UE_LOG(LogSuzie, Display, TEXT("Creating struct %s"), *ObjectPath);
            GetStruct(Objects, ObjectPath);
        }
    }
    
    return true;
}

void FSuziePluginModule::ProcessAllJsonClassDefinitions()
{
    // Clear any pending classes from previous runs
    PendingDynamicClasses.Empty();
    PendingConstruction.Empty();
    
    // Get the directory path from settings
    FString JsonClassesPath;
    if (Settings)
    {
        // Use the full path provided in settings instead of making it relative to content dir
        JsonClassesPath = Settings->JsonClassesDirectory.Path.TrimStartAndEnd();
    }
    else
    {
        // Default to project content dir if no settings are available
        JsonClassesPath = FPaths::ProjectContentDir() / TEXT("DynamicClasses");
    }
    
    UE_LOG(LogSuzie, Display, TEXT("JSON Classes Path: %s"), *JsonClassesPath);
    
    // Log the status of settings values for diagnostic purposes
    if (Settings)
    {
        UE_LOG(LogSuzie, Display, TEXT("Processing JSONs with the following settings:"));
        UE_LOG(LogSuzie, Display, TEXT("Load All Files: %s"), Settings->bLoadAllFiles ? TEXT("Yes") : TEXT("No"));
        UE_LOG(LogSuzie, Display, TEXT("JSON Files Count: %d"), Settings->JsonFiles.Num());
        
        // Count selected files for logging
        int32 SelectedCount = 0;
        for (const FJsonFileConfig& FileConfig : Settings->JsonFiles)
        {
            if (FileConfig.bSelected)
            {
                SelectedCount++;
                UE_LOG(LogSuzie, Display, TEXT("Selected JSON File: %s"), *FileConfig.FilePath.FilePath);
            }
        }
        
        UE_LOG(LogSuzie, Display, TEXT("Selected Files Count: %d"), SelectedCount);
    }
    
    // Check if directory exists
    if (!FPlatformFileManager::Get().GetPlatformFile().DirectoryExists(*JsonClassesPath))
    {
        UE_LOG(LogSuzie, Warning, TEXT("JSON Classes directory not found: %s"), *JsonClassesPath);
        return;
    }
    
    // Collect JSON files to process
    TArray<FString> JsonFiles;
    
    if (Settings && Settings->bLoadAllFiles)
    {
        // Find all JSON files in the directory
        TArray<FString> AllJsonFiles;
        FPlatformFileManager::Get().GetPlatformFile().FindFiles(AllJsonFiles, *JsonClassesPath, TEXT("json"));
        
        for (const FString& JsonFilePath : AllJsonFiles)
        {
            UE_LOG(LogSuzie, Display, TEXT("Processing JSON class definition: %s"), *JsonFilePath);
            if (ProcessJsonFile(JsonFilePath))
            {
                JsonFiles.Add(JsonFilePath);
            }
        }
        UE_LOG(LogSuzie, Display, TEXT("Found %d JSON class definition files"), JsonFiles.Num());
    }
    else if (Settings && Settings->JsonFiles.Num() > 0)
    {
        // Process the selected JSON files from settings
        int32 ProcessedCount = 0;
        
        for (const FJsonFileConfig& FileConfig : Settings->JsonFiles)
        {
            // Skip files that aren't selected
            if (!FileConfig.bSelected)
            {
                continue;
            }
            
            FString JsonFilePath;
            
            // Get file name for path construction if needed
            FString FileName = FPaths::GetCleanFilename(FileConfig.FilePath.FilePath);
            
            // If it's just a file name without path, combine with JsonClassesPath
            if (FileConfig.FilePath.FilePath == FileName)
            {
                JsonFilePath = FPaths::Combine(JsonClassesPath, FileName);
            }
            else
            {
                // Handle paths properly based on whether they're relative or absolute
                if (FPaths::IsRelative(FileConfig.FilePath.FilePath))
                {
                    // Only append to ProjectContentDir if it's relative
                    JsonFilePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / FileConfig.FilePath.FilePath);
                }
                else
                {
                    // Already absolute path, just use it directly
                    JsonFilePath = FileConfig.FilePath.FilePath;
                }
            }
            
            UE_LOG(LogSuzie, Display, TEXT("Processing JSON class definition: %s"), *JsonFilePath);
            
            // Continue with processing the file
            if (ProcessJsonFile(JsonFilePath))
            {
                JsonFiles.Add(JsonFilePath);
                ProcessedCount++;
            }
        }
        
        UE_LOG(LogSuzie, Display, TEXT("Processing %d selected JSON class definition files"), ProcessedCount);
    }
    else
    {
        UE_LOG(LogSuzie, Display, TEXT("No JSON files selected for processing. Open the Suzie Settings to select files."));
    }
    
    // After all classes are created, finalize them
    FinalizeAllDynamicClasses();
    
    // Open settings tab automatically on first launch if no JSON files are selected
    bool bHasSelectedFiles = false;
    if (Settings && Settings->JsonFiles.Num() > 0)
    {
        for (const FJsonFileConfig& FileConfig : Settings->JsonFiles)
        {
            if (FileConfig.bSelected)
            {
                bHasSelectedFiles = true;
                break;
            }
        }
    }
    
    if (Settings && !Settings->bLoadAllFiles && !bHasSelectedFiles)
    {
        // We need to defer this to avoid trying to open it while the UI is still initializing
        FTimerHandle OpenSettingsTimerHandle;
        GEditor->GetTimerManager()->SetTimer(OpenSettingsTimerHandle, []() {
            FGlobalTabmanager::Get()->TryInvokeTab(FName("SuzieSettings"));
        }, 1.0f, false);
    }
}

UClass* FSuziePluginModule::GetUnregisteredClass(const TSharedPtr<FJsonObject>& Objects, const FString& ClassPath)
{
    if (UClass* NewClass = FindObject<UClass>(nullptr, *ClassPath)) return NewClass;
    
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
    UClass* NewClass = FindObject<UClass>(nullptr, *ClassPath);
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
    UScriptStruct* NewStruct = FindObject<UScriptStruct>(nullptr, *StructPath);
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

FString FSuziePluginModule::GetConfigFilePath() const
{
    FString PluginConfigDir = FPaths::ProjectPluginsDir() / TEXT("Suzie/Config/");
    return PluginConfigDir / TEXT("Suzie.ini");
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

IMPLEMENT_MODULE(FSuziePluginModule, Suzie)
