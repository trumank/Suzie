#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SuzieSettings.generated.h"

/**
 * Configuration for a single JSON file
 */
USTRUCT()
struct SUZIE_API FJsonFileConfig
{
    GENERATED_BODY()
    
    // Path to the JSON file
    UPROPERTY(config)
    FFilePath FilePath;
    
    // Whether this file is selected for loading
    UPROPERTY(config)
    bool bSelected = false;
    
    FJsonFileConfig() : bSelected(false) {}
    
    FJsonFileConfig(const FString& InPath, bool bInSelected = false)
    {
        FilePath.FilePath = InPath;
        bSelected = bInSelected;
    }
};

/**
 * Settings for the Suzie plugin.
 */
UCLASS(config = Suzie, defaultconfig, meta = (DisplayName = "Suzie Plugin"))
class SUZIE_API USuzieSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    USuzieSettings();

    // Directory to search for JSON class definition files
    UPROPERTY(config, EditAnywhere, Category = "JSON Import", meta = (DisplayName = "JSON Classes Directory", ContentDir))
    FDirectoryPath JsonClassesDirectory;

    // Array of JSON files with selection state
    UPROPERTY(config)
    TArray<FJsonFileConfig> JsonFiles;

    // Whether to load all JSON files in the directory
    UPROPERTY(config, EditAnywhere, Category = "JSON Import", meta = (DisplayName = "Load All Files in Directory"))
    bool bLoadAllFiles;

    //~ Begin UDeveloperSettings Interface
    virtual FName GetCategoryName() const override;
    virtual FText GetSectionText() const override;
#if WITH_EDITOR
    virtual FText GetSectionDescription() const override;
#endif
    //~ End UDeveloperSettings Interface
};
