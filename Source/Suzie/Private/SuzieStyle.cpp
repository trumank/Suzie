#include "SuzieStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/SlateStyleRegistry.h"
#include "Interfaces/IPluginManager.h"

TSharedPtr<FSlateStyleSet> FSuzieStyle::StyleSet = nullptr;

void FSuzieStyle::Initialize()
{
    if (!StyleSet.IsValid())
    {
        StyleSet = CreateSlateStyleSet();
        FSlateStyleRegistry::RegisterSlateStyle(*StyleSet.Get());
    }
}

void FSuzieStyle::Shutdown()
{
    if (StyleSet.IsValid())
    {
        FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet.Get());
        ensure(StyleSet.IsUnique());
        StyleSet.Reset();
    }
}

void FSuzieStyle::ReloadTextures()
{
    if (FSlateApplication::IsInitialized())
    {
        FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
    }
}

FName FSuzieStyle::GetStyleSetName()
{
    static FName StyleSetName(TEXT("SuzieStyle"));
    return StyleSetName;
}

TSharedPtr<FSlateStyleSet> FSuzieStyle::Get()
{
    return StyleSet;
}

TSharedRef<FSlateStyleSet> FSuzieStyle::CreateSlateStyleSet()
{
    TSharedRef<FSlateStyleSet> Style = MakeShareable(new FSlateStyleSet(GetStyleSetName()));
    Style->SetContentRoot(IPluginManager::Get().FindPlugin(TEXT("Suzie"))->GetBaseDir() / TEXT("Resources"));

    const FVector2D Icon16x16(16.0f, 16.0f);
    const FVector2D Icon20x20(20.0f, 20.0f);
    const FVector2D Icon40x40(40.0f, 40.0f);
    
    Style->Set("Suzie.PluginIcon", new FSlateImageBrush(Style->RootToContentDir(TEXT("suzie_40"), TEXT(".png")), Icon40x40));

    return Style;
}
