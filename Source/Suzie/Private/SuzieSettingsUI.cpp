#include "SuzieSettingsUI.h"
#include "Settings/SuzieSettings.h"
#include "SuziePlugin.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/SlateTypes.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "EditorDirectories.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "SuzieSettingsUI"

// Helper method to show notifications
void SSuzieSettingsUI::ShowNotification(const FText& Message, float Duration, bool bSuccess) const
{
    FNotificationInfo Info(Message);
    Info.ExpireDuration = Duration;
    Info.bUseLargeFont = false;
    Info.FadeOutDuration = 0.5f;
    
    // Create and set the notification
    auto NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
    if (NotificationItem.IsValid())
    {
        NotificationItem->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_None);
    }
}

// Helper method to determine file selection state
bool SSuzieSettingsUI::DetermineFileSelectionState(const FString& FilePath, const FString& FileName, 
                               const TMap<FString, bool>& CurrentState)
{
    // Check current session state for exact path match first
    if (CurrentState.Contains(FilePath))
    {
        return CurrentState[FilePath];
    }
    
    // Check settings for exact path match
    for (const FJsonFileConfig& FileConfig : Settings->JsonFiles)
    {
        if (FileConfig.FilePath.FilePath == FilePath)
        {
            return FileConfig.bSelected;
        }
    }
    
    // Default to false if not found
    return false;
}

// Helper to load settings from disk
FString SSuzieSettingsUI::GetAbsolutePath(const FString& Path) const
{
    if (FPaths::IsRelative(Path.TrimStartAndEnd()))
    {
        return FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / Path.TrimStartAndEnd());
    }
    return Path.TrimStartAndEnd();
}

void SSuzieSettingsUI::UpdateDirectoryTextBox(const FString& Path)
{
    if (DirectoryTextBox.IsValid())
    {
        DirectoryTextBox->SetText(FText::FromString(Path));
        DirectoryTextBox->Invalidate(EInvalidateWidgetReason::Layout);
    }
}

void SSuzieSettingsUI::LoadSettingsFromDisk()
{
    // Ensure we have the latest settings from disk
    FSuziePluginModule& SuziePlugin = FModuleManager::GetModuleChecked<FSuziePluginModule>("Suzie");
    FString SuzieConfigFile = SuziePlugin.GetConfigFilePath();
    Settings->LoadConfig(nullptr, *SuzieConfigFile);
    
    // Log basic info about loaded settings
    UE_LOG(LogSuzie, Verbose, TEXT("Loaded settings: LoadAllFiles=%s, JsonFiles=%d"), 
        Settings->bLoadAllFiles ? TEXT("Yes") : TEXT("No"), 
        Settings->JsonFiles.Num());
}

void SSuzieSettingsUI::SaveSettingsToDisk()
{
    FSuziePluginModule& SuziePlugin = FModuleManager::GetModuleChecked<FSuziePluginModule>("Suzie");
    FString SuzieConfigFile = SuziePlugin.GetConfigFilePath();
    Settings->SaveConfig(CPF_Config, *SuzieConfigFile);
    GConfig->Flush(false, *SuzieConfigFile);
    UE_LOG(LogSuzie, Display, TEXT("Saved settings to config file: %s"), *SuzieConfigFile);
}

void SSuzieSettingsUI::Construct(const FArguments& InArgs)
{
    Settings = GetMutableDefault<USuzieSettings>();
    check(Settings);
    
    CurrentDirectory = Settings->JsonClassesDirectory.Path;
    
    ChildSlot
    [
        SNew(SVerticalBox)
        
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(5)
        [
            CreateDirectorySection()
        ]
        
        + SVerticalBox::Slot()
        .FillHeight(1.0f)
        .Padding(5)
        [
            CreateFileSelectionSection()
        ]
        
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(5)
        .HAlign(HAlign_Right)
        [
            SNew(SHorizontalBox)
            
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(5, 0)
            [
                SNew(SButton)
                .Text(LOCTEXT("RefreshButton", "Refresh List"))
                .OnClicked(this, &SSuzieSettingsUI::OnRefreshButtonClicked)
                .ToolTipText(LOCTEXT("RefreshTooltip", "Refresh the list of available JSON files without changing selections or loading files"))
            ]
            
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(5, 0)
            [
                SNew(SButton)
                .Text(LOCTEXT("ReloadButton", "Reset & Reload"))
                .OnClicked(this, &SSuzieSettingsUI::OnReloadButtonClicked)
                .ToolTipText(LOCTEXT("ReloadTooltip", "Reset UI to match previous applied settings and reload those JSON files"))
            ]
            
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(5, 0)
            [
                SNew(SButton)
                .Text(LOCTEXT("ApplyButton", "Apply & Load"))
                .OnClicked(this, &SSuzieSettingsUI::OnApplyButtonClicked)
                .ToolTipText(LOCTEXT("ApplyTooltip", "Save current selection settings and load the selected JSON files"))
            ]
        ]
    ];
    
    // Initialize the JSON file list
    RefreshJsonFileList();
}

