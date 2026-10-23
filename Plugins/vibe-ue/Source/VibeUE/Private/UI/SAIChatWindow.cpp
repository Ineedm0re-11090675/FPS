// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "UI/SAIChatWindow.h"
#include "Chat/AIChatCommands.h"
#include "Core/ToolRegistry.h"
#include "MCP/MCPServer.h"
#include "Utils/VibeUEPaths.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY(LogAIChatWindow);

TWeakPtr<SWindow> SAIChatWindow::WindowInstance;
TSharedPtr<SAIChatWindow> SAIChatWindow::WidgetInstance;

namespace VibeUEColors
{
	const FLinearColor Background(0.05f, 0.05f, 0.08f, 1.0f);
	const FLinearColor BackgroundLight(0.08f, 0.08f, 0.12f, 1.0f);
	const FLinearColor BackgroundCard(0.10f, 0.10f, 0.14f, 1.0f);
	const FLinearColor Border(0.2f, 0.2f, 0.25f, 1.0f);
	const FLinearColor Cyan(0.0f, 0.9f, 0.9f, 1.0f);
	const FLinearColor Green(0.2f, 0.8f, 0.4f, 1.0f);
	const FLinearColor Orange(0.85f, 0.55f, 0.15f, 1.0f);
	const FLinearColor Red(0.9f, 0.25f, 0.25f, 1.0f);
	const FLinearColor TextPrimary(0.78f, 0.78f, 0.82f, 1.0f);
	const FLinearColor TextSecondary(0.55f, 0.55f, 0.60f, 1.0f);
	const FLinearColor TextMuted(0.38f, 0.38f, 0.42f, 1.0f);
}

namespace
{
	enum class EPackageImpactKind : uint8
	{
		Level,
		Widget,
		Niagara,
		Material,
		Blueprint,
		DataAsset,
		Animation,
		Other
	};

	EPackageImpactKind ClassifyPackage(const FString& PackageName)
	{
		const FString BaseName = FPaths::GetBaseFilename(PackageName);
		if (PackageName.Contains(TEXT("/Maps/")) || PackageName.Contains(TEXT("/Map/")) || BaseName.StartsWith(TEXT("L_")) || BaseName.StartsWith(TEXT("LVL_")))
		{
			return EPackageImpactKind::Level;
		}
		if (BaseName.StartsWith(TEXT("WBP_")) || PackageName.Contains(TEXT("/UI/")) || PackageName.Contains(TEXT("/Widgets/")))
		{
			return EPackageImpactKind::Widget;
		}
		if (BaseName.StartsWith(TEXT("NS_")) || BaseName.StartsWith(TEXT("NE_")) || PackageName.Contains(TEXT("/Niagara/")) || PackageName.Contains(TEXT("/VFX/")))
		{
			return EPackageImpactKind::Niagara;
		}
		if (BaseName.StartsWith(TEXT("M_")) || BaseName.StartsWith(TEXT("MI_")) || PackageName.Contains(TEXT("/Materials/")))
		{
			return EPackageImpactKind::Material;
		}
		if (BaseName.StartsWith(TEXT("DA_")) || PackageName.Contains(TEXT("/Data/")) || PackageName.Contains(TEXT("/DataAssets/")))
		{
			return EPackageImpactKind::DataAsset;
		}
		if (BaseName.StartsWith(TEXT("ABP_")) || BaseName.StartsWith(TEXT("A_")) || PackageName.Contains(TEXT("/Animation/")) || PackageName.Contains(TEXT("/Anim/")))
		{
			return EPackageImpactKind::Animation;
		}
		if (BaseName.StartsWith(TEXT("BP_")) || PackageName.Contains(TEXT("/Blueprints/")))
		{
			return EPackageImpactKind::Blueprint;
		}
		return EPackageImpactKind::Other;
	}

	FString GetPackageKindLabel(EPackageImpactKind Kind)
	{
		switch (Kind)
		{
		case EPackageImpactKind::Level:
			return TEXT("Level");
		case EPackageImpactKind::Widget:
			return TEXT("Widget");
		case EPackageImpactKind::Niagara:
			return TEXT("Niagara");
		case EPackageImpactKind::Material:
			return TEXT("Material");
		case EPackageImpactKind::Blueprint:
			return TEXT("Blueprint");
		case EPackageImpactKind::DataAsset:
			return TEXT("DataAsset");
		case EPackageImpactKind::Animation:
			return TEXT("Animation");
		default:
			return TEXT("Asset");
		}
	}

	FString DescribePackage(const FString& PackageName)
	{
		return FString::Printf(TEXT("%s %s"), *GetPackageKindLabel(ClassifyPackage(PackageName)), *PackageName);
	}

	FString NormalizeImpactAssetPath(const FString& AssetPath)
	{
		FString Normalized = AssetPath;
		int32 DotIndex = INDEX_NONE;
		if (Normalized.FindLastChar(TEXT('.'), DotIndex))
		{
			const FString CandidatePackagePath = Normalized.Left(DotIndex);
			if (CandidatePackagePath.StartsWith(TEXT("/")))
			{
				Normalized = CandidatePackagePath;
			}
		}
		return Normalized;
	}

	FString DescribeAffectedAsset(const FString& AssetPath)
	{
		return DescribePackage(NormalizeImpactAssetPath(AssetPath));
	}

