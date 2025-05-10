#include "SuzieUICommands.h"

#define LOCTEXT_NAMESPACE "FSuzieUICommands"

void FSuzieUICommands::RegisterCommands()
{
    UI_COMMAND(OpenSettings, "Suzie", "Open Suzie plugin settings", EUserInterfaceActionType::Button, FInputChord());
    UI_COMMAND(ReloadJsonFiles, "Reload JSON Files", "Reload JSON class definitions", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