TSharedRef<SWidget> SSuzieSettingsUI::CreateDirectorySection()
{
    return SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(5)
        [
            SNew(SVerticalBox)
            
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0, 0, 0, 5)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("JsonDirectoryLabel", "JSON Files Directory"))
                .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
            ]
            
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SHorizontalBox)
                
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .Padding(0, 0, 5, 0)
                [
                    SAssignNew(DirectoryTextBox, SEditableTextBox)
                    .Text(FText::FromString(Settings->JsonClassesDirectory.Path))
                    .OnTextChanged_Lambda([this](const FText& NewText)
                    {
                        OnDirectoryPathChanged(NewText.ToString());
                    })
                ]
                
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("BrowseButton", "Browse..."))
                    .OnClicked(this, &SSuzieSettingsUI::OnBrowseForDirectory)
                ]
            ]
        ];
}

TSharedRef<SWidget> SSuzieSettingsUI::CreateFileSelectionSection()
{
    return SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(5)
        [
            SNew(SVerticalBox)
            
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0, 0, 0, 5)
            [
                SNew(SHorizontalBox)
                
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SAssignNew(SelectAllCheckbox, SCheckBox)
                    .IsChecked(this, &SSuzieSettingsUI::GetSelectAllCheckboxState)
                    .OnCheckStateChanged(this, &SSuzieSettingsUI::OnSelectAllFilesChanged)
                    .IsEnabled_Lambda([this]() { return IsWidgetEnabled("SelectAllCheckbox"); })
                ]
                
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(5, 0, 0, 0)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("SelectAllLabel", "Select/Deselect All"))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
            ]
            
            + SVerticalBox::Slot()
            .FillHeight(1.0f)
            [
                SNew(SBox)
                .HeightOverride(300)
                [
                    SAssignNew(FileListView, SListView<TSharedPtr<FJsonFileEntry>>)
                    .ListItemsSource(&JsonFiles)
                    .OnGenerateRow(this, &SSuzieSettingsUI::OnGenerateFileRow)
                    .SelectionMode(ESelectionMode::None)
                    .HeaderRow
                    (
                        SNew(SHeaderRow)
                        +SHeaderRow::Column("Selected")
                        .DefaultLabel(LOCTEXT("SelectedColumnHeader", ""))
                        .FixedWidth(24)
                        +SHeaderRow::Column("FileName")
                        .DefaultLabel(LOCTEXT("FileNameColumnHeader", "JSON Files"))
                    )
                ]
            ]
            
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0, 5, 0, 0)
            [
                SNew(SHorizontalBox)
                
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SAssignNew(LoadAllFilesCheckbox, SCheckBox)
                    .IsChecked(this, &SSuzieSettingsUI::GetLoadAllFilesCheckboxState)
                    .OnCheckStateChanged(this, &SSuzieSettingsUI::OnLoadAllFilesChanged)
                    .ToolTipText(LOCTEXT("LoadAllFilesTooltip", "When enabled, all JSON files in the directory will be loaded without the need to select them individually"))
                ]
                
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(5, 0, 0, 0)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("LoadAllFilesLabel", "Automatically Load All Files in Directory"))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                    .ToolTipText(LOCTEXT("LoadAllFilesTooltip", "When enabled, all JSON files in the directory will be loaded without the need to select them individually"))
                ]
            ]
        ];
}