	FString FormatToolRecordLabel(const FMCPToolCallRecord& Record)
	{
		return Record.ActionName.IsEmpty()
			? Record.ToolName
			: FString::Printf(TEXT("%s:%s"), *Record.ToolName, *Record.ActionName);
	}

	FString JoinLimitedList(const TArray<FString>& Values, int32 MaxToShow = 4)
	{
		if (Values.Num() <= MaxToShow)
		{
			return FString::Join(Values, TEXT(" | "));
		}

		TArray<FString> LimitedValues;
		LimitedValues.Reserve(MaxToShow + 1);
		for (int32 Index = 0; Index < MaxToShow; ++Index)
		{
			LimitedValues.Add(Values[Index]);
		}
		LimitedValues.Add(FString::Printf(TEXT("+%d more"), Values.Num() - MaxToShow));
		return FString::Join(LimitedValues, TEXT(" | "));
	}

	void AddUniqueHint(TArray<FString>& Hints, const FString& Hint)
	{
		if (!Hint.IsEmpty() && !Hints.Contains(Hint))
		{
			Hints.Add(Hint);
		}
	}

	TArray<FString> BuildVerificationHints(const FMCPToolCallRecord& Record)
	{
		TArray<FString> Hints = Record.VerificationHints;

		if (Record.ActorCountDelta > 0)
		{
			AddUniqueHint(Hints, FString::Printf(TEXT("Open the current level and confirm %d actor addition(s) in World Outliner/viewport"), Record.ActorCountDelta));
		}
		else if (Record.ActorCountDelta < 0)
		{
			AddUniqueHint(Hints, FString::Printf(TEXT("Open the current level and confirm %d actor removal(s) in World Outliner/viewport"), FMath::Abs(Record.ActorCountDelta)));
		}

		for (const FString& PackageName : Record.NewDirtyPackages)
		{
			switch (ClassifyPackage(PackageName))
			{
			case EPackageImpactKind::Level:
				AddUniqueHint(Hints, TEXT("Review dirty levels for actor placement, transforms, lighting, and save state"));
				break;
			case EPackageImpactKind::Widget:
				AddUniqueHint(Hints, TEXT("Open dirty Widget Blueprints and verify hierarchy, bindings, viewmodels, and designer layout"));
				break;
			case EPackageImpactKind::Niagara:
				AddUniqueHint(Hints, TEXT("Open dirty Niagara assets and verify emitters, spawn/update params, DI bindings, and preview output"));
				break;
			case EPackageImpactKind::Material:
				AddUniqueHint(Hints, TEXT("Open dirty Material assets and verify node graph links, parameter defaults, and preview"));
				break;
			case EPackageImpactKind::Blueprint:
				AddUniqueHint(Hints, TEXT("Open dirty Blueprints, compile them, and verify graph/default changes"));
				break;
			case EPackageImpactKind::DataAsset:
				AddUniqueHint(Hints, TEXT("Open dirty data assets and verify edited property values in Details"));
				break;
			case EPackageImpactKind::Animation:
				AddUniqueHint(Hints, TEXT("Open dirty animation assets and verify sequence/montage settings or generated content"));
				break;
			default:
				AddUniqueHint(Hints, TEXT("Inspect dirty assets in Content Browser before saving"));
				break;
			}
		}

		return Hints;
	}

	FString BuildFriendlyErrorHint(const FMCPToolCallRecord& Record)
	{
		const FString ErrorCode = Record.ErrorCode.ToLower();
		if (ErrorCode == TEXT("missing_parameters") ||
			ErrorCode == TEXT("missing_widget_path") ||
			ErrorCode == TEXT("missing_component_name") ||
			ErrorCode == TEXT("missing_binding_index"))
		{
			return TEXT("The tool call arguments were incomplete; refresh schemas or inspect the required fields before retrying");
		}
		if (ErrorCode == TEXT("component_not_found") ||
			ErrorCode == TEXT("property_not_found") ||
			ErrorCode == TEXT("remove_component_failed"))
		{
			return TEXT("Run describe/list_components first, then retry using the exact widget/component/property names");
		}
		if (ErrorCode == TEXT("add_component_failed"))
		{
			return TEXT("Inspect valid widget types and parent panel names with search_types and describe before retrying");
		}
		if (ErrorCode == TEXT("add_view_model_failed") || ErrorCode == TEXT("add_binding_failed"))
		{
			return TEXT("Inspect ViewModel registration/binding targets in the widget first, then retry with exact names");
		}
		if (ErrorCode == TEXT("tool_timeout"))
		{
			return TEXT("The game thread did not finish in time; inspect Output Log / Recent MCP events and retry");
		}

		const FString Error = Record.ErrorSummary.ToLower();
		if (Error.IsEmpty())
		{
			return FString();
		}
		if (Error.Contains(TEXT("world_not_ready")) || Error.Contains(TEXT("level finishes loading")) || Error.Contains(TEXT("unreal engine loading")))
		{
			return TEXT("Editor world is still loading; wait a few seconds and retry the same request");
		}
		if (Error.Contains(TEXT("tool_disabled")) || Error.Contains(TEXT("is disabled")))
		{
			return TEXT("Re-enable the tool from the Tools panel, then retry from Codex/Cursor");
		}
		if (Error.Contains(TEXT("missing required parameter")))
		{
			return TEXT("The MCP client sent incomplete tool arguments; retry after refreshing tool schemas");
		}
		if (Error.Contains(TEXT("unknown tool")) || Error.Contains(TEXT("not found")))
		{
			return TEXT("The client tool list is stale; reconnect or refresh MCP tools and retry");
		}
		if (Error.Contains(TEXT("timeout")))
		{
			return TEXT("The tool stalled or took too long; inspect Output Log / Recent MCP events and retry");
		}
		if (Error.Contains(TEXT("python")))
		{
			return TEXT("Inspect the Python error, then validate the touched asset/level manually before retrying");
		}
		return TEXT("Inspect Recent MCP events and Output Log for the full failure context");
	}

