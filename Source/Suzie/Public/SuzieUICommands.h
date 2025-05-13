#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "SuzieStyle.h"

/**
 * UI Commands for the Suzie plugin
 */
class FSuzieUICommands : public TCommands<FSuzieUICommands>
{
public:
    /** Constructor */
    FSuzieUICommands()
        : TCommands<FSuzieUICommands>("Suzie", NSLOCTEXT("Context", "SuzieCommands", "Suzie Plugin"), NAME_None, FSuzieStyle::GetStyleSetName())
    {}

    /** Register all commands */
    virtual void RegisterCommands() override;

    /** Command to open the settings UI */
    TSharedPtr<FUICommandInfo> OpenSettings;
    
    /** Command to reload JSON class definitions */
    TSharedPtr<FUICommandInfo> ReloadJsonFiles;
};