void SSuzieSettingsUI::OnDirectoryPathChanged(const FString& NewPath)
{
    // If directory changed, we should reset selection state
    bool bDirectoryChanged = (CurrentDirectory != NewPath);
    
    // Update current directory
    CurrentDirectory = NewPath;
    
    // If directory changed, always clear the current UI state and refresh from settings
    // This prevents carrying over selection state for files with the same name
    if (bDirectoryChanged)
    {
        // Clear current selection state in the UI (JsonFiles will be regenerated during refresh)
        JsonFiles.Empty();
        
        RefreshJsonFileList(true);
    }
    else
    {
        // Same directory, just refresh without using settings as source
        RefreshJsonFileList(false);
    }
}

TSharedPtr<FJsonFileEntry> SSuzieSettingsUI::CreateFileEntry(const FString& FilePath, const FString& FileName, bool bUseSettingsAsSource, const TMap<FString, bool>& CurrentState)
{
    bool bIsSelected = false;
    
    if (bUseSettingsAsSource)
    {
        // Use only settings to determine selection state
        for (const FJsonFileConfig& FileConfig : Settings->JsonFiles)
        {
            FString ConfigFileName = FPaths::GetCleanFilename(FileConfig.FilePath.FilePath);
            if (ConfigFileName == FileName)
            {
                bIsSelected = FileConfig.bSelected;
                break;
            }
        }
    }
    else
    {
        // Use DetermineFileSelectionState to prioritize current UI state over settings
        bIsSelected = DetermineFileSelectionState(FilePath, FileName, CurrentState);
    }
    
    return MakeShareable(new FJsonFileEntry(FilePath, FileName, bIsSelected));
}

void SSuzieSettingsUI::RefreshJsonFileList(bool bUseSettingsAsSource)
{
    // If we should use settings as the source of truth, load them from disk first
    if (bUseSettingsAsSource)
    {
        // Load fresh settings from config
        LoadSettingsFromDisk();
        
        // Alert the user that we're resetting to saved settings
        ShowNotification(LOCTEXT("ResetUIMessage", "UI reset to last applied settings"), 2.0f, false);
        
        // Update directory path from settings
        CurrentDirectory = Settings->JsonClassesDirectory.Path;
        
        // Update directory text box to match
        UpdateDirectoryTextBox(CurrentDirectory);
    }
    
    // Store the current selection state in a map before clearing the list
    TMap<FString, bool> FileSelectionState;
    for (const TSharedPtr<FJsonFileEntry>& FileEntry : JsonFiles)
    {
        FileSelectionState.Add(FileEntry->FilePath, FileEntry->bIsSelected);
    }
    
    JsonFiles.Empty();
    
    // Get absolute path
    FString AbsolutePath = GetAbsolutePath(CurrentDirectory);
    
    // Check if directory exists
    if (!FPlatformFileManager::Get().GetPlatformFile().DirectoryExists(*AbsolutePath))
    {
        UE_LOG(LogSuzie, Warning, TEXT("Directory does not exist: %s"), *AbsolutePath);
        return;
    }
    
    // Find all JSON files in the directory
    TArray<FString> FoundFiles;
    FPlatformFileManager::Get().GetPlatformFile().FindFiles(FoundFiles, *AbsolutePath, TEXT("json"));
    
    // Create entries for each file with the correct selection state
    for (const FString& FoundFile : FoundFiles)
    {
        FString FileName = FPaths::GetCleanFilename(FoundFile);
        TSharedPtr<FJsonFileEntry> Entry = CreateFileEntry(FoundFile, FileName, bUseSettingsAsSource, FileSelectionState);
        JsonFiles.Add(Entry);
    }
    
    // Update the list view if it's been created
    if (FileListView.IsValid())
    {
        FileListView->RebuildList();
    }
    
    // Initialize the SelectAll checkbox based on actual file selection states
    if (SelectAllCheckbox.IsValid())
    {
        ECheckBoxState NewSelectAllState = GetSelectAllCheckboxState();
        SelectAllCheckbox->SetIsChecked(NewSelectAllState);
        
        // Force a complete refresh of this widget and all its children
        TSharedPtr<SWidget> Parent = SelectAllCheckbox->GetParentWidget();
        if (Parent.IsValid())
        {
            Parent->Invalidate(EInvalidateWidgetReason::Paint);
        }
    }
    
    // Initialize the LoadAllFiles checkbox to match the settings value
    if (LoadAllFilesCheckbox.IsValid())
    {
        LoadAllFilesCheckbox->SetIsChecked(Settings->bLoadAllFiles ? ECheckBoxState::Checked : ECheckBoxState::Unchecked);
    }
}