	int32 GetActiveClientCount(const FMCPServerDiagnosticsSnapshot& Snapshot)
	{
		return FMath::Max(Snapshot.ActiveSSEConnections, Snapshot.ActiveSessions);
	}

	void CountToolCallStates(const TArray<FMCPToolCallRecord>& Records, int32& OutRunning, int32& OutSucceeded, int32& OutFailed)
	{
		OutRunning = 0;
		OutSucceeded = 0;
		OutFailed = 0;

		for (const FMCPToolCallRecord& Record : Records)
		{
			if (Record.bInProgress)
			{
				++OutRunning;
			}
			else if (Record.bIsError)
			{
				++OutFailed;
			}
			else
			{
				++OutSucceeded;
			}
		}
	}

	int32 SecondsAgo(const FDateTime& WhenUtc)
	{
		if (WhenUtc.GetTicks() <= 0)
		{
			return -1;
		}
		return FMath::Max(0, static_cast<int32>((FDateTime::UtcNow() - WhenUtc).GetTotalSeconds()));
	}
}

void SAIChatWindow::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SBorder)
		.BorderBackgroundColor(VibeUEColors::Background)
		.Padding(0)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderBackgroundColor(VibeUEColors::BackgroundLight)
				.Padding(8)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.Padding(0, 0, 8, 0)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("VibeUE MCP Dashboard")))
						.ColorAndOpacity(FSlateColor(VibeUEColors::TextPrimary))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0, 0, 12, 0)
					[
						SAssignNew(ToolsCountText, STextBlock)
						.Text(this, &SAIChatWindow::GetMCPDashboardToolsCountText)
						.ColorAndOpacity(FSlateColor(VibeUEColors::Cyan))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0, 0, 12, 0)
					[
						SAssignNew(MCPStatusText, STextBlock)
						.Text(this, &SAIChatWindow::GetMCPStatusText)
						.ToolTipText(this, &SAIChatWindow::GetMCPStatusTooltipText)
						.ColorAndOpacity(this, &SAIChatWindow::GetMCPStatusColor)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0, 0, 4, 0)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ContentPadding(FMargin(4))
						.ToolTipText(FText::FromString(TEXT("Tool Manager")))
						.OnClicked(this, &SAIChatWindow::OnToolsClicked)
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Icons.Package"))
							.ColorAndOpacity(FSlateColor(VibeUEColors::TextSecondary))
							.DesiredSizeOverride(FVector2D(16, 16))
						]
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ContentPadding(FMargin(4))
						.ToolTipText(FText::FromString(TEXT("MCP Server Settings")))
						.OnClicked(this, &SAIChatWindow::OnSettingsClicked)
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Icons.Toolbar.Settings"))
							.ColorAndOpacity(FSlateColor(VibeUEColors::TextSecondary))
							.DesiredSizeOverride(FVector2D(16, 16))
						]
					]
				]
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(8)
			[
				SNew(SBorder)
				.BorderBackgroundColor(VibeUEColors::BackgroundCard)
				.Padding(8)
				[
					SNew(SScrollBox)

					+ SScrollBox::Slot()
					.Padding(0, 0, 0, 8)
					[
						SNew(SBorder)
						.BorderBackgroundColor(VibeUEColors::BackgroundLight)
						.Padding(10)
						[
							SNew(SVerticalBox)

							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(SHorizontalBox)

								+ SHorizontalBox::Slot()
								.FillWidth(1.0f)
								.VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Text(FText::FromString(TEXT("Server")))
									.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
									.ColorAndOpacity(FSlateColor(VibeUEColors::TextPrimary))
								]

								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								[
									SNew(SButton)
									.Text(FText::FromString(TEXT("Copy URL")))
									.ToolTipText(FText::FromString(TEXT("Copy the local MCP URL for Codex/Cursor")))
									.OnClicked_Lambda([]() -> FReply
									{
										FPlatformApplicationMisc::ClipboardCopy(*FMCPServer::Get().GetServerUrl());
										return FReply::Handled();
									})
								]
							]

							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0, 8, 0, 0)
							[
								SNew(STextBlock)
								.Text(this, &SAIChatWindow::GetMCPDashboardServerSummaryText)
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
								.ColorAndOpacity(FSlateColor(VibeUEColors::TextSecondary))
								.AutoWrapText(true)
							]
						]
					]

					+ SScrollBox::Slot()
					.Padding(0, 0, 0, 8)
					[
						SNew(SBorder)
						.BorderBackgroundColor(VibeUEColors::BackgroundLight)
						.Padding(10)
						[
							SNew(SVerticalBox)

							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("Recent tools/call")))
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
								.ColorAndOpacity(FSlateColor(VibeUEColors::TextPrimary))
							]

							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0, 8, 0, 0)
							[
								SNew(SBox)
								.MinDesiredHeight(220.0f)
								[
									SAssignNew(ToolCallsTextBox, SMultiLineEditableTextBox)
									.IsReadOnly(true)
									.Text(this, &SAIChatWindow::GetMCPDashboardToolCallsText)
									.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
									.AutoWrapText(true)
								]
							]
						]
					]

					+ SScrollBox::Slot()
					.Padding(0, 0, 0, 0)
					[
						SNew(SBorder)
						.BorderBackgroundColor(VibeUEColors::BackgroundLight)
						.Padding(10)
						[
							SNew(SVerticalBox)

							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("Recent MCP events")))
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
								.ColorAndOpacity(FSlateColor(VibeUEColors::TextPrimary))
							]

							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0, 8, 0, 0)
							[
								SNew(SBox)
								.MinDesiredHeight(220.0f)
								[
									SAssignNew(RecentEventsTextBox, SMultiLineEditableTextBox)
									.IsReadOnly(true)
									.Text(this, &SAIChatWindow::GetMCPDashboardRecentEventsText)
									.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
									.AutoWrapText(true)
								]
							]
						]
					]
				]
			]
		]
	];
}

