#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"
#include "Styling/AppStyle.h"

/**
 * Style class for the Suzie plugin
 */
class FSuzieStyle
{
public:
    /** Initialize the style */
    static void Initialize();

    /** Shutdown the style */
    static void Shutdown();
    
    /** Reload style textures */
    static void ReloadTextures();

    /** Get the style set */
    static TSharedPtr<FSlateStyleSet> Get();

    /** Get the style set name */
    static FName GetStyleSetName();

private:
    /** The style set instance */
    static TSharedPtr<FSlateStyleSet> StyleSet;

    /** Create the slate style set */
    static TSharedRef<FSlateStyleSet> CreateSlateStyleSet();
};
