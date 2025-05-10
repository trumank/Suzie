#include "SuziePlugin.h"
#include "SuzieUICommands.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

#define LOCTEXT_NAMESPACE "FSuzieMenu"

TSharedRef<SWidget> GenerateSuzieMenu(TSharedRef<FUICommandList> CommandList)
{
    const bool bShouldCloseWindowAfterMenuSelection = true;
    FMenuBuilder MenuBuilder(bShouldCloseWindowAfterMenuSelection, CommandList);

    MenuBuilder.BeginSection("Suzie", LOCTEXT("SuzieToolbarMenu", "Suzie Menu"));
    {
        MenuBuilder.AddMenuEntry(FSuzieUICommands::Get().OpenSettings);
        MenuBuilder.AddMenuEntry(FSuzieUICommands::Get().ReloadJsonFiles);
    }
    MenuBuilder.EndSection();
    
    return MenuBuilder.MakeWidget();
}

#undef LOCTEXT_NAMESPACE