void SAIChatWindow::OpenWindow()
{
	if (WindowInstance.IsValid())
	{
		if (TSharedPtr<SWindow> Window = WindowInstance.Pin())
		{
			Window->BringToFront();
			return;
		}
	}

	WidgetInstance = SNew(SAIChatWindow);

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(TEXT("VibeUE MCP Dashboard")))
		.ClientSize(FVector2D(760, 760))
		.SupportsMinimize(true)
		.SupportsMaximize(true)
		[
			WidgetInstance.ToSharedRef()
		];

	WindowInstance = Window;
	Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([](const TSharedRef<SWindow>&)
	{
		WindowInstance.Reset();
		WidgetInstance.Reset();
	}));

	FSlateApplication::Get().AddWindow(Window);
}

void SAIChatWindow::CloseWindow()
{
	if (TSharedPtr<SWindow> Window = WindowInstance.Pin())
	{
		Window->RequestDestroyWindow();
	}
	WindowInstance.Reset();
	WidgetInstance.Reset();
}

void SAIChatWindow::ToggleWindow()
{
	if (IsWindowOpen())
	{
		CloseWindow();
		return;
	}
	OpenWindow();
}

bool SAIChatWindow::IsWindowOpen()
{
	return WindowInstance.IsValid();
}

void SAIChatWindow::CloseToolsPopup()
{
	if (TSharedPtr<SWindow> Window = ToolsPopupWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
	ToolsPopupWindow.Reset();
}

FReply SAIChatWindow::OnToolsClicked()
{
	CloseToolsPopup();

	FToolRegistry& Registry = FToolRegistry::Get();
	TArray<FToolMetadata> DashboardTools;
	for (const FToolMetadata& Tool : Registry.GetAllTools())
	{
		if (!Tool.bInternalOnly)
		{
			DashboardTools.Add(Tool);
		}
	}

	DashboardTools.Sort([](const FToolMetadata& A, const FToolMetadata& B)
	{
		if (A.Category == B.Category)
		{
			return A.Name < B.Name;
		}
		return A.Category < B.Category;
	});

	TSharedPtr<TMap<FString, bool>> PendingStates = MakeShared<TMap<FString, bool>>();
	for (const FToolMetadata& Tool : DashboardTools)
	{
		PendingStates->Add(Tool.Name, Registry.IsToolEnabled(Tool.Name));
	}

	TSharedRef<SVerticalBox> ToolsList = SNew(SVerticalBox);
	for (const FToolMetadata& Tool : DashboardTools)
	{
		const FString ToolName = Tool.Name;
		const bool bEnabled = Registry.IsToolEnabled(Tool.Name);

		ToolsList->AddSlot()
		.AutoHeight()
		.Padding(8, 4)
		[
			SNew(SBorder)
			.BorderBackgroundColor(bEnabled ? FLinearColor(0.10f, 0.12f, 0.14f, 1.0f) : FLinearColor(0.08f, 0.08f, 0.08f, 1.0f))
			.Padding(6)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0, 0, 8, 0)
				[
					SNew(SCheckBox)
					.IsChecked(bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
					.OnCheckStateChanged_Lambda([ToolName, PendingStates](ECheckBoxState NewState)
					{
						PendingStates->Add(ToolName, NewState == ECheckBoxState::Checked);
					})
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(FText::FromString(Tool.Name))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
						.ColorAndOpacity(FSlateColor(bEnabled ? VibeUEColors::TextPrimary : VibeUEColors::TextMuted))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0, 2, 0, 0)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Tool.Description))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor(VibeUEColors::TextMuted))
						.AutoWrapText(true)
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8, 0, 0, 0)
				[
					SNew(SBorder)
					.BorderBackgroundColor(FLinearColor(0.15f, 0.15f, 0.18f, 1.0f))
					.Padding(FMargin(6, 2))
					[
						SNew(STextBlock)
						.Text(FText::FromString(Tool.Category))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FSlateColor(VibeUEColors::TextSecondary))
					]
				]
			]
		];
	}

	TSharedRef<SWindow> PopupWindow = SNew(SWindow)
		.Title(FText::FromString(TEXT("VibeUE MCP Tools")))
		.ClientSize(FVector2D(560, 640))
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		.IsTopmostWindow(true);

	TWeakPtr<SWindow> WeakPopupWindow = PopupWindow;
	PopupWindow->SetContent(
		SNew(SBorder)
		.BorderBackgroundColor(VibeUEColors::Background)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("Expose %d tool(s) to external MCP clients"), DashboardTools.Num())))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
				.ColorAndOpacity(FSlateColor(VibeUEColors::TextPrimary))
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					ToolsList
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Disabled tools are hidden from `tools/list` and rejected by `tools/call`.")))
					.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
					.ColorAndOpacity(FSlateColor(VibeUEColors::TextMuted))
					.AutoWrapText(true)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8, 0, 0, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Save & Close")))
					.OnClicked_Lambda([PendingStates, WeakPopupWindow]() -> FReply
					{
						TSet<FString> DisabledTools;
						for (const TPair<FString, bool>& Pair : *PendingStates)
						{
							if (!Pair.Value)
							{
								DisabledTools.Add(Pair.Key);
							}
						}

						FToolRegistry::Get().SetDisabledToolsAndSave(DisabledTools);
						if (TSharedPtr<SWindow> Window = WeakPopupWindow.Pin())
						{
							Window->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8, 0, 0, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Cancel")))
					.OnClicked_Lambda([WeakPopupWindow]() -> FReply
					{
						if (TSharedPtr<SWindow> Window = WeakPopupWindow.Pin())
						{
							Window->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
			]
		]
	);

	FSlateApplication::Get().AddWindow(PopupWindow);
	ToolsPopupWindow = PopupWindow;
	return FReply::Handled();
}

FReply SAIChatWindow::OnSettingsClicked()
{
	TSharedRef<SWindow> SettingsWindow = SNew(SWindow)
		.Title(FText::FromString(TEXT("VibeUE MCP Settings")))
		.ClientSize(FVector2D(520, 520))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	const bool bMCPServerEnabled = FMCPServer::GetEnabledFromConfig();
	const int32 MCPServerPort = FMCPServer::GetPortFromConfig();
	const int32 MCPServerRequestReadTimeoutMs = FMCPServer::GetRequestReadTimeoutMsFromConfig();
	const int32 MCPServerToolExecutionTimeoutMs = FMCPServer::GetToolExecutionTimeoutMsFromConfig();
	const FString MCPServerApiKey = FMCPServer::GetApiKeyFromConfig();

	TSharedPtr<SCheckBox> MCPServerEnabledCheckBox;
	TSharedPtr<SSpinBox<int32>> MCPServerPortSpinBox;
	TSharedPtr<SSpinBox<int32>> MCPServerRequestReadTimeoutSpinBox;
	TSharedPtr<SSpinBox<int32>> MCPServerToolExecutionTimeoutSpinBox;
	TSharedPtr<SEditableTextBox> MCPServerApiKeyInput;

	SettingsWindow->SetContent(
		SNew(SBorder)
		.BorderBackgroundColor(VibeUEColors::Background)
		.Padding(12)
		[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 0, 0, 12)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Local MCP Server")))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					.ColorAndOpacity(FSlateColor(VibeUEColors::TextPrimary))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 0, 0, 8)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SAssignNew(MCPServerEnabledCheckBox, SCheckBox)
						.IsChecked(bMCPServerEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.Padding(6, 0, 0, 0)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Enable MCP Server")))
						.ColorAndOpacity(FSlateColor(VibeUEColors::TextPrimary))
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 0, 0, 8)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(0.4f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Port")))
						.ColorAndOpacity(FSlateColor(VibeUEColors::TextSecondary))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(0.6f)
					[
						SAssignNew(MCPServerPortSpinBox, SSpinBox<int32>)
						.MinValue(1024)
						.MaxValue(65535)
						.Value(MCPServerPort)
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 0, 0, 8)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(0.4f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Request Read Timeout (ms)")))
						.ColorAndOpacity(FSlateColor(VibeUEColors::TextSecondary))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(0.6f)
					[
						SAssignNew(MCPServerRequestReadTimeoutSpinBox, SSpinBox<int32>)
						.MinValue(1000)
						.MaxValue(300000)
						.Value(MCPServerRequestReadTimeoutMs)
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 0, 0, 8)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(0.4f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Tool Execution Timeout (ms)")))
						.ColorAndOpacity(FSlateColor(VibeUEColors::TextSecondary))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(0.6f)
					[
						SAssignNew(MCPServerToolExecutionTimeoutSpinBox, SSpinBox<int32>)
						.MinValue(1000)
						.MaxValue(3600000)
						.Value(MCPServerToolExecutionTimeoutMs)
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Bearer Token (optional)")))
					.ColorAndOpacity(FSlateColor(VibeUEColors::TextSecondary))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 4, 0, 8)
				[
					SAssignNew(MCPServerApiKeyInput, SEditableTextBox)
					.Text(FText::FromString(MCPServerApiKey))
					.IsPassword(true)
					.HintText(FText::FromString(TEXT("Leave empty for unauthenticated access")))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 4, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Logging uses [Core.Log] LogMCPServer in Unreal ini files.")))
					.ColorAndOpacity(FSlateColor(VibeUEColors::TextMuted))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 4, 0, 0)
				[
					SNew(STextBlock)
					.Text_Lambda([]() -> FText
					{
						const FMCPServer& Server = FMCPServer::Get();
						if (!Server.IsRunning())
						{
							return FText::FromString(TEXT("Status: stopped"));
						}
						return FText::FromString(FString::Printf(TEXT("Status: listening at %s"), *Server.GetServerUrl()));
					})
					.ColorAndOpacity_Lambda([]() -> FSlateColor
					{
						return FMCPServer::Get().IsRunning() ? FSlateColor(VibeUEColors::Green) : FSlateColor(VibeUEColors::TextMuted);
					})
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 12, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Plugin Install Path")))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(FSlateColor(VibeUEColors::TextPrimary))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 4, 0, 0)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FPaths::ConvertRelativePathToFull(FVibeUEPaths::GetPluginDir())))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor(VibeUEColors::TextMuted))
						.AutoWrapText(true)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(8, 0, 0, 0)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Open Folder")))
						.OnClicked_Lambda([]() -> FReply
						{
							FPlatformProcess::ExploreFolder(*FPaths::ConvertRelativePathToFull(FVibeUEPaths::GetPluginDir()));
							return FReply::Handled();
						})
					]
				]

				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SSpacer)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Save")))
						.OnClicked_Lambda([MCPServerEnabledCheckBox,
						                   MCPServerPortSpinBox,
						                   MCPServerRequestReadTimeoutSpinBox,
						                   MCPServerToolExecutionTimeoutSpinBox,
						                   MCPServerApiKeyInput,
						                   SettingsWindow]() -> FReply
						{
							const bool bOldEnabled = FMCPServer::GetEnabledFromConfig();
							const int32 OldPort = FMCPServer::GetPortFromConfig();
							const int32 OldRequestReadTimeoutMs = FMCPServer::GetRequestReadTimeoutMsFromConfig();
							const int32 OldToolExecutionTimeoutMs = FMCPServer::GetToolExecutionTimeoutMsFromConfig();
							const FString OldApiKey = FMCPServer::GetApiKeyFromConfig();

							const bool bNewEnabled = MCPServerEnabledCheckBox->IsChecked();
							const int32 NewPort = MCPServerPortSpinBox->GetValue();
							const int32 NewRequestReadTimeoutMs = MCPServerRequestReadTimeoutSpinBox->GetValue();
							const int32 NewToolExecutionTimeoutMs = MCPServerToolExecutionTimeoutSpinBox->GetValue();
							const FString NewApiKey = MCPServerApiKeyInput->GetText().ToString();

							const bool bChanged = bOldEnabled != bNewEnabled ||
								OldPort != NewPort ||
								OldRequestReadTimeoutMs != NewRequestReadTimeoutMs ||
								OldToolExecutionTimeoutMs != NewToolExecutionTimeoutMs ||
								OldApiKey != NewApiKey;

							FMCPServer::SaveEnabledToConfig(bNewEnabled);
							FMCPServer::SavePortToConfig(NewPort);
							FMCPServer::SaveRequestReadTimeoutMsToConfig(NewRequestReadTimeoutMs);
							FMCPServer::SaveToolExecutionTimeoutMsToConfig(NewToolExecutionTimeoutMs);
							FMCPServer::SaveApiKeyToConfig(NewApiKey);

							FMCPServer& Server = FMCPServer::Get();
							if (bChanged)
							{
								if (Server.IsRunning())
								{
									Server.StopServer();
								}
								Server.LoadConfig();
								if (bNewEnabled)
								{
									Server.Start();
								}
							}

							SettingsWindow->RequestDestroyWindow();
							return FReply::Handled();
						})
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(8, 0, 0, 0)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Cancel")))
						.OnClicked_Lambda([SettingsWindow]() -> FReply
						{
							SettingsWindow->RequestDestroyWindow();
							return FReply::Handled();
						})
					]
				]
			]
		);

	FSlateApplication::Get().AddWindow(SettingsWindow);
	return FReply::Handled();
}