bool SSuzieSettingsUI::IsWidgetEnabled(const FString& WidgetName) const
{
    if (WidgetName == "FileCheckbox" || WidgetName == "SelectAllCheckbox")
    {
        return !Settings->bLoadAllFiles;
    }
    
    return true;
}

void SSuzieSettingsUI::HandleButtonAction(const FString& ButtonType)
{
    if (ButtonType == "Apply")
    {
        // Save current settings with save to config
        UpdateSettings(true);
        
        // Notify the plugin to reprocess the JSON files
        FSuziePluginModule& SuziePlugin = FModuleManager::GetModuleChecked<FSuziePluginModule>("Suzie");
        SuziePlugin.ProcessAllJsonClassDefinitions();
        
        // Display a temporary message to the user
        ShowNotification(LOCTEXT("ApplyMessage", "Settings saved and JSON files loaded"), 3.0f, true);
        
        // Force a complete refresh of the UI to ensure all state is consistent
        RefreshJsonFileList(true);
    }
    else if (ButtonType == "Refresh")
    {
        RefreshJsonFileList(false);
    }
    else if (ButtonType == "Reload")
    {
        // Update UI to reflect the loaded settings FIRST, before actual reload
        RefreshJsonFileList(true);
        
        // Now tell the plugin to reload JSON files using the saved settings
        FSuziePluginModule& SuziePlugin = FModuleManager::GetModuleChecked<FSuziePluginModule>("Suzie");
        SuziePlugin.ProcessAllJsonClassDefinitions();
        
        // Display a temporary message to the user
        ShowNotification(LOCTEXT("ReloadMessage", "Reloaded JSON files from last applied settings"), 3.0f, false);
    }
}

FReply SSuzieSettingsUI::OnBrowseForDirectory()
{
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (DesktopPlatform)
    {
        // Use absolute path for the dialog
        FString DefaultPath = GetAbsolutePath(CurrentDirectory);
        FString SelectedDir;
        
        const FString Title = LOCTEXT("SelectJsonDirectory", "Select JSON Files Directory").ToString();
        const void* ParentWindowWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
        
        if (DesktopPlatform->OpenDirectoryDialog(ParentWindowWindowHandle, Title, DefaultPath, SelectedDir))
        {
            // Keep absolute path when selected
            CurrentDirectory = SelectedDir;
            RefreshJsonFileList(false);
            UpdateDirectoryTextBox(CurrentDirectory);
        }
    }
    
    return FReply::Handled();
}

TSharedRef<ITableRow> SSuzieSettingsUI::OnGenerateFileRow(TSharedPtr<FJsonFileEntry> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
    // Force re-create the row each time to ensure checkbox state is fresh
    return SNew(STableRow<TSharedPtr<FJsonFileEntry>>, OwnerTable)
        [
            SNew(SHorizontalBox)
            
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(5, 0)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([Item]() { return Item->bIsSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged(this, &SSuzieSettingsUI::OnFileCheckboxChanged, Item)
                .IsEnabled_Lambda([this]() { return IsWidgetEnabled("FileCheckbox"); })
            ]
            
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            .VAlign(VAlign_Center)
            .Padding(5, 0)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Item->FileName))
                .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
            ]
        ];
}

void SSuzieSettingsUI::OnFileCheckboxChanged(ECheckBoxState NewState, TSharedPtr<FJsonFileEntry> Item)
{
    // Don't allow changes if bLoadAllFiles is true - should be disabled by IsEnabled_Lambda anyway
    if (Settings->bLoadAllFiles || !Item.IsValid())
    {
        return;
    }
    
    // Update the file entry selection state
    bool bOldState = Item->bIsSelected;
    Item->bIsSelected = (NewState == ECheckBoxState::Checked);
    
    // Only perform additional actions if the state has actually changed
    if (bOldState != Item->bIsSelected)
    {
        // State changed, but no need to log every checkbox change
    }
    
    // Update the Select All checkbox state based on current selections
    if (SelectAllCheckbox.IsValid())
    {
        // Recalculate the state directly to ensure it's accurate
        ECheckBoxState NewSelectAllState = GetSelectAllCheckboxState();
        SelectAllCheckbox->SetIsChecked(NewSelectAllState);
    }
    
    // Force a list rebuild to ensure the visual state is updated
    if (FileListView.IsValid())
    {
        FileListView->RebuildList();
    }
}

