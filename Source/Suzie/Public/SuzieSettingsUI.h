#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Input/SCheckBox.h"

class USuzieSettings;

struct FJsonFileEntry
{
    FString FilePath;
    FString FileName;
    bool bIsSelected;
    
    FJsonFileEntry(const FString& InPath, const FString& InName, bool bInSelected = false)
        : FilePath(InPath), FileName(InName), bIsSelected(bInSelected)
    {
    }
};

class SSuzieSettingsUI : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSuzieSettingsUI)
    {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    TSharedRef<SWidget> CreateDirectorySection();
    TSharedRef<SWidget> CreateFileSelectionSection();
    
    // Helper method to show notifications
    void ShowNotification(const FText& Message, float Duration = 3.0f, bool bSuccess = false) const;
    
    // Generic helper for checkbox state determination
    template<typename T>
    ECheckBoxState GetCheckboxStateFromCollection(const TArray<T>& Collection, TFunction<bool(const T&)> Predicate) const
    {
        if (Collection.Num() == 0) 
            return ECheckBoxState::Unchecked;
        
        bool bAllChecked = true;
        bool bAnyChecked = false;
        
        for (const T& Item : Collection)
        {
            bool bChecked = Predicate(Item);
            bAllChecked &= bChecked;
            bAnyChecked |= bChecked;
        }
        
        if (bAllChecked) return ECheckBoxState::Checked;
        if (bAnyChecked) return ECheckBoxState::Undetermined;
        return ECheckBoxState::Unchecked;
    }
    
    // Helper method to determine file selection state
    bool DetermineFileSelectionState(const FString& FilePath, const FString& FileName, 
                                  const TMap<FString, bool>& CurrentState);
    
    // Settings management
    void LoadSettingsFromDisk();
    void SaveSettingsToDisk();
    
    void OnDirectoryPathChanged(const FString& NewPath);
    void RefreshJsonFileList(bool bUseSettingsAsSource = false);
    FReply OnBrowseForDirectory();
    FString GetAbsolutePath(const FString& Path) const;
    void UpdateDirectoryTextBox(const FString& Path);
    TSharedPtr<FJsonFileEntry> CreateFileEntry(const FString& FilePath, const FString& FileName, bool bUseSettingsAsSource, const TMap<FString, bool>& CurrentState);
    
    TSharedRef<ITableRow> OnGenerateFileRow(TSharedPtr<FJsonFileEntry> Item, const TSharedRef<STableViewBase>& OwnerTable);
    void OnFileCheckboxChanged(ECheckBoxState NewState, TSharedPtr<FJsonFileEntry> Item);
    ECheckBoxState GetSelectAllCheckboxState() const;
    void OnSelectAllFilesChanged(ECheckBoxState NewState);
    ECheckBoxState GetLoadAllFilesCheckboxState() const;
    void OnLoadAllFilesChanged(ECheckBoxState NewState);
    // Update settings with optional save to config
    void UpdateSettings(bool bSaveToConfig = true);
    
    // Widget state management
    bool IsWidgetEnabled(const FString& WidgetName) const;
    void HandleButtonAction(const FString& ButtonType);
    
    FReply OnApplyButtonClicked();
    FReply OnRefreshButtonClicked();
    FReply OnReloadButtonClicked();

    TSharedPtr<SCheckBox> SelectAllCheckbox;
    TSharedPtr<SCheckBox> LoadAllFilesCheckbox;
    TSharedPtr<SEditableTextBox> DirectoryTextBox;
    TSharedPtr<SListView<TSharedPtr<FJsonFileEntry>>> FileListView;
    TArray<TSharedPtr<FJsonFileEntry>> JsonFiles;
    USuzieSettings* Settings = nullptr;
    FString CurrentDirectory;
};