FText SAIChatWindow::GetMCPStatusText() const
{
	const FMCPServerDiagnosticsSnapshot Snapshot = FMCPServer::Get().GetDiagnosticsSnapshot(25);
	if (!Snapshot.bIsRunning)
	{
		return FText::FromString(TEXT("Server stopped"));
	}

	int32 RunningCount = 0;
	int32 SuccessCount = 0;
	int32 FailedCount = 0;
	CountToolCallStates(Snapshot.RecentToolCalls, RunningCount, SuccessCount, FailedCount);

	const int32 ClientCount = GetActiveClientCount(Snapshot);
	if (RunningCount > 0)
	{
		return FText::FromString(FString::Printf(TEXT("Clients:%d | running:%d"), ClientCount, RunningCount));
	}
	if (FailedCount > 0 && !Snapshot.LastToolsCallToolName.IsEmpty() && Snapshot.bLastToolsCallIsError)
	{
		return FText::FromString(FString::Printf(TEXT("Clients:%d | last failed:%s"), ClientCount, *Snapshot.LastToolsCallToolName));
	}
	if (SuccessCount > 0 && !Snapshot.LastToolsCallToolName.IsEmpty())
	{
		return FText::FromString(FString::Printf(TEXT("Clients:%d | last ok:%s"), ClientCount, *Snapshot.LastToolsCallToolName));
	}
	if (Snapshot.TotalRpcRequests > 0)
	{
		return FText::FromString(FString::Printf(TEXT("Clients:%d | idle"), ClientCount));
	}
	return FText::FromString(TEXT("Waiting for client traffic"));
}