ECheckBoxState SSuzieSettingsUI::GetSelectAllCheckboxState() const
{
    return GetCheckboxStateFromCollection<TSharedPtr<FJsonFileEntry>>(
        JsonFiles, 
        [](const TSharedPtr<FJsonFileEntry>& File){ return File->bIsSelected; }
    );
}

void SSuzieSettingsUI::OnSelectAllFilesChanged(ECheckBoxState NewState)
{
    // Don't allow changes if bLoadAllFiles is true - should be disabled by IsEnabled_Lambda anyway
    if (Settings->bLoadAllFiles)
    {
        return;
    }
    
    // Handle the mixed state (Undetermined) by checking all items
    bool bShouldBeSelected = (NewState == ECheckBoxState::Checked || NewState == ECheckBoxState::Undetermined);
    
    // Update all file entries to match the select all state
    bool bAnyChanged = false;
    for (TSharedPtr<FJsonFileEntry>& File : JsonFiles)
    {
        // Update selection state if needed
        if (File->bIsSelected != bShouldBeSelected)
        {
            File->bIsSelected = bShouldBeSelected;
            bAnyChanged = true;
        }
    }
    
    // Update the checkbox state to match the new selection state
    if (SelectAllCheckbox.IsValid())
    {
        SelectAllCheckbox->SetIsChecked(bShouldBeSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked);
    }
    
    // Refresh the list view to update checkbox visuals - force a full redraw
    if (FileListView.IsValid() && bAnyChanged)
    {
        FileListView->RebuildList();
    }
}

ECheckBoxState SSuzieSettingsUI::GetLoadAllFilesCheckboxState() const
{
    if (!Settings) return ECheckBoxState::Unchecked;
    
    return Settings->bLoadAllFiles ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SSuzieSettingsUI::OnLoadAllFilesChanged(ECheckBoxState NewState)
{
    // Update the setting value (this doesn't affect the UI state of the file list)
    bool bOldValue = Settings->bLoadAllFiles;
    Settings->bLoadAllFiles = (NewState == ECheckBoxState::Checked);
    
    // Value might have changed state, but no need to log this UI event
    
    // Refresh the list to update the enabled/disabled state of checkboxes
    if (FileListView.IsValid())
    {
        FileListView->RebuildList();
    }
}

void SSuzieSettingsUI::UpdateSettings(bool bSaveToConfig)
{
    // Update directory path
    Settings->JsonClassesDirectory.Path = CurrentDirectory;
    
    // Clear existing JSON file records
    Settings->JsonFiles.Empty();
    
    // Create a list of all files with their selection state
    int32 SelectedCount = 0;
    for (TSharedPtr<FJsonFileEntry> File : JsonFiles)
    {
        // Create file config entry
        FJsonFileConfig FileConfig;
        FileConfig.FilePath.FilePath = File->FilePath;
        FileConfig.bSelected = File->bIsSelected;
        
        // Add to the settings
        Settings->JsonFiles.Add(FileConfig);
        
        // Count selected files for logging
        if (File->bIsSelected)
        {
            SelectedCount++;
        }
    }
    
    UE_LOG(LogSuzie, Verbose, TEXT("Updated settings with %d selected files out of %d total"), 
        SelectedCount, Settings->JsonFiles.Num());
    
    // Save to disk if requested
    if (bSaveToConfig)
    {
        SaveSettingsToDisk();
    }
}

FReply SSuzieSettingsUI::OnApplyButtonClicked()
{
    HandleButtonAction("Apply");
    return FReply::Handled();
}

FReply SSuzieSettingsUI::OnRefreshButtonClicked()
{
    HandleButtonAction("Refresh");
    return FReply::Handled();
}

FReply SSuzieSettingsUI::OnReloadButtonClicked()
{
    HandleButtonAction("Reload");
    return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