FText SAIChatWindow::GetMCPStatusTooltipText() const
{
	const FMCPServerDiagnosticsSnapshot Snapshot = FMCPServer::Get().GetDiagnosticsSnapshot(12);
	int32 RunningCount = 0;
	int32 SuccessCount = 0;
	int32 FailedCount = 0;
	CountToolCallStates(Snapshot.RecentToolCalls, RunningCount, SuccessCount, FailedCount);

	FString Text;
	Text += FString::Printf(TEXT("URL: %s\n"), *Snapshot.ServerUrl);
	Text += FString::Printf(TEXT("Running: %s\n"), Snapshot.bIsRunning ? TEXT("Yes") : TEXT("No"));
	Text += FString::Printf(TEXT("Clients: %d active | SSE=%d | sessions=%d\n"), GetActiveClientCount(Snapshot), Snapshot.ActiveSSEConnections, Snapshot.ActiveSessions);
	Text += FString::Printf(TEXT("Tool calls: running=%d ok=%d failed=%d\n"), RunningCount, SuccessCount, FailedCount);
	Text += FString::Printf(TEXT("RPC total: %lld\n"), Snapshot.TotalRpcRequests);

	const int32 RpcAgo = SecondsAgo(Snapshot.LastRpcUtc);
	if (RpcAgo >= 0 && !Snapshot.LastRpcMethod.IsEmpty())
	{
		Text += FString::Printf(TEXT("Last RPC: %s (%ds ago)\n"), *Snapshot.LastRpcMethod, RpcAgo);
	}

	const int32 CallAgo = SecondsAgo(Snapshot.LastToolsCallUtc);
	if (CallAgo >= 0 && !Snapshot.LastToolsCallToolName.IsEmpty())
	{
		Text += FString::Printf(TEXT("Last tools/call: %s (%s, %ds ago)"), *Snapshot.LastToolsCallToolName, Snapshot.bLastToolsCallIsError ? TEXT("error") : TEXT("ok"), CallAgo);
	}

	return FText::FromString(Text);
}

FSlateColor SAIChatWindow::GetMCPStatusColor() const
{
	const FMCPServerDiagnosticsSnapshot Snapshot = FMCPServer::Get().GetDiagnosticsSnapshot(25);
	if (!Snapshot.bIsRunning)
	{
		return FSlateColor(VibeUEColors::Red);
	}

	for (const FMCPToolCallRecord& Record : Snapshot.RecentToolCalls)
	{
		if (Record.bInProgress)
		{
			return FSlateColor(VibeUEColors::Orange);
		}
	}

	if (Snapshot.bLastToolsCallIsError)
	{
		return FSlateColor(VibeUEColors::Red);
	}
	if (GetActiveClientCount(Snapshot) > 0 || Snapshot.TotalRpcRequests > 0)
	{
		return FSlateColor(VibeUEColors::Green);
	}
	return FSlateColor(VibeUEColors::Cyan);
}

FText SAIChatWindow::GetMCPDashboardToolsCountText() const
{
	const FToolRegistry& Registry = FToolRegistry::Get();
	if (!Registry.IsInitialized())
	{
		return FText::FromString(TEXT("Tools: --"));
	}

	int32 Count = 0;
	for (const FToolMetadata& Tool : Registry.GetEnabledTools())
	{
		if (!Tool.bInternalOnly)
		{
			++Count;
		}
	}

	return FText::FromString(FString::Printf(TEXT("Tools: %d"), Count));
}

FText SAIChatWindow::GetMCPDashboardServerSummaryText() const
{
	const FMCPServerDiagnosticsSnapshot Snapshot = FMCPServer::Get().GetDiagnosticsSnapshot(10);

	FString Text;
	Text += FString::Printf(TEXT("URL: %s\n"), *Snapshot.ServerUrl);
	Text += FString::Printf(TEXT("State: %s | Port: %d\n"), Snapshot.bIsRunning ? TEXT("listening") : TEXT("stopped"), Snapshot.Port);
	Text += FString::Printf(TEXT("Clients: active=%d | SSE=%d | sessions=%d | accepted=%lld\n"),
		GetActiveClientCount(Snapshot),
		Snapshot.ActiveSSEConnections,
		Snapshot.ActiveSessions,
		Snapshot.TotalConnectionsAccepted);

	if (Snapshot.TotalRpcRequests == 0)
	{
		Text += TEXT("\nHint: point Codex/Cursor at the URL above, then run `self_test`, `project_info`, or `editor_context` to confirm connectivity.");
	}

	return FText::FromString(Text);
}

FText SAIChatWindow::GetMCPDashboardToolCallsText() const
{
	const FMCPServerDiagnosticsSnapshot Snapshot = FMCPServer::Get().GetDiagnosticsSnapshot(64);
	if (Snapshot.RecentToolCalls.Num() == 0)
	{
		return FText::FromString(TEXT("No tools/call yet.\nConnect Codex/Cursor, then run `self_test`, `project_info`, or `editor_context` to populate this panel."));
	}

	int32 RunningCount = 0;
	int32 SuccessCount = 0;
	int32 FailedCount = 0;
	CountToolCallStates(Snapshot.RecentToolCalls, RunningCount, SuccessCount, FailedCount);

	TArray<FString> Lines;
	Lines.Reserve(256);
	Lines.Add(FString::Printf(TEXT("Summary: running=%d | ok=%d | failed=%d"), RunningCount, SuccessCount, FailedCount));
	Lines.Add(TEXT(""));

	constexpr int32 MaxRecordsToShow = 20;
	int32 Shown = 0;
	for (int32 Index = Snapshot.RecentToolCalls.Num() - 1; Index >= 0 && Shown < MaxRecordsToShow; --Index, ++Shown)
	{
		const FMCPToolCallRecord& Record = Snapshot.RecentToolCalls[Index];
		const FString Status = Record.bInProgress ? TEXT("running") : (Record.bIsError ? TEXT("error") : TEXT("ok"));
		const FString Duration = (!Record.bInProgress && Record.DurationMs > 0.0)
			? FString::Printf(TEXT(" | %.0fms"), Record.DurationMs)
			: FString();
		const FString ToolLabel = FormatToolRecordLabel(Record);

		Lines.Add(FString::Printf(TEXT("#%lld %s — %s%s"), Record.Sequence, *ToolLabel, *Status, *Duration));

		if (!Record.Summary.IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("  summary: %s"), *Record.Summary));
		}

		if (!Record.RequestId.IsEmpty() || !Record.SessionId.IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("  request=%s | session=%s"),
				Record.RequestId.IsEmpty() ? TEXT("-") : *Record.RequestId,
				Record.SessionId.IsEmpty() ? TEXT("-") : *Record.SessionId));
		}

		TArray<FString> ImpactBits;
		if (Record.NewDirtyPackageCount > 0)
		{
			ImpactBits.Add(FString::Printf(TEXT("dirty packages=%d"), Record.NewDirtyPackageCount));
		}
		if (Record.ActorCountDelta != 0)
		{
			ImpactBits.Add(FString::Printf(TEXT("actors=%+d"), Record.ActorCountDelta));
		}
		if (ImpactBits.Num() == 0)
		{
			ImpactBits.Add((Record.ChangedItems.Num() > 0 || Record.AffectedAssets.Num() > 0)
				? TEXT("structured change reported")
				: TEXT("no editor-side asset delta detected"));
		}
		Lines.Add(FString::Printf(TEXT("  impact: %s"), *FString::Join(ImpactBits, TEXT(" | "))));

		if (Record.ChangedItems.Num() > 0)
		{
			Lines.Add(FString::Printf(TEXT("  changed: %s"), *JoinLimitedList(Record.ChangedItems)));
		}

		if (Record.AffectedAssets.Num() > 0)
		{
			TArray<FString> DescribedAssets;
			DescribedAssets.Reserve(Record.AffectedAssets.Num());
			for (const FString& AssetPath : Record.AffectedAssets)
			{
				DescribedAssets.Add(DescribeAffectedAsset(AssetPath));
			}
			Lines.Add(FString::Printf(TEXT("  assets: %s"), *JoinLimitedList(DescribedAssets)));
		}

		if (Record.NewDirtyPackages.Num() > 0)
		{
			TArray<FString> DescribedPackages;
			for (const FString& PackageName : Record.NewDirtyPackages)
			{
				DescribedPackages.Add(DescribePackage(PackageName));
			}

			const int32 ExtraCount = FMath::Max(0, Record.NewDirtyPackageCount - Record.NewDirtyPackages.Num());
			FString AssetLine = FString::Join(DescribedPackages, TEXT("; "));
			if (ExtraCount > 0)
			{
				AssetLine += FString::Printf(TEXT("; +%d more"), ExtraCount);
			}
			Lines.Add(FString::Printf(TEXT("  dirty: %s"), *AssetLine));
		}

		if (Record.Warnings.Num() > 0)
		{
			Lines.Add(FString::Printf(TEXT("  warnings: %s"), *JoinLimitedList(Record.Warnings)));
		}

		const TArray<FString> Hints = BuildVerificationHints(Record);
		if (Hints.Num() > 0)
		{
			Lines.Add(FString::Printf(TEXT("  verify: %s"), *FString::Join(Hints, TEXT(" | "))));
		}

		if (Record.bIsError && !Record.ErrorSummary.IsEmpty())
		{
			const FString ErrorPrefix = Record.ErrorCode.IsEmpty()
				? TEXT("")
				: FString::Printf(TEXT("[%s] "), *Record.ErrorCode);
			Lines.Add(FString::Printf(TEXT("  error: %s%s"), *ErrorPrefix, *Record.ErrorSummary));

			const FString FriendlyHint = BuildFriendlyErrorHint(Record);
			if (!FriendlyHint.IsEmpty())
			{
				Lines.Add(FString::Printf(TEXT("  hint: %s"), *FriendlyHint));
			}
		}

		Lines.Add(TEXT(""));
	}

	return FText::FromString(FString::Join(Lines, TEXT("\n")));
}

FText SAIChatWindow::GetMCPDashboardRecentEventsText() const
{
	const FMCPServerDiagnosticsSnapshot Snapshot = FMCPServer::Get().GetDiagnosticsSnapshot(200);
	if (Snapshot.RecentEvents.Num() == 0)
	{
		return FText::FromString(TEXT("No MCP activity yet."));
	}

	TArray<FString> Lines;
	constexpr int32 MaxLinesToShow = 120;
	Lines.Reserve(FMath::Min(MaxLinesToShow, Snapshot.RecentEvents.Num()));

	for (int32 Index = Snapshot.RecentEvents.Num() - 1, Shown = 0; Index >= 0 && Shown < MaxLinesToShow; --Index, ++Shown)
	{
		Lines.Add(Snapshot.RecentEvents[Index]);
	}

	return FText::FromString(FString::Join(Lines, TEXT("\n")));
}
